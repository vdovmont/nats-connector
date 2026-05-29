#include "http_handler.h"

#include <Poco/URI.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <vector>

#include "logger.h"
#include "nats_manager.h"
#include "request_id_builder.h"

namespace {
void TrySetPromiseError(const std::shared_ptr<std::promise<nlohmann::json>>& promise, const std::exception& e) {
    nlohmann::json error_json;
    error_json["error"] = e.what();
    try {
        promise->set_value(std::move(error_json));
    } catch (...) {}
}
}  // namespace

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
const std::chrono::system_clock::time_point FileRequestHandler::application_start_time_ =
    std::chrono::system_clock::now();
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

void FileRequestHandler::ResetMathAliveWatcher() {
    std::lock_guard<std::mutex> lock(health_mutex_);
    mathcore_subscription_active_ = false;
    mathcore_alive_.store(false, std::memory_order_relaxed);
    last_mathcore_heartbeat_ = std::chrono::steady_clock::now();
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

int FileRequestHandler::GetQueryByID(const std::string& ID) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    EnsureStateLoadedLocked();
    auto it = id_query_map_.find(ID);
    if (it == id_query_map_.end()) {
        return 0;
    }
    return it->second;
}

int FileRequestHandler::GetLastQueryNumber() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    EnsureStateLoadedLocked();
    return query_number_;
}

std::string FileRequestHandler::FormatSystemTime(const std::chrono::system_clock::time_point& time_point) {
    auto time_t_value = std::chrono::system_clock::to_time_t(time_point);
    std::tm timeinfo{};
#if defined(_WIN32)
    localtime_s(&timeinfo, &time_t_value);
#else
    localtime_r(&time_t_value, &timeinfo);
#endif

    std::ostringstream oss;
    oss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

int FileRequestHandler::ParseQuery(std::string& uri) {
    Poco::URI::QueryParameters params;
    try {
        Poco::URI parsedUri(uri);
        params = parsedUri.getQueryParameters();
    } catch (const std::exception& e) {
        logger::log_error() << "Failed to parse URI for query: " << e.what() << std::endl;
        return 0;
    }

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
    Poco::URI::QueryParameters params;
    try {
        Poco::URI parsedUri(uri);
        params = parsedUri.getQueryParameters();
    } catch (const std::exception& e) {
        logger::log_error() << "Failed to parse URI for log id: " << e.what() << std::endl;
        return "";
    }

    for (const auto& p : params) {
        if (p.first == "id") {
            return p.second;
        }
    }

    return "";
}

std::string FileRequestHandler::ParseId(const std::string& uri) {
    Poco::URI::QueryParameters params;
    try {
        Poco::URI parsedUri(uri);
        params = parsedUri.getQueryParameters();
    } catch (const std::exception& e) {
        logger::log_error() << "Failed to parse URI for id: " << e.what() << std::endl;
        return "";
    }

    for (const auto& p : params) {
        if (p.first == "id") {
            return p.second;
        }
    }

    return "";
}

int FileRequestHandler::ParseQueue(const std::string& uri) {
    Poco::URI::QueryParameters params;
    try {
        Poco::URI parsedUri(uri);
        params = parsedUri.getQueryParameters();
    } catch (const std::exception& e) {
        logger::log_error() << "Failed to parse URI for queue: " << e.what() << std::endl;
        return 0;
    }

    for (const auto& p : params) {
        if (p.first == "queue" || p.first == "numTicket" || p.first == "num") {
            try {
                return std::stoi(p.second);
            } catch (const std::exception&) {
                return 0;
            }
        }
    }

    return 0;
}

nlohmann::json FileRequestHandler::ParseLogsListOptions(const std::string& uri) {
    nlohmann::json options = {{"sort", "name"}, {"order", "desc"}};
    Poco::URI::QueryParameters params;
    try {
        Poco::URI parsedUri(uri);
        params = parsedUri.getQueryParameters();
    } catch (const std::exception& e) {
        logger::log_error() << "Failed to parse URI for logslist options: " << e.what() << std::endl;
        return {{"error", "invalid logslist parameters"}};
    }

    for (const auto& p : params) {
        if (p.first == "sort") {
            if (p.second != "name" && p.second != "date") {
                return {{"error", "invalid sort value"}};
            }
            options["sort"] = p.second;
        } else if (p.first == "order") {
            if (p.second != "asc" && p.second != "desc") {
                return {{"error", "invalid order value"}};
            }
            options["order"] = p.second;
        }
    }

    return options;
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
    const auto start_time = std::chrono::steady_clock::now();
    bool done = false;
    while (!done) {
        if (std::chrono::steady_clock::now() - start_time > std::chrono::seconds(61)) {
            nats_manager_.Unsubscribe(response_subject);
            response_json = make_error("MathCore timeout reached");
            logger::log_error() << "Timeout while waiting for " << request_name << " response" << std::endl;
            break;
        }

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
            try {
                response_json = future.get();
                logger::log() << "Received MathCore response for " << request_name << std::endl;
                done = true;
            } catch (const std::exception& e) {
                nats_manager_.Unsubscribe(response_subject);
                response_json = make_error("MathCore response failed");
                logger::log_error() << "Failed to read MathCore response for " << request_name << ": " << e.what()
                                    << std::endl;
                done = true;
            }
        }
    }
}

