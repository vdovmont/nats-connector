#include "http_handler.h"

#include <Poco/URI.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>

#include "logger.h"
#include "nats_manager.h"

inline std::string ToString(Status s) {
    switch (s) {
        case Status::Error: return "Error";
        case Status::Ok: return "Ok";
        default: return "Undefined";
    }
}

int FileRequestHandler::query_number_ = 0;
std::unordered_map<std::string, int> FileRequestHandler::id_query_map_;
std::unordered_map<std::string, int> FileRequestHandler::persisted_id_query_map_;
std::mutex FileRequestHandler::state_mutex_;
bool FileRequestHandler::state_loaded_ = false;
const std::string FileRequestHandler::kStateFilePath = "query_state.json";
std::atomic<bool> FileRequestHandler::mathcore_alive_{true};
std::atomic<uint64_t> FileRequestHandler::mathcore_startup_epoch_{0};
std::chrono::steady_clock::time_point FileRequestHandler::last_mathcore_heartbeat_ = std::chrono::steady_clock::now();
std::mutex FileRequestHandler::health_mutex_;
bool FileRequestHandler::mathcore_subscription_active_ = false;
const std::chrono::seconds FileRequestHandler::kMathAliveTimeout(60);
const std::string FileRequestHandler::kMathAliveSubject = "IsMathAlive.*";

bool FileRequestHandler::StartMathAliveWatcher(NatsManager& nats_manager) {
    std::lock_guard<std::mutex> lock(health_mutex_);
    if (mathcore_subscription_active_) {
        return true;
    }

    // Consider MathCore healthy until we miss the first heartbeat window.
    last_mathcore_heartbeat_ = std::chrono::steady_clock::now();
    mathcore_alive_.store(true, std::memory_order_relaxed);
    mathcore_subscription_active_ =
        nats_manager.Subscribe(kMathAliveSubject, [](const std::string&, const nlohmann::json& message) {
            FileRequestHandler::RecordMathCoreHeartbeat(message);
        });

    if (!mathcore_subscription_active_) {
        mathcore_alive_.store(false, std::memory_order_relaxed);
        logger::log_error() << "Failed to subscribe to MathCore heartbeat subject: " << kMathAliveSubject << std::endl;
    }

    return mathcore_subscription_active_;
}

bool FileRequestHandler::IsMathCoreAlive() {
    std::lock_guard<std::mutex> lock(health_mutex_);
    auto now = std::chrono::steady_clock::now();
    if (now - last_mathcore_heartbeat_ > kMathAliveTimeout) {
        bool was_alive = mathcore_alive_.exchange(false, std::memory_order_relaxed);
        if (was_alive) {
            logger::log_error() << "MathCore heartbeat timeout" << std::endl;
        }
    }
    return mathcore_alive_.load(std::memory_order_relaxed);
}

void FileRequestHandler::RecordMathCoreHeartbeat(const nlohmann::json& payload) {
    bool is_startup = false;
    bool was_alive = true;
    if (payload.contains("event") && payload["event"].is_string()) {
        is_startup = (payload["event"].get<std::string>() == "startup");
    }

    // strange block because of lock_guard scope (inside we're holding health_mutex_ and outside we're not)
    {
        std::lock_guard<std::mutex> lock(health_mutex_);
        last_mathcore_heartbeat_ = std::chrono::steady_clock::now();
        was_alive = mathcore_alive_.exchange(true, std::memory_order_relaxed);
        if (is_startup) {
            mathcore_startup_epoch_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (is_startup) {
        logger::log() << "MathCore startup heartbeat received" << std::endl;
        HandleMathCoreStartup();
    } else if (!was_alive) {
        logger::log() << "MathCore heartbeat received after timeout" << std::endl;
    }
}

void FileRequestHandler::HandleMathCoreStartup() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    id_query_map_.clear();
    query_number_ = 0;
    state_loaded_ = true;
    PersistStateLocked();
}

std::string FileRequestHandler::GenerateID() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000;

    // Format: YYYYMMDD_HHMMSS
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t_now), "%Y%m%d_%H%M%S");

    return oss.str();
}

