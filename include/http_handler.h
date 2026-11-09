#pragma once

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Util/ServerApplication.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app_settings.h"
#include "nats_manager.h"

enum class Status : int {
    Error = 0,
    Ok = 1
};
std::string ToString(Status s);

struct QueueEntry {
    int queue_number = 0;
    std::string task_id;
    nlohmann::json payload = nlohmann::json::object();
    std::int64_t created_epoch_ms = 0;
    std::int64_t updated_epoch_ms = 0;
    std::string instance_id;
    std::string state = "pending";
};

struct InstanceInfo {
    std::string instance_id;
    std::string state = "unavailable";
    std::string current_task_id;
    std::int64_t started_at = 0;
    std::int64_t last_heartbeat_epoch_ms = 0;
    std::int64_t unavailable_since_epoch_ms = 0;
    std::string version;
    std::string commit;
};

struct LogEntry {
    std::string instance_id;
    std::string name;
    std::int64_t modified_time = 0;
};

class FileRequestHandler : public Poco::Net::HTTPRequestHandler {
  public:
    explicit FileRequestHandler(NatsManager& nats_manager) : nats_manager_(nats_manager) {}

    void handleRequest(Poco::Net::HTTPServerRequest& request, Poco::Net::HTTPServerResponse& response) override;

    static void Configure(const AppSettings& settings);
    static bool StartMathCoreSubscriptions(NatsManager& nats_manager);
    static void ResetMathCoreSubscriptions();
    static void ShutdownBackgroundWorkers();

  private:
    static std::int64_t NowEpochMilliseconds();
    static std::int64_t NowEpochSeconds();
    static std::string FormatSystemTime(const std::chrono::system_clock::time_point& time_point);
    static std::string FormatEpochSeconds(std::int64_t epoch_seconds);
    static std::string GenerateRequestId();
    static std::string ExtractID(const std::string& subject);
    static std::string StripInstancePrefix(const std::string& id, std::string& instance_id);
    static nlohmann::json GenerateResponse(int query, const std::string& id, Status status, const std::string& desc);
    static nlohmann::json GenerateErrorResponse(int query, const std::string& desc);
    static bool StartBackgroundWorker(std::function<void()> worker);

    static void LoadQueueStateLocked();
    static void PersistQueueStateLocked();
    static void CleanupExpiredQueueEntriesLocked();
    static int AllocateQueueNumberLocked();
    static std::optional<QueueEntry> FindQueueEntryByNumberLocked(int queue_number);
    static std::optional<QueueEntry> FindQueueEntryByTaskIdLocked(const std::string& task_id);
    static void TouchQueueEntryLocked(const std::string& task_id);
    static void RemoveQueueEntryLocked(const std::string& task_id);

    static void MarkUnavailableInstancesLocked();
    static std::optional<std::string> FindIdleInstanceLocked();
    static void DispatchPendingLocked(NatsManager& nats_manager);

    static void OnRegisterRequest(NatsManager& nats_manager, const nlohmann::json& message);
    static void OnHeartbeat(NatsManager& nats_manager, const nlohmann::json& message);
    static void OnTaskEvent(NatsManager& nats_manager, const std::string& subject, const nlohmann::json& message);
    static void OnLogsUpdated(const nlohmann::json& message);
    static void RefreshLogsList(NatsManager& nats_manager, const std::string& instance_id);
    static nlohmann::json RequestMathCore(NatsManager& nats_manager,
                                          const std::string& instance_id,
                                          const std::string& subject_suffix,
                                          nlohmann::json payload,
                                          const std::string& request_name);

    static std::string GetQueryParam(const std::string& uri, const std::string& key);
    static int ParseQueryNumber(const std::string& uri);
    static nlohmann::json ParseLogsListOptions(const std::string& uri);

    void HandleStart(Poco::Net::HTTPServerRequest& request, std::ostream& ostr);
    void HandleState(std::ostream& ostr, int queue_number);
    void HandleLogsList(std::ostream& ostr, const nlohmann::json& options);
    void HandleGetLog(std::ostream& ostr, const std::string& id, const std::string& instance_filter);
    void HandleGetQueue(std::ostream& ostr);
    void HandleStopCalculation(std::ostream& ostr, const std::string& id);
    void HandleNatsLogsList(std::ostream& ostr);
    void HandleNatsGetLog(std::ostream& ostr, const std::string& id);
    nlohmann::json PackLogFileToJson(const std::filesystem::path& log_path);

    NatsManager& nats_manager_;

    static AppSettings settings_;
    static std::mutex state_mutex_;
    static bool queue_state_loaded_;
    static bool assignment_grace_dispatch_scheduled_;
    static const std::string kQueueStateFilePath;
    static const std::chrono::system_clock::time_point application_start_time_;
    static std::chrono::steady_clock::time_point assignment_enabled_at_;
    static std::unordered_map<int, QueueEntry> queue_entries_;
    static std::unordered_map<std::string, int> task_queue_numbers_;
    static std::unordered_map<std::string, InstanceInfo> instances_;
    static std::vector<LogEntry> logs_index_;
    static bool subscriptions_active_;
    static std::atomic<bool> stopping_;
    static std::mutex workers_mutex_;
    static std::condition_variable workers_cv_;
    static std::vector<std::thread> workers_;
};

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
