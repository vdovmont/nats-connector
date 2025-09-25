#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Util/ServerApplication.h>

#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "nats_manager.h"

enum class Status : int {
    Error = 0,
    Ok = 1
};
std::string ToString(Status s);

// HTTP request handler
class FileRequestHandler : public Poco::Net::HTTPRequestHandler {
  public:
    explicit FileRequestHandler(NatsManager& nats_manager) : nats_manager_(nats_manager) {}

    void handleRequest(Poco::Net::HTTPServerRequest& request, Poco::Net::HTTPServerResponse& response) override;

  private:
    NatsManager& nats_manager_;

    std::string GenerateID();
    std::string ParseID(std::string& uri);

    void HandleStart(Poco::Net::HTTPServerRequest& request, std::ostream& ostr);
    void HandleState(std::ostream& ostr, std::string& ID);

    nlohmann::json GenerateResponse(const std::string& ID, const enum Status status, const std::string& desc);
    nlohmann::json GenerateErrorResponse(const std::string& desc);
    void OnMessageState(const std::string& msg_subject,
                        const nlohmann::json& message,
                        nlohmann::json& state,
                        const std::string& ID);
};

// Factory to create handlers (needed by Poco)
class FileRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory {
  public:
    explicit FileRequestHandlerFactory(NatsManager& nats_manager) : nats_manager_(nats_manager) {}

    Poco::Net::HTTPRequestHandler* createRequestHandler(const Poco::Net::HTTPServerRequest&) override {
        return new FileRequestHandler(nats_manager_);
    }

  private:
    NatsManager& nats_manager_;
};

class ServerApp : public Poco::Util::ServerApplication {
  protected:
    int main(const std::vector<std::string>&) override;
};