std::string FileRequestHandler::GetID(int Query) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    EnsureStateLoadedLocked();
    for (const auto& pair : id_query_map_) {
        if (pair.second == Query) {
            return pair.first;
        }
    }
    return "";
}

int FileRequestHandler::ParseQuery(std::string& uri) {
    Poco::URI parsedUri(uri);
    Poco::URI::QueryParameters params = parsedUri.getQueryParameters();

    int Query = 0;
    for (const auto& p : params) {
        if (p.first == "numTicket" || p.first == "num") {
            try {
                Query = std::stoi(p.second);
            } catch (const std::exception& e) {
                Query = 0;
            }
            break;
        }
    }
    return Query;
}

std::string FileRequestHandler::ParseLogId(const std::string& uri) {
    Poco::URI parsedUri(uri);
    Poco::URI::QueryParameters params = parsedUri.getQueryParameters();

    for (const auto& p : params) {
        if (p.first == "id") {
            return p.second;
        }
    }

    return "";
}

int FileRequestHandler::NextQuery(const std::string& ID) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    EnsureStateLoadedLocked();
    ++query_number_;
    id_query_map_[ID] = query_number_;
    persisted_id_query_map_[ID] = query_number_;
    PersistStateLocked();
    return query_number_;
}

void FileRequestHandler::WaitForResponse(uint64_t startup_epoch,
                                         const std::string& response_subject,
                                         const std::string& request_name,
                                         std::future<nlohmann::json>& future,
                                         const std::function<nlohmann::json(const std::string&)>& make_error,
                                         const std::function<void()>& on_restart_cleanup,
                                         nlohmann::json& response_json) {
    bool done = false;
    while (!done) {
        if (startup_epoch != mathcore_startup_epoch_.load(std::memory_order_relaxed)) {
            nats_manager_.Unsubscribe(response_subject);
            response_json = make_error("MathCore was restarted");
            if (on_restart_cleanup) {
                on_restart_cleanup();
            }
            logger::log_error() << "MathCore restarted while waiting for " << request_name << " response" << std::endl;
            done = true;
            break;
        }

        if (!IsMathCoreAlive()) {
            nats_manager_.Unsubscribe(response_subject);
            response_json = make_error("MathCore is unavailable");
            logger::log_error() << "MathCore unavailable while waiting for " << request_name << " response"
                                << std::endl;
            done = true;
            break;
        }

        auto status = future.wait_for(std::chrono::seconds(1));
        if (status == std::future_status::ready) {
            response_json = future.get();
            logger::log() << "Received MathCore response for " << request_name << std::endl;
            done = true;
        }
    }
}

void FileRequestHandler::handleRequest(Poco::Net::HTTPServerRequest& request, Poco::Net::HTTPServerResponse& response) {
    response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
    response.setContentType("application/json");

    std::ostream& ostr = response.send();
    std::string uri = request.getURI();
    nlohmann::json errorJson;

    if (uri.find("/start") == 0) {
        HandleStart(request, ostr);
    } else if (uri.find("/state") == 0) {
        int Query = ParseQuery(uri);
        if (Query == 0) {
            errorJson["error"] = "invalid or missing query number";
            ostr << errorJson.dump();
        } else {
            HandleState(ostr, Query);
        }
    } else if (uri.find("/logslist") == 0 || uri.find("/loglist") == 0) {
        HandleLogsList(ostr);
    } else if (uri.find("/getlog") == 0) {
        std::string id = ParseLogId(uri);
        if (id.empty()) {
            errorJson["error"] = "invalid or missing id";
            ostr << errorJson.dump();
        } else {
            HandleGetLog(ostr, id);
        }
    } else {
        nlohmann::json errorJson;
        errorJson["error"] = "unknown command";
        ostr << errorJson.dump();
    }
}

