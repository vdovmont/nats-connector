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
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
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
    static void ResetMathAliveWatcher();
    static bool IsMathCoreAlive();

  private:
    static void RecordMathCoreHeartbeat(const nlohmann::json& payload);
    static void HandleMathCoreStartup();

    std::string GenerateID();
    static std::string GetID(int Query);
    static int GetQueryByID(const std::string& ID);
    static int GetLastQueryNumber();
    static std::string FormatSystemTime(const std::chrono::system_clock::time_point& time_point);
    int ParseQuery(std::string& uri);
    std::string ParseLogId(const std::string& uri);
    std::string ParseId(const std::string& uri);
    int ParseQueue(const std::string& uri);
    nlohmann::json ParseLogsListOptions(const std::string& uri);
    int NextQuery(const std::string& ID);

    void HandleStart(Poco::Net::HTTPServerRequest& request, std::ostream& ostr);
    void HandleState(std::ostream& ostr, int ID);
    void HandleLogsList(std::ostream& ostr, const nlohmann::json& request_options);
    void HandleGetLog(std::ostream& ostr, const std::string& id);
    void HandleGetQueue(std::ostream& ostr);
    void HandleStopCalculation(std::ostream& ostr, const std::string& id);
    void HandleNatsLogsList(std::ostream& ostr);
    void HandleNatsGetLog(std::ostream& ostr, const std::string& id);
    void WaitForResponse(uint64_t startup_epoch,
                         const std::string& response_subject,
                         const std::string& request_name,
                         std::future<nlohmann::json>& future,
                         const std::function<nlohmann::json(const std::string&)>& make_error,
                         const std::function<void()>& on_restart_cleanup,
                         nlohmann::json& response_json);

    nlohmann::json PackLogFileToJson(const std::filesystem::path& log_path);
    static nlohmann::json GenerateResponse(const int query,
                                           const std::string& ID,
                                           const enum Status status,
                                           const std::string& desc);
    static nlohmann::json GenerateErrorResponse(const int query, const std::string& desc);
    static void AddQueueNumbersToGetQueueResponse(nlohmann::json& response);
    static void OnMessageState(const std::string& msg_subject,
                               const nlohmann::json& message,
                               nlohmann::json& state,
                               const int Query);

    static void EnsureStateLoadedLocked();
    static void PersistStateLocked();
    static void RemovePairLocked(const std::string& id);
    static void RemovePersistedPairLocked(const std::string& id);

    NatsManager& nats_manager_;
    static int query_number_;
    static std::unordered_map<std::string, int> id_query_map_;
    static std::unordered_map<std::string, int> persisted_id_query_map_;
    static std::mutex state_mutex_;
    static bool state_loaded_;
    static const std::string kStateFilePath;
    static const std::chrono::system_clock::time_point application_start_time_;

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
