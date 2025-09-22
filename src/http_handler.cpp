#include "http_handler.h"

#include <Poco/URI.h>

#include <chrono>
#include <iomanip>

#include "nats_manager.h"

std::string FileRequestHandler::GenerateID() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000;

    // Format: YYYYMMDD_HHMMSS
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t_now), "%Y%m%d_%H%M%S");

    return oss.str();
}

std::string FileRequestHandler::ParseID(std::string& uri) {
    Poco::URI parsedUri(uri);
    Poco::URI::QueryParameters params = parsedUri.getQueryParameters();
    std::string ID;
    for (const auto& p : params) {
        if (p.first == "num") {
            ID = p.second;
            break;
        }
    }
    return ID;
}

void FileRequestHandler::handleRequest(Poco::Net::HTTPServerRequest& request, Poco::Net::HTTPServerResponse& response) {
    response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
    response.setContentType("application/json");

    std::ostream& ostr = response.send();
    std::string uri = request.getURI();

    if (uri.find("/start") == 0) {
        HandleStart(request, ostr);
    } else if (uri.find("/state") == 0) {
        std::string ID = ParseID(uri);
        HandleState(ostr, ID);
    } else {
        nlohmann::json errorJson = {{"status", Status::Error}, {"reason", "unknown command"}};
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
        std::string start_subject = "Start.";
        start_subject += ID;

        nlohmann::json message = {{"data", body.str()}};
        bool published = nats_manager_.Publish(start_subject, message);

        if (published) {
            responseJson = {{"status", Status::Ok}, {"id", ID}};
        } else {
            responseJson = {{"status", Status::Error}, {"reason", "Failed to publish message to NATS"}};
        }
    } else {
        responseJson = {{"status", Status::Error}, {"reason", "Message is empty"}};
    }

    ostr << responseJson.dump();
}

void FileRequestHandler::HandleState(std::ostream& ostr, std::string& ID) {
    nlohmann::json responseJson;

    if (!ID.empty()) {
        std::string state_request_subject = "State.Request.";
        std::string state_response_subject = "State.Response.";
        state_request_subject += ID;
        state_response_subject += ID;
        nlohmann::json state;
        bool status = nats_manager_.Subscribe(
            state_response_subject, [this, &state](const std::string& msg_subject, const nlohmann::json& message) {
                this->OnMessageState(msg_subject, message, state);
            });
        if (!status) {
            responseJson = {{"status", Status::Error}, {"reason", "Failed to subscribe to NATS subject"}};
        } else {
            nlohmann::json message = {{"id", ID}};
            status = nats_manager_.Publish(state_request_subject, message);
            if (!status) {
                responseJson = {{"status", Status::Error}, {"reason", "Failed to publish message to NATS"}};
            } else {
                while (state.empty()) {
                    // wait for the state to be updated via the callback
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                responseJson = state;
                //responseJson = {{"status", Status::Ok}, {"state", state}};
            }
        }
    } else {
        responseJson = {{"status", Status::Error}, {"reason", "ID is not provided"}};
    }

    ostr << responseJson.dump();
}

void FileRequestHandler::OnMessageState(const std::string& msg_subject,
                                        const nlohmann::json& message,
                                        nlohmann::json& state) {
    state = message;
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