void FileRequestHandler::HandleStart(Poco::Net::HTTPServerRequest& request, std::ostream& ostr) {
    std::ostringstream body;
    std::istream& stream = request.stream();
    body << stream.rdbuf();
    nlohmann::json responseJson;

    if (!IsMathCoreAlive()) {
        responseJson = GenerateErrorResponse(0, "MathCore is unavailable");
        logger::log_error() << "Received Start request while MathCore is unavailable" << std::endl;
    } else if (!body.str().empty()) {
        std::string ID = GenerateID();
        int Query = NextQuery(ID);
        std::string start_subject = "Start.";
        start_subject += ID;
        logger::log() << "Received Start request with ID=" << ID << " (query=" << Query << ")" << std::endl;

        nlohmann::json message = nlohmann::json::parse(body.str());
        bool published = nats_manager_.Publish(start_subject, message);

        if (published) {
            responseJson = GenerateResponse(Query, ID, Status::Ok, "BUFFERED");
        } else {
            std::lock_guard<std::mutex> lock(state_mutex_);
            RemovePairLocked(ID);
            responseJson = GenerateResponse(Query, ID, Status::Error, "Failed to publish message to NATS");
            logger::log_error() << "Failed to publish Start request with ID=" << ID << std::endl;
        }
    } else {
        responseJson["error"] = "Message is empty";
        logger::log_error() << "Received Start request with empty body" << std::endl;
    }

    logger::log() << "Sent Start response" << std::endl;
    ostr << responseJson.dump();
}

void FileRequestHandler::HandleState(std::ostream& ostr, int Query) {
    std::string ID = GetID(Query);
    nlohmann::json responseJson;

    if (ID.empty()) {
        responseJson =
            GenerateResponse(Query, ID, Status::Error, "Wrong query number (either not found or not generated yet)");
        logger::log_error() << "Received State request with invalid query=" << Query << std::endl;
        ostr << responseJson.dump();
        return;
    }

    std::string state_request_subject = "State.Request." + ID;
    std::string state_response_subject = "State.Response." + ID;
    uint64_t startup_epoch = mathcore_startup_epoch_.load(std::memory_order_relaxed);
    logger::log() << "Received State request with ID=" << ID << " (query=" << Query << ")" << std::endl;

    if (!IsMathCoreAlive()) {
        responseJson = GenerateResponse(Query, ID, Status::Error, "MathCore is unavailable");
        std::lock_guard<std::mutex> lock(state_mutex_);
        EnsureStateLoadedLocked();
        logger::log_error() << "MathCore unavailable for State request ID=" << ID << std::endl;
        ostr << responseJson.dump();
        return;
    }

    std::promise<nlohmann::json> promise;
    auto future = promise.get_future();

    auto sub = nats_manager_.Subscribe(
        state_response_subject,
        [this, &promise, Query](const std::string& msg_subject, const nlohmann::json& message) mutable {
            nlohmann::json state;
            this->OnMessageState(msg_subject, message, state, Query);
            promise.set_value(std::move(state));
            nats_manager_.Unsubscribe(msg_subject);  // unsubscribe right after we get our message
        });

    if (!sub) {
        responseJson = GenerateResponse(Query, ID, Status::Error, "Failed to subscribe to NATS subject");
    } else {
        nlohmann::json request = {{"id", ID}};
        if (!nats_manager_.Publish(state_request_subject, request)) {
            nats_manager_.Unsubscribe(state_response_subject);
            responseJson = GenerateResponse(Query, ID, Status::Error, "Failed to publish message to NATS");
        } else {
            auto make_error = [Query, &ID, this](const std::string& message) {
                return GenerateResponse(Query, ID, Status::Error, message);
            };
            auto on_restart_cleanup = [this, &ID]() {
                std::lock_guard<std::mutex> lock(state_mutex_);
                EnsureStateLoadedLocked();
                RemovePairLocked(ID);
            };
            logger::log() << "Waiting for MathCore response to State request ID=" << ID << std::endl;
            WaitForResponse(startup_epoch,
                            state_response_subject,
                            "State request ID=" + ID,
                            future,
                            make_error,
                            on_restart_cleanup,
                            responseJson);
        }
    }

    logger::log() << "Sent State response for ID=" << ID << std::endl;
    ostr << responseJson.dump();
}