void FileRequestHandler::handleRequest(Poco::Net::HTTPServerRequest& request, Poco::Net::HTTPServerResponse& response) {
    response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
    response.setContentType("application/json");

    std::ostream& ostr = response.send();
    try {
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
            nlohmann::json request_options = ParseLogsListOptions(uri);
            if (request_options.contains("error")) {
                ostr << request_options.dump();
            } else {
                HandleLogsList(ostr, request_options);
            }
        } else if (uri.find("/natslogslist") == 0 || uri.find("/natsloglist") == 0) {
            HandleNatsLogsList(ostr);
        } else if (uri.find("/getlog") == 0) {
            std::string id = ParseLogId(uri);
            if (id.empty()) {
                errorJson["error"] = "invalid or missing id";
                ostr << errorJson.dump();
            } else {
                HandleGetLog(ostr, id);
            }
        } else if (uri.find("/getqueue") == 0) {
            HandleGetQueue(ostr);
        } else if (uri.find("/stopcalculation") == 0) {
            std::string id = ParseId(uri);
            if (id.empty()) {
                int queue = ParseQueue(uri);
                id = GetID(queue);
                if (queue == 0 || id.empty()) {
                    errorJson["error"] = "invalid or missing id/queue";
                    ostr << errorJson.dump();
                    return;
                }
            }
            HandleStopCalculation(ostr, id);
        } else if (uri.find("/natsgetlog") == 0) {
            std::string id = ParseLogId(uri);
            if (id.empty()) {
                errorJson["error"] = "invalid or missing id";
                ostr << errorJson.dump();
            } else {
                HandleNatsGetLog(ostr, id);
            }
        } else {
            nlohmann::json errorJson;
            errorJson["error"] = "unknown command";
            ostr << errorJson.dump();
        }
    } catch (const std::exception& e) {
        nlohmann::json errorJson;
        errorJson["error"] = "internal error";
        errorJson["details"] = e.what();
        logger::log_error() << "Unhandled exception in handleRequest: " << e.what() << std::endl;
        ostr << errorJson.dump();
    } catch (...) {
        nlohmann::json errorJson;
        errorJson["error"] = "internal error";
        logger::log_error() << "Unhandled unknown exception in handleRequest" << std::endl;
        ostr << errorJson.dump();
    }
}

