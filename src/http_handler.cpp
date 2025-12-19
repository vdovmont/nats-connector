#include "http_handler.h"

#include <Poco/URI.h>

#include <chrono>
#include <fstream>
#include <future>
#include <iomanip>
#include <mutex>

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
std::mutex FileRequestHandler::state_mutex_;
bool FileRequestHandler::state_loaded_ = false;
const std::string FileRequestHandler::kStateFilePath = "query_state.json";

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

int FileRequestHandler::NextQuery(const std::string& ID) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    EnsureStateLoadedLocked();
    ++query_number_;
    id_query_map_[ID] = query_number_;
    PersistStateLocked();
    return query_number_;
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

    if (!body.str().empty()) {
        std::string ID = GenerateID();
        int Query = NextQuery(ID);
        std::string start_subject = "Start.";
        start_subject += ID;

        nlohmann::json message = nlohmann::json::parse(body.str());
        bool published = nats_manager_.Publish(start_subject, message);

        if (published) {
            responseJson = GenerateResponse(Query, ID, Status::Ok, "BUFFERED");
        } else {
            std::lock_guard<std::mutex> lock(state_mutex_);
            RemovePairLocked(ID);
            responseJson = GenerateResponse(Query, ID, Status::Error, "Failed to publish message to NATS");
        }
    } else {
        responseJson["error"] = "Message is empty";
    }

    ostr << responseJson.dump();
}

void FileRequestHandler::HandleState(std::ostream& ostr, int Query) {
    std::string ID = GetID(Query);
    nlohmann::json responseJson;

    if (ID.empty()) {
        responseJson =
            GenerateResponse(Query, ID, Status::Error, "Wrong query number (either not found or not generated yet)");
        ostr << responseJson.dump();
        return;
    }

    std::string state_request_subject = "State.Request." + ID;
    std::string state_response_subject = "State.Response." + ID;

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
            responseJson = GenerateResponse(Query, ID, Status::Error, "Failed to publish message to NATS");
        } else {
            // wait for response (blocks until promise is fulfilled)
            responseJson = future.get();
        }
    }

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
    RemovePairLocked(ID);
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
                        if (query > query_number_) {
                            query_number_ = query;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to load persisted query state: " << e.what() << std::endl;
        }
    }
    state_loaded_ = true;
}

void FileRequestHandler::PersistStateLocked() {
    nlohmann::json persisted = nlohmann::json::array();
    for (const auto& entry : id_query_map_) {
        persisted.push_back({{"id", entry.first}, {"query", entry.second}});
    }

    std::ofstream output(kStateFilePath, std::ios::trunc);
    if (!output.is_open()) {
        std::cerr << "Failed to open state file for writing: " << kStateFilePath << std::endl;
        return;
    }

    output << persisted.dump();
}

void FileRequestHandler::RemovePairLocked(const std::string& id) {
    if (id.empty()) {
        return;
    }

    auto it = id_query_map_.find(id);
    if (it == id_query_map_.end()) {
        return;
    }

    id_query_map_.erase(it);
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

    Poco::Net::ServerSocket svs(port);
    Poco::Net::HTTPServer srv(new FileRequestHandlerFactory(nats_manager), svs, new Poco::Net::HTTPServerParams);
    srv.start();
    std::cout << "HTTP Server started on port " << port << std::endl;
    waitForTerminationRequest();  // wait for CTRL-C
    srv.stop();

    return Application::EXIT_OK;
}