void FileRequestHandler::HandleLogsList(std::ostream& ostr) {
    nlohmann::json responseJson;
    uint64_t startup_epoch = mathcore_startup_epoch_.load(std::memory_order_relaxed);
    const std::string request_subject = "LogsList.Request";
    const std::string response_subject = "LogsList.Response";
    logger::log() << "Received LogsList request" << std::endl;

    if (!IsMathCoreAlive()) {
        responseJson = GenerateErrorResponse(0, "MathCore is unavailable");
        logger::log_error() << "MathCore unavailable for LogsList request" << std::endl;
        ostr << responseJson.dump();
        return;
    }

    std::promise<nlohmann::json> promise;
    auto future = promise.get_future();

    auto sub = nats_manager_.Subscribe(
        response_subject, [&promise, this](const std::string& msg_subject, const nlohmann::json& message) mutable {
            promise.set_value(message);
            nats_manager_.Unsubscribe(msg_subject);  // unsubscribe right after we get our message
        });

    if (!sub) {
        responseJson = GenerateErrorResponse(0, "Failed to subscribe to NATS subject");
    } else if (!nats_manager_.Publish(request_subject, nlohmann::json::object())) {
        nats_manager_.Unsubscribe(response_subject);
        responseJson = GenerateErrorResponse(0, "Failed to publish message to NATS");
    } else {
        auto make_error = [this](const std::string& message) {
            return GenerateErrorResponse(0, message);
        };
        logger::log() << "Waiting for MathCore response to LogsList request" << std::endl;
        WaitForResponse(startup_epoch, response_subject, "LogsList request", future, make_error, nullptr, responseJson);
    }

    logger::log() << "Sent LogsList response" << std::endl;
    ostr << responseJson.dump();
}

void FileRequestHandler::HandleGetLog(std::ostream& ostr, const std::string& id) {
    nlohmann::json responseJson;
    uint64_t startup_epoch = mathcore_startup_epoch_.load(std::memory_order_relaxed);
    const std::string request_subject = "GetLog.Request." + id;
    const std::string response_subject = "GetLog.Response." + id;
    logger::log() << "Received GetLog request with ID=" << id << std::endl;

    if (!IsMathCoreAlive()) {
        responseJson = GenerateErrorResponse(0, "MathCore is unavailable");
        logger::log_error() << "MathCore unavailable for GetLog request ID=" << id << std::endl;
        ostr << responseJson.dump();
        return;
    }

    std::promise<nlohmann::json> promise;
    auto future = promise.get_future();

    auto sub = nats_manager_.Subscribe(
        response_subject, [&promise, this](const std::string& msg_subject, const nlohmann::json& message) mutable {
            promise.set_value(message);
            nats_manager_.Unsubscribe(msg_subject);  // unsubscribe right after we get our message
        });

    if (!sub) {
        responseJson = GenerateErrorResponse(0, "Failed to subscribe to NATS subject");
    } else {
        nlohmann::json request = {{"id", id}};
        if (!nats_manager_.Publish(request_subject, request)) {
            nats_manager_.Unsubscribe(response_subject);
            responseJson = GenerateErrorResponse(0, "Failed to publish message to NATS");
        } else {
            auto make_error = [this](const std::string& message) {
                return GenerateErrorResponse(0, message);
            };
            logger::log() << "Waiting for MathCore response to GetLog request ID=" << id << std::endl;
            WaitForResponse(
                startup_epoch, response_subject, "GetLog request ID=" + id, future, make_error, nullptr, responseJson);
        }
    }

    logger::log() << "Sent GetLog response for ID=" << id << std::endl;
    ostr << responseJson.dump();
}