void FileRequestHandler::HandleStart(Poco::Net::HTTPServerRequest& request, std::ostream& ostr) {
    std::ostringstream body;
    std::istream& stream = request.stream();
    body << stream.rdbuf();
    nlohmann::json responseJson;

    if (!nats_manager_.IsConnected()) {
        responseJson = GenerateErrorResponse(0, "NATS server is unavailable");
        logger::log_error() << "Received Start request while NATS server is unavailable" << std::endl;
    } else if (!IsMathCoreAlive()) {
        responseJson = GenerateErrorResponse(0, "MathCore is unavailable");
        logger::log_error() << "Received Start request while MathCore is unavailable" << std::endl;
    } else if (!body.str().empty()) {
        std::string ID;
        int Query = 0;

        try {
            nlohmann::json message = nlohmann::json::parse(body.str());
            ID = RequestIdBuilder().Build(message);
            Query = NextQuery(ID);
            const std::string start_subject = "Start." + ID;
            logger::log() << "Received Start request with ID=" << ID << " (query=" << Query << ")" << std::endl;

            bool published = nats_manager_.Publish(start_subject, message);

            if (published) {
                responseJson = GenerateResponse(Query, ID, Status::Ok, "BUFFERED");
            } else {
                std::lock_guard<std::mutex> lock(state_mutex_);
                RemovePairLocked(ID);
                responseJson = GenerateResponse(Query, ID, Status::Error, "Failed to publish message to NATS");
                logger::log_error() << "Failed to publish Start request with ID=" << ID << std::endl;
            }
        } catch (const std::exception& e) {
            if (!ID.empty()) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                RemovePairLocked(ID);
                responseJson = GenerateResponse(Query, ID, Status::Error, "Invalid JSON body");
            } else {
                responseJson = GenerateErrorResponse(0, "Invalid JSON body");
            }
            logger::log_error() << "Invalid JSON in Start request: " << e.what() << std::endl;
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

    if (!nats_manager_.IsConnected()) {
        responseJson = GenerateResponse(Query, ID, Status::Error, "NATS server is unavailable");
        logger::log_error() << "NATS server unavailable for State request ID=" << ID << std::endl;
        ostr << responseJson.dump();
        return;
    }

    if (!IsMathCoreAlive()) {
        responseJson = GenerateResponse(Query, ID, Status::Error, "MathCore is unavailable");
        std::lock_guard<std::mutex> lock(state_mutex_);
        EnsureStateLoadedLocked();
        logger::log_error() << "MathCore unavailable for State request ID=" << ID << std::endl;
        ostr << responseJson.dump();
        return;
    }

    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto future = promise->get_future();

    auto sub =
        nats_manager_.Subscribe(state_response_subject,
                                [promise, done, Query, nats = &nats_manager_](const std::string& msg_subject,
                                                                              const nlohmann::json& message) mutable {
                                    if (done->exchange(true)) {
                                        return;
                                    }
                                    try {
                                        nlohmann::json state;
                                        FileRequestHandler::OnMessageState(msg_subject, message, state, Query);
                                        promise->set_value(std::move(state));
                                    } catch (const std::exception& e) {
                                        TrySetPromiseError(promise, e);
                                    }
                                    nats->Unsubscribe(msg_subject);  // unsubscribe right after we get our message
                                });

    if (!sub) {
        responseJson = GenerateResponse(Query, ID, Status::Error, "Failed to subscribe to NATS subject");
    } else {
        nlohmann::json request = {{"id", ID}};
        if (!nats_manager_.Publish(state_request_subject, request)) {
            nats_manager_.Unsubscribe(state_response_subject);
            responseJson = GenerateResponse(Query, ID, Status::Error, "Failed to publish message to NATS");
        } else {
            auto make_error = [Query, &ID](const std::string& message) {
                return GenerateResponse(Query, ID, Status::Error, message);
            };
            auto on_restart_cleanup = [&ID]() {
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

void FileRequestHandler::HandleLogsList(std::ostream& ostr, const nlohmann::json& request_options) {
    nlohmann::json responseJson;
    uint64_t startup_epoch = mathcore_startup_epoch_.load(std::memory_order_relaxed);
    const std::string request_subject = "LogsList.Request";
    const std::string response_subject = "LogsList.Response";
    logger::log() << "Received LogsList request sort=" << request_options.value("sort", "name")
                  << " order=" << request_options.value("order", "desc") << std::endl;

    if (!nats_manager_.IsConnected()) {
        responseJson = GenerateErrorResponse(0, "NATS server is unavailable");
        logger::log_error() << "NATS server unavailable for LogsList request" << std::endl;
        ostr << responseJson.dump();
        return;
    }

    if (!IsMathCoreAlive()) {
        responseJson = GenerateErrorResponse(0, "MathCore is unavailable");
        logger::log_error() << "MathCore unavailable for LogsList request" << std::endl;
        ostr << responseJson.dump();
        return;
    }

    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto future = promise->get_future();

    auto sub = nats_manager_.Subscribe(
        response_subject,
        [promise, done, nats = &nats_manager_](const std::string& msg_subject, const nlohmann::json& message) mutable {
            if (done->exchange(true)) {
                return;
            }
            try {
                promise->set_value(message);
            } catch (const std::exception& e) {
                TrySetPromiseError(promise, e);
            }
            nats->Unsubscribe(msg_subject);  // unsubscribe right after we get our message
        });

    if (!sub) {
        responseJson = GenerateErrorResponse(0, "Failed to subscribe to NATS subject");
    } else if (!nats_manager_.Publish(request_subject, request_options)) {
        nats_manager_.Unsubscribe(response_subject);
        responseJson = GenerateErrorResponse(0, "Failed to publish message to NATS");
    } else {
        auto make_error = [](const std::string& message) {
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

    if (!nats_manager_.IsConnected()) {
        responseJson = GenerateErrorResponse(0, "NATS server is unavailable");
        logger::log_error() << "NATS server unavailable for GetLog request ID=" << id << std::endl;
        ostr << responseJson.dump();
        return;
    }

    if (!IsMathCoreAlive()) {
        responseJson = GenerateErrorResponse(0, "MathCore is unavailable");
        logger::log_error() << "MathCore unavailable for GetLog request ID=" << id << std::endl;
        ostr << responseJson.dump();
        return;
    }

    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto future = promise->get_future();

    auto sub = nats_manager_.Subscribe(
        response_subject,
        [promise, done, nats = &nats_manager_](const std::string& msg_subject, const nlohmann::json& message) mutable {
            if (done->exchange(true)) {
                return;
            }
            try {
                promise->set_value(message);
            } catch (const std::exception& e) {
                TrySetPromiseError(promise, e);
            }
            nats->Unsubscribe(msg_subject);  // unsubscribe right after we get our message
        });

    if (!sub) {
        responseJson = GenerateErrorResponse(0, "Failed to subscribe to NATS subject");
    } else {
        nlohmann::json request = {{"id", id}};
        if (!nats_manager_.Publish(request_subject, request)) {
            nats_manager_.Unsubscribe(response_subject);
            responseJson = GenerateErrorResponse(0, "Failed to publish message to NATS");
        } else {
            auto make_error = [](const std::string& message) {
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

void FileRequestHandler::HandleGetQueue(std::ostream& ostr) {
    nlohmann::json responseJson;
    uint64_t startup_epoch = mathcore_startup_epoch_.load(std::memory_order_relaxed);
    const std::string request_subject = "GetQueue.Request";
    const std::string response_subject = "GetQueue.Response";
    logger::log() << "Received GetQueue request" << std::endl;

    if (!nats_manager_.IsConnected()) {
        responseJson = GenerateErrorResponse(0, "NATS server is unavailable");
        logger::log_error() << "NATS server unavailable for GetQueue request" << std::endl;
        ostr << responseJson.dump();
        return;
    }

    if (!IsMathCoreAlive()) {
        responseJson = GenerateErrorResponse(0, "MathCore is unavailable");
        logger::log_error() << "MathCore unavailable for GetQueue request" << std::endl;
        ostr << responseJson.dump();
        return;
    }

    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto future = promise->get_future();

    auto sub = nats_manager_.Subscribe(
        response_subject,
        [promise, done, nats = &nats_manager_](const std::string& msg_subject, const nlohmann::json& message) mutable {
            if (done->exchange(true)) {
                return;
            }
            try {
                promise->set_value(message);
            } catch (const std::exception& e) {
                TrySetPromiseError(promise, e);
            }
            nats->Unsubscribe(msg_subject);
        });

    if (!sub) {
        responseJson = GenerateErrorResponse(0, "Failed to subscribe to NATS subject");
    } else if (!nats_manager_.Publish(request_subject, nlohmann::json::object())) {
        nats_manager_.Unsubscribe(response_subject);
        responseJson = GenerateErrorResponse(0, "Failed to publish message to NATS");
    } else {
        auto make_error = [](const std::string& message) {
            return GenerateErrorResponse(0, message);
        };
        logger::log() << "Waiting for MathCore response to GetQueue request" << std::endl;
        WaitForResponse(startup_epoch, response_subject, "GetQueue request", future, make_error, nullptr, responseJson);
    }

    AddQueueNumbersToGetQueueResponse(responseJson);
    responseJson["0. " + FormatSystemTime(std::chrono::system_clock::now())] = "Current time";
    responseJson["1. " + FormatSystemTime(application_start_time_)] = "nats-connector restart time";
    responseJson["Last nats-connector queue number"] = GetLastQueryNumber();

    logger::log() << "Sent GetQueue response" << std::endl;
    ostr << responseJson.dump();
}

void FileRequestHandler::HandleStopCalculation(std::ostream& ostr, const std::string& id) {
    nlohmann::json responseJson;
    uint64_t startup_epoch = mathcore_startup_epoch_.load(std::memory_order_relaxed);
    const std::string request_subject = "StopCalculation.Request." + id;
    const std::string response_subject = "StopCalculation.Response." + id;
    logger::log() << "Received StopCalculation request with ID=" << id << std::endl;

    if (!nats_manager_.IsConnected()) {
        responseJson = GenerateErrorResponse(0, "NATS server is unavailable");
        logger::log_error() << "NATS server unavailable for StopCalculation request ID=" << id << std::endl;
        ostr << responseJson.dump();
        return;
    }

    if (!IsMathCoreAlive()) {
        responseJson = GenerateErrorResponse(0, "MathCore is unavailable");
        logger::log_error() << "MathCore unavailable for StopCalculation request ID=" << id << std::endl;
        ostr << responseJson.dump();
        return;
    }

    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto future = promise->get_future();

    auto sub = nats_manager_.Subscribe(
        response_subject,
        [promise, done, nats = &nats_manager_](const std::string& msg_subject, const nlohmann::json& message) mutable {
            if (done->exchange(true)) {
                return;
            }
            try {
                promise->set_value(message);
            } catch (const std::exception& e) {
                TrySetPromiseError(promise, e);
            }
            nats->Unsubscribe(msg_subject);
        });

    if (!sub) {
        responseJson = GenerateErrorResponse(0, "Failed to subscribe to NATS subject");
    } else {
        nlohmann::json request = {{"id", id}};
        if (!nats_manager_.Publish(request_subject, request)) {
            nats_manager_.Unsubscribe(response_subject);
            responseJson = GenerateErrorResponse(0, "Failed to publish message to NATS");
        } else {
            auto make_error = [](const std::string& message) {
                return GenerateErrorResponse(0, message);
            };
            logger::log() << "Waiting for MathCore response to StopCalculation request ID=" << id << std::endl;
            WaitForResponse(startup_epoch,
                            response_subject,
                            "StopCalculation request ID=" + id,
                            future,
                            make_error,
                            nullptr,
                            responseJson);
        }
    }

    logger::log() << "Sent StopCalculation response for ID=" << id << std::endl;
    ostr << responseJson.dump();
}

void FileRequestHandler::HandleNatsLogsList(std::ostream& ostr) {
    nlohmann::json responseJson;
    try {
        logger::log() << "Received NatsLogsList request" << std::endl;
        const std::filesystem::path logs_dir("logs/nats-connector");
        std::vector<std::string> files;
        if (std::filesystem::exists(logs_dir)) {
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator
                     it(logs_dir, std::filesystem::directory_options::skip_permission_denied, ec),
                 end;
                 it != end;
                 it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (it->is_regular_file(ec)) {
                    if (ec) {
                        ec.clear();
                        continue;
                    }
                    const auto name = it->path().stem().string();
                    files.push_back(name);
                }
            }
        }
        std::sort(files.begin(), files.end(), std::greater<std::string>());
        responseJson["files"] = files;
        logger::log() << "Sent NatsLogsList response with " << files.size() << " files" << std::endl;
    } catch (const std::exception& e) {
        responseJson["error"] = e.what();
        logger::log_error() << "Error in NatsLogsList: " << e.what() << std::endl;
    }

    ostr << responseJson.dump();
}

void FileRequestHandler::HandleNatsGetLog(std::ostream& ostr, const std::string& id) {
    nlohmann::json responseJson;
    try {
        logger::log() << "Received NatsGetLog request with ID=" << id << std::endl;
        const std::filesystem::path logs_dir("logs/nats-connector");
        if (!std::filesystem::exists(logs_dir)) {
            responseJson["error"] = "Logs directory not found";
            logger::log() << "Sent NatsGetLog response for ID=" << id << " (logs dir missing)" << std::endl;
            ostr << responseJson.dump();
            return;
        }

        const std::string target_stem = std::filesystem::path(id).stem().string();
        std::filesystem::path found_path;
        std::filesystem::path found_json;
        std::filesystem::path found_log;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator
                 it(logs_dir, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             it != end;
             it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (it->is_regular_file(ec) && it->path().stem().string() == target_stem) {
                const auto ext = it->path().extension().string();
                if (ext == ".json") {
                    found_json = it->path();
                } else if (ext == ".log") {
                    found_log = it->path();
                } else if (found_path.empty()) {
                    found_path = it->path();
                }
            }
        }

        if (!found_json.empty()) {
            found_path = found_json;
        } else if (!found_log.empty()) {
            found_path = found_log;
        }

        if (found_path.empty()) {
            responseJson["error"] = "Log file not found";
            logger::log() << "Sent NatsGetLog response for ID=" << id << " (file not found)" << std::endl;
            ostr << responseJson.dump();
            return;
        }

        if (found_path.extension() == ".log") {
            responseJson = PackLogFileToJson(found_path);
            logger::log() << "Sent NatsGetLog response for ID=" << id << " (log packed)" << std::endl;
            ostr << responseJson.dump();
            return;
        }

        std::ifstream in_file(found_path);
        if (!in_file.is_open()) {
            responseJson["error"] = "Failed to open log file";
            logger::log() << "Sent NatsGetLog response for ID=" << id << " (open failed)" << std::endl;
            ostr << responseJson.dump();
            return;
        }

        try {
            in_file >> responseJson;
        } catch (const std::exception& e) {
            nlohmann::json error_json;
            error_json["error"] = "Failed to parse log file as json";
            error_json["details"] = e.what();
            error_json["filename"] = found_path.filename().string();
            responseJson = std::move(error_json);
            logger::log() << "Sent NatsGetLog response for ID=" << id << " (json parse failed)" << std::endl;
            ostr << responseJson.dump();
            return;
        }

        logger::log() << "Sent NatsGetLog response for ID=" << id << std::endl;
    } catch (const std::exception& e) {
        responseJson["error"] = e.what();
        logger::log_error() << "Error in NatsGetLog: " << e.what() << std::endl;
    }

    ostr << responseJson.dump();
}

nlohmann::json FileRequestHandler::PackLogFileToJson(const std::filesystem::path& log_path) {
    nlohmann::json response;
    response["filename"] = log_path.filename().string();
    response["lines"] = nlohmann::json::array();

    std::ifstream in_file(log_path);
    if (!in_file.is_open()) {
        response["error"] = "Failed to open log file";
        return response;
    }

    std::string line;
    while (std::getline(in_file, line)) {
        response["lines"].push_back(line);
    }

    return response;
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

void FileRequestHandler::AddQueueNumbersToGetQueueResponse(nlohmann::json& response) {
    if (!response.contains("MathCore") || !response["MathCore"].is_array()) {
        return;
    }

    for (auto& entry : response["MathCore"]) {
        if (!entry.is_string()) {
            continue;
        }

        std::string value = entry.get<std::string>();
        const auto separator_pos = value.find(':');
        if (separator_pos == std::string::npos) {
            continue;
        }

        std::string id = value.substr(separator_pos + 1);
        id.erase(id.begin(), std::find_if(id.begin(), id.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        id.erase(std::find_if(id.rbegin(), id.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
                 id.end());

        int query = GetQueryByID(id);
        if (query == 0) {
            continue;
        }

        entry = value + ", nats queue: " + std::to_string(query);
    }
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
    Poco::Net::ServerSocket svs(port);
    Poco::Net::HTTPServer srv(new FileRequestHandlerFactory(nats_manager), svs, new Poco::Net::HTTPServerParams);
    srv.start();
    logger::log() << "HTTP Server started on port " << port << std::endl;
    nats_manager.StartReconnectLoop(
        nats_server_url,
        [&]() {
            logger::log() << "Connected to NATS server at " << nats_server_url << std::endl;
            FileRequestHandler::StartMathAliveWatcher(nats_manager);
        },
        [&]() { FileRequestHandler::ResetMathAliveWatcher(); });

    waitForTerminationRequest();  // wait for CTRL-C
    srv.stop();
    nats_manager.StopReconnectLoop();

    return Application::EXIT_OK;
}
