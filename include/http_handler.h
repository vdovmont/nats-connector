#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Util/ServerApplication.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unordered_map>

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

    // Subscribe to MathCore heartbeat channel; should be called once during startup.
    static bool StartMathAliveWatcher(NatsManager& nats_manager);
    static bool IsMathCoreAlive();

  private:
    static void RecordMathCoreHeartbeat(const nlohmann::json& payload);
    static void HandleMathCoreStartup();

    std::string GenerateID();
    std::string GetID(int Query);
    int ParseQuery(std::string& uri);
    int NextQuery(const std::string& ID);

    void HandleStart(Poco::Net::HTTPServerRequest& request, std::ostream& ostr);
    void HandleState(std::ostream& ostr, int ID);

    nlohmann::json GenerateResponse(const int query,
                                    const std::string& ID,
                                    const enum Status status,
                                    const std::string& desc);
    nlohmann::json GenerateErrorResponse(const int query, const std::string& desc);
    void OnMessageState(const std::string& msg_subject,
                        const nlohmann::json& message,
                        nlohmann::json& state,
                        const int Query);

    void EnsureStateLoadedLocked();
    static void PersistStateLocked();
    void RemovePairLocked(const std::string& id);
    void RemovePersistedPairLocked(const std::string& id);

    NatsManager& nats_manager_;
    static int query_number_;
    static std::unordered_map<std::string, int> id_query_map_;
    static std::unordered_map<std::string, int> persisted_id_query_map_;
    static std::mutex state_mutex_;
    static bool state_loaded_;
    static const std::string kStateFilePath;

    static std::atomic<bool> mathcore_alive_;
    static std::atomic<uint64_t> mathcore_startup_epoch_;
    static std::chrono::steady_clock::time_point last_mathcore_heartbeat_;
    static std::mutex health_mutex_;
    static bool mathcore_subscription_active_;
    static const std::chrono::seconds kMathAliveTimeout;
    static const std::string kMathAliveSubject;
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