nlohmann::json FileRequestHandler::GenerateResponse(const int query,
                                                    const std::string& ID = "null",
                                                    const enum Status status = Status::Ok,
                                                    const std::string& desc = "BUFFERED") {
    nlohmann::json response;
    response["query"] = query;
    response["globalID"] = ID;
    response["status"] = status;
    response["desc"] = desc;
    response["solnumbs"] = 0;
    response["time"] = 0;
    return response;
}

nlohmann::json FileRequestHandler::GenerateErrorResponse(const int query, const std::string& desc) {
    return GenerateResponse(query, "null", Status::Error, desc);
}

void FileRequestHandler::OnMessageState(const std::string& msg_subject,
                                        const nlohmann::json& message,
                                        nlohmann::json& state,
                                        const int Query) {
    std::string ID = GetID(Query);
    if (message.contains("message") || message.contains("error")) {
        state["solutions"] = nlohmann::json::array();
        std::string desc;
        if (message.contains("message")) {
            desc = message["message"];
            state["state"] = GenerateResponse(Query, ID, Status::Ok, desc);
        } else {
            desc = message["error"];
            state["state"] = GenerateResponse(Query, ID, Status::Error, desc);
        }
    } else {
        state = message;
        state["state"]["query"] = Query;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    EnsureStateLoadedLocked();
    RemovePersistedPairLocked(ID);
}

void FileRequestHandler::EnsureStateLoadedLocked() {
    if (state_loaded_) {
        return;
    }

    std::ifstream input(kStateFilePath);
    if (input.is_open()) {
        try {
            nlohmann::json persisted;
            input >> persisted;
            if (persisted.is_array()) {
                for (const auto& entry : persisted) {
                    if (entry.contains("id") && entry.contains("query")) {
                        std::string id = entry["id"].get<std::string>();
                        int query = entry["query"].get<int>();
                        id_query_map_[id] = query;
                        persisted_id_query_map_[id] = query;
                        if (query > query_number_) {
                            query_number_ = query;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            logger::log_error() << "Failed to load persisted query state: " << e.what() << std::endl;
        }
    }
    state_loaded_ = true;
}

void FileRequestHandler::PersistStateLocked() {
    nlohmann::json persisted = nlohmann::json::array();
    for (const auto& entry : persisted_id_query_map_) {
        persisted.push_back({{"id", entry.first}, {"query", entry.second}});
    }

    std::ofstream output(kStateFilePath, std::ios::trunc);
    if (!output.is_open()) {
        logger::log_error() << "Failed to open state file for writing: " << kStateFilePath << std::endl;
        return;
    }

    output << persisted.dump();
}

void FileRequestHandler::RemovePairLocked(const std::string& id) {
    if (id.empty()) {
        return;
    }

    // Remove from both in-memory and persisted maps (used for failed starts).
    id_query_map_.erase(id);
    RemovePersistedPairLocked(id);
}

void FileRequestHandler::RemovePersistedPairLocked(const std::string& id) {
    if (id.empty()) {
        return;
    }

    auto it = persisted_id_query_map_.find(id);
    if (it == persisted_id_query_map_.end()) {
        return;
    }

    persisted_id_query_map_.erase(it);
    PersistStateLocked();
}

int ServerApp::main(const std::vector<std::string>&) {
    std::string nats_server_url = "nats://localhost:4222";
    int port = 9000;

    NatsManager nats_manager;
    bool status = nats_manager.Connect(nats_server_url);
    if (!status) {
        return Application::EXIT_SOFTWARE;
    }
    FileRequestHandler::StartMathAliveWatcher(nats_manager);

    Poco::Net::ServerSocket svs(port);
    Poco::Net::HTTPServer srv(new FileRequestHandlerFactory(nats_manager), svs, new Poco::Net::HTTPServerParams);
    srv.start();
    logger::log() << "HTTP Server started on port " << port << std::endl;
    waitForTerminationRequest();  // wait for CTRL-C
    srv.stop();

    return Application::EXIT_OK;
}