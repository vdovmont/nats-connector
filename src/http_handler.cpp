#include "http_handler.h"

#include <Poco/URI.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

#include "logger.h"
#include "request_id_builder.h"

namespace {

void TrySetPromise(const std::shared_ptr<std::promise<nlohmann::json>>& promise, const nlohmann::json& value) {
    try {
        promise->set_value(value);
    } catch (...) {}
}

bool IsTerminalState(const std::string& state) {
    return state == "finished" || state == "cancelled" || state == "failed";
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

}  // namespace

inline std::string ToString(Status s) {
    switch (s) {
        case Status::Error: return "Error";
        case Status::Ok: return "Ok";
        default: return "Undefined";
    }
}

AppSettings FileRequestHandler::settings_;
std::mutex FileRequestHandler::state_mutex_;
bool FileRequestHandler::queue_state_loaded_ = false;
bool FileRequestHandler::assignment_grace_dispatch_scheduled_ = false;
bool FileRequestHandler::health_monitor_scheduled_ = false;
const std::string FileRequestHandler::kQueueStateFilePath = "queue_state.json";
const std::chrono::system_clock::time_point FileRequestHandler::application_start_time_ =
    std::chrono::system_clock::now();
std::chrono::steady_clock::time_point FileRequestHandler::assignment_enabled_at_ = std::chrono::steady_clock::now();
std::unordered_map<int, QueueEntry> FileRequestHandler::queue_entries_;
std::unordered_map<std::string, int> FileRequestHandler::task_queue_numbers_;
std::unordered_map<std::string, InstanceInfo> FileRequestHandler::instances_;
std::vector<LogEntry> FileRequestHandler::logs_index_;
bool FileRequestHandler::subscriptions_active_ = false;
std::atomic<bool> FileRequestHandler::stopping_{false};
std::mutex FileRequestHandler::workers_mutex_;
std::condition_variable FileRequestHandler::workers_cv_;
std::vector<std::thread> FileRequestHandler::workers_;

void FileRequestHandler::Configure(const AppSettings& settings) {
    settings_ = settings;
    stopping_.store(false, std::memory_order_release);
    assignment_enabled_at_ = std::chrono::steady_clock::now() + settings_.mathcore_assignment_grace;
}

bool FileRequestHandler::StartBackgroundWorker(std::function<void()> worker) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    if (stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    workers_.emplace_back([worker = std::move(worker)]() { worker(); });
    return true;
}

void FileRequestHandler::ShutdownBackgroundWorkers() {
    stopping_.store(true, std::memory_order_release);
    workers_cv_.notify_all();

    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::int64_t FileRequestHandler::NowEpochMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t FileRequestHandler::NowEpochSeconds() { return NowEpochMilliseconds() / 1000; }

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

std::string FileRequestHandler::FormatEpochSeconds(std::int64_t epoch_seconds) {
    if (epoch_seconds <= 0) {
        return "";
    }
    return FormatSystemTime(std::chrono::system_clock::from_time_t(static_cast<std::time_t>(epoch_seconds)));
}

std::string FileRequestHandler::GenerateRequestId() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device rd;
    std::uniform_int_distribution<int> dist(100000, 999999);
    return "req_" + std::to_string(NowEpochMilliseconds()) + "_" + std::to_string(counter.fetch_add(1)) + "_" +
           std::to_string(dist(rd));
}

std::string FileRequestHandler::ExtractID(const std::string& subject) {
    const size_t last_dot = subject.find_last_of('.');
    if (last_dot == std::string::npos || last_dot + 1 >= subject.size()) {
        return "";
    }
    return subject.substr(last_dot + 1);
}

std::string FileRequestHandler::StripInstancePrefix(const std::string& id, std::string& instance_id) {
    const size_t sep = id.find("::");
    if (sep == std::string::npos) {
        return id;
    }
    instance_id = id.substr(0, sep);
    return id.substr(sep + 2);
}

nlohmann::json FileRequestHandler::GenerateResponse(int query,
                                                    const std::string& id,
                                                    Status status,
                                                    const std::string& desc) {
    nlohmann::json response;
    response["query"] = query;
    response["globalID"] = id;
    response["status"] = status;
    response["desc"] = desc;
    response["solnumbs"] = 0;
    response["time"] = 0;
    return response;
}

nlohmann::json FileRequestHandler::GenerateErrorResponse(int query, const std::string& desc) {
    return GenerateResponse(query, "null", Status::Error, desc);
}

void FileRequestHandler::LoadQueueStateLocked() {
    if (queue_state_loaded_) {
        return;
    }
    queue_state_loaded_ = true;

    std::ifstream input(kQueueStateFilePath);
    if (!input.is_open()) {
        return;
    }

    try {
        nlohmann::json json;
        input >> json;
        const std::int64_t now = NowEpochMilliseconds();
        const std::int64_t timeout_ms = settings_.queue_entry_timeout.count() * 1000;
        if (json.value("last_edit_epoch_ms", 0LL) + timeout_ms < now) {
            return;
        }

        for (const auto& entry_json : json.value("entries", nlohmann::json::array())) {
            QueueEntry entry;
            entry.queue_number = entry_json.value("queue_number", 0);
            entry.task_id = entry_json.value("task_id", "");
            entry.payload = entry_json.value("payload", nlohmann::json::object());
            entry.created_epoch_ms = entry_json.value("created_epoch_ms", 0LL);
            entry.updated_epoch_ms = entry_json.value("updated_epoch_ms", 0LL);
            entry.instance_id = entry_json.value("instance_id", "");
            entry.state = entry_json.value("state", "pending");
            if (entry.queue_number > 0 && !entry.task_id.empty() && entry.updated_epoch_ms + timeout_ms >= now) {
                queue_entries_[entry.queue_number] = entry;
                task_queue_numbers_[entry.task_id] = entry.queue_number;
            }
        }
    } catch (const std::exception& e) {
        logger::log_error() << "Failed to load queue state: " << e.what() << std::endl;
    }
}

void FileRequestHandler::PersistQueueStateLocked() {
    nlohmann::json json;
    json["last_edit_epoch_ms"] = NowEpochMilliseconds();
    json["entries"] = nlohmann::json::array();
    for (const auto& [_, entry] : queue_entries_) {
        json["entries"].push_back({{"queue_number", entry.queue_number},
                                   {"task_id", entry.task_id},
                                   {"payload", entry.payload},
                                   {"created_epoch_ms", entry.created_epoch_ms},
                                   {"updated_epoch_ms", entry.updated_epoch_ms},
                                   {"instance_id", entry.instance_id},
                                   {"state", entry.state}});
    }

    std::ofstream output(kQueueStateFilePath, std::ios::trunc);
    if (!output.is_open()) {
        logger::log_error() << "Failed to write queue state file" << std::endl;
        return;
    }
    output << json.dump(4);
}

void FileRequestHandler::CleanupExpiredQueueEntriesLocked() {
    LoadQueueStateLocked();
    const std::int64_t now = NowEpochMilliseconds();
    const std::int64_t timeout_ms = settings_.queue_entry_timeout.count() * 1000;
    std::vector<int> remove_numbers;
    for (const auto& [number, entry] : queue_entries_) {
        if (entry.updated_epoch_ms + timeout_ms < now) {
            remove_numbers.push_back(number);
        }
    }
    for (int number : remove_numbers) {
        task_queue_numbers_.erase(queue_entries_[number].task_id);
        queue_entries_.erase(number);
    }
    if (!remove_numbers.empty()) {
        PersistQueueStateLocked();
    }
}

int FileRequestHandler::AllocateQueueNumberLocked() {
    CleanupExpiredQueueEntriesLocked();
    int number = 1;
    while (queue_entries_.find(number) != queue_entries_.end()) {
        ++number;
    }
    return number;
}

std::optional<QueueEntry> FileRequestHandler::FindQueueEntryByNumberLocked(int queue_number) {
    CleanupExpiredQueueEntriesLocked();
    auto it = queue_entries_.find(queue_number);
    if (it == queue_entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<QueueEntry> FileRequestHandler::FindQueueEntryByTaskIdLocked(const std::string& task_id) {
    CleanupExpiredQueueEntriesLocked();
    auto it_number = task_queue_numbers_.find(task_id);
    if (it_number == task_queue_numbers_.end()) {
        return std::nullopt;
    }
    auto it = queue_entries_.find(it_number->second);
    if (it == queue_entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void FileRequestHandler::TouchQueueEntryLocked(const std::string& task_id) {
    auto it_number = task_queue_numbers_.find(task_id);
    if (it_number == task_queue_numbers_.end()) {
        return;
    }
    auto it = queue_entries_.find(it_number->second);
    if (it == queue_entries_.end()) {
        return;
    }
    it->second.updated_epoch_ms = NowEpochMilliseconds();
    PersistQueueStateLocked();
}

void FileRequestHandler::RemoveQueueEntryLocked(const std::string& task_id) {
    auto it_number = task_queue_numbers_.find(task_id);
    if (it_number == task_queue_numbers_.end()) {
        return;
    }
    queue_entries_.erase(it_number->second);
    task_queue_numbers_.erase(it_number);
    PersistQueueStateLocked();
}

void FileRequestHandler::MarkUnavailableInstancesLocked() {
    const std::int64_t now = NowEpochMilliseconds();
    const std::int64_t ttl_ms = settings_.mathcore_instance_ttl.count() * 1000;
    const std::int64_t release_ms = settings_.mathcore_instance_release.count() * 1000;
    std::vector<std::string> release_ids;

    for (auto& [id, instance] : instances_) {
        if (instance.state != "unavailable" && instance.last_heartbeat_epoch_ms > 0 &&
            instance.last_heartbeat_epoch_ms + ttl_ms < now) {
            instance.state = "unavailable";
            instance.unavailable_since_epoch_ms = now;
            logger::log_error() << "MathCore heartbeat failed for " << id << " after "
                                << settings_.mathcore_instance_ttl.count() << " seconds" << std::endl;
            for (auto& [_, entry] : queue_entries_) {
                if (entry.instance_id == id && !IsTerminalState(entry.state)) {
                    entry.state = "mathcore unavailable/restarted";
                    entry.updated_epoch_ms = now;
                }
            }
            PersistQueueStateLocked();
        }
        if (instance.state == "unavailable" && instance.unavailable_since_epoch_ms > 0 &&
            instance.unavailable_since_epoch_ms + release_ms < now) {
            release_ids.push_back(id);
        }
    }

    for (const auto& id : release_ids) {
        logger::log() << "Released MathCore instance " << id << " after " << settings_.mathcore_instance_release.count()
                      << " seconds unavailable" << std::endl;
        instances_.erase(id);
        logs_index_.erase(
            std::remove_if(
                logs_index_.begin(), logs_index_.end(), [&](const LogEntry& entry) { return entry.instance_id == id; }),
            logs_index_.end());
    }
}

std::optional<std::string> FileRequestHandler::FindIdleInstanceLocked() {
    if (std::chrono::steady_clock::now() < assignment_enabled_at_) {
        return std::nullopt;
    }
    MarkUnavailableInstancesLocked();
    std::vector<std::string> ids;
    for (const auto& [id, instance] : instances_) {
        if (instance.state == "idle") {
            ids.push_back(id);
        }
    }
    if (ids.empty()) {
        return std::nullopt;
    }
    std::sort(ids.begin(), ids.end());
    return ids.front();
}

void FileRequestHandler::DispatchPendingLocked(NatsManager& nats_manager) {
    CleanupExpiredQueueEntriesLocked();
    bool changed = false;
    for (auto& [_, entry] : queue_entries_) {
        if (entry.state != "pending" || !entry.instance_id.empty()) {
            continue;
        }
        auto instance_id = FindIdleInstanceLocked();
        if (!instance_id.has_value()) {
            break;
        }
        nlohmann::json payload = entry.payload;
        const std::string subject = "MathCore." + *instance_id + ".Start." + entry.task_id;
        if (nats_manager.Publish(subject, payload)) {
            entry.instance_id = *instance_id;
            entry.state = "assigned";
            entry.updated_epoch_ms = NowEpochMilliseconds();
            auto& instance = instances_[*instance_id];
            instance.state = "busy";
            instance.current_task_id = entry.task_id;
            changed = true;
            logger::log() << "Dispatched task " << entry.task_id << " to " << *instance_id << std::endl;
        }
    }
    if (changed) {
        PersistQueueStateLocked();
    }
}

void FileRequestHandler::OnRegisterRequest(NatsManager& nats_manager, const nlohmann::json& message) {
    const std::string boot_id = message.value("boot_id", "");
    if (boot_id.empty()) {
        logger::log_error() << "Ignoring MathCore registration without boot_id" << std::endl;
        return;
    }

    std::string assigned_id;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        MarkUnavailableInstancesLocked();
        const std::string preferred_id = message.value("preferred_id", "");
        auto preferred = instances_.find(preferred_id);
        if (!preferred_id.empty() && (preferred == instances_.end() || preferred->second.state == "unavailable")) {
            assigned_id = preferred_id;
        } else {
            for (int index = 1; assigned_id.empty(); ++index) {
                std::ostringstream id;
                id << "MathCore" << std::setw(2) << std::setfill('0') << index;
                if (instances_.find(id.str()) == instances_.end()) {
                    assigned_id = id.str();
                }
            }
        }

        InstanceInfo& instance = instances_[assigned_id];
        instance.instance_id = assigned_id;
        instance.state = "idle";
        instance.current_task_id.clear();
        instance.last_heartbeat_epoch_ms = NowEpochMilliseconds();
        instance.unavailable_since_epoch_ms = 0;
    }

    nats_manager.Publish("MathCore.Register.Response." + boot_id, {{"instance_id", assigned_id}});
    logger::log() << "Registered MathCore instance " << assigned_id << std::endl;
    StartBackgroundWorker([&nats_manager, assigned_id]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (FileRequestHandler::stopping_.load(std::memory_order_acquire)) {
            return;
        }
        FileRequestHandler::RefreshLogsList(nats_manager, assigned_id);
    });
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        DispatchPendingLocked(nats_manager);
    }
}

void FileRequestHandler::OnHeartbeat(NatsManager& nats_manager, const nlohmann::json& message) {
    const std::string instance_id = message.value("instance_id", "");
    if (instance_id.empty()) {
        return;
    }

    bool recovered = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto existing = instances_.find(instance_id);
        recovered = existing == instances_.end() || existing->second.state == "unavailable";

        InstanceInfo& instance = instances_[instance_id];
        instance.instance_id = instance_id;
        instance.state = message.value("state", "idle");
        instance.current_task_id = message.contains("current_task_id") && !message["current_task_id"].is_null()
                                     ? message["current_task_id"].get<std::string>()
                                     : "";
        instance.started_at = message.value("started_at", 0LL);
        instance.last_heartbeat_epoch_ms = NowEpochMilliseconds();
        instance.unavailable_since_epoch_ms = 0;
        instance.version = message.value("version", "");
        instance.commit = message.value("commit", "");
    }

    if (!recovered) {
        return;
    }

    logger::log() << "Recovered MathCore instance " << instance_id << " from heartbeat" << std::endl;
    StartBackgroundWorker(
        [&nats_manager, instance_id]() { FileRequestHandler::RefreshLogsList(nats_manager, instance_id); });
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        DispatchPendingLocked(nats_manager);
    }
}

void FileRequestHandler::OnTaskEvent(NatsManager& nats_manager,
                                     const std::string& subject,
                                     const nlohmann::json& message) {
    const std::string task_id = message.value("task_id", "");
    const std::string instance_id = message.value("instance_id", "");
    const std::string event = ExtractID(subject);
    if (task_id.empty() || instance_id.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto entry = FindQueueEntryByTaskIdLocked(task_id);
        if (entry.has_value()) {
            QueueEntry& stored = queue_entries_[entry->queue_number];
            stored.instance_id = instance_id;
            stored.updated_epoch_ms = NowEpochMilliseconds();
            stored.state = Lower(event);
            PersistQueueStateLocked();
        }

        auto& instance = instances_[instance_id];
        instance.instance_id = instance_id;
        instance.last_heartbeat_epoch_ms = NowEpochMilliseconds();
        if (event == "Started") {
            instance.state = "busy";
            instance.current_task_id = task_id;
        } else if (event == "Finished" || event == "Cancelled" || event == "Failed") {
            instance.state = "idle";
            instance.current_task_id.clear();
        }
        DispatchPendingLocked(nats_manager);
    }
}

void FileRequestHandler::OnLogsUpdated(const nlohmann::json& message) {
    const std::string instance_id = message.value("instance_id", "");
    const std::string action = message.value("action", "");
    if (instance_id.empty() || !message.contains("files") || !message["files"].is_array()) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto& file : message["files"]) {
        const std::string name = file.value("name", "");
        if (name.empty()) {
            continue;
        }
        if (action == "created") {
            logs_index_.erase(std::remove_if(logs_index_.begin(),
                                             logs_index_.end(),
                                             [&](const LogEntry& entry) {
                                                 return entry.instance_id == instance_id && entry.name == name;
                                             }),
                              logs_index_.end());
            logs_index_.push_back(LogEntry{instance_id, name, file.value("modified_time", 0LL)});
        } else if (action == "updated") {
            auto it = std::find_if(logs_index_.begin(), logs_index_.end(), [&](const LogEntry& entry) {
                return entry.instance_id == instance_id && entry.name == name;
            });
            if (it != logs_index_.end()) {
                it->modified_time = file.value("modified_time", it->modified_time);
            }
        } else if (action == "deleted") {
            logs_index_.erase(std::remove_if(logs_index_.begin(),
                                             logs_index_.end(),
                                             [&](const LogEntry& entry) {
                                                 return entry.instance_id == instance_id && entry.name == name;
                                             }),
                              logs_index_.end());
        }
    }
}

void FileRequestHandler::RefreshLogsList(NatsManager& nats_manager, const std::string& instance_id) {
    nlohmann::json response = RequestMathCore(
        nats_manager, instance_id, "LogsList.Request", nlohmann::json::object(), "LogsList " + instance_id);
    if (!response.contains("files") || !response["files"].is_array()) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    logs_index_.erase(std::remove_if(logs_index_.begin(),
                                     logs_index_.end(),
                                     [&](const LogEntry& entry) { return entry.instance_id == instance_id; }),
                      logs_index_.end());
    for (const auto& file : response["files"]) {
        if (file.is_string()) {
            logs_index_.push_back(LogEntry{instance_id, file.get<std::string>(), 0});
        } else {
            logs_index_.push_back(LogEntry{instance_id, file.value("name", ""), file.value("modified_time", 0LL)});
        }
    }
}

nlohmann::json FileRequestHandler::RequestMathCore(NatsManager& nats_manager,
                                                   const std::string& instance_id,
                                                   const std::string& subject_suffix,
                                                   nlohmann::json payload,
                                                   const std::string& request_name) {
    const std::string request_id = GenerateRequestId();
    const std::string response_subject = "MathCore.Response." + request_id;
    payload["request_id"] = request_id;

    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    auto done = std::make_shared<std::atomic<bool>>(false);

    if (!nats_manager.Subscribe(response_subject, [promise, done](const std::string&, const nlohmann::json& message) {
            if (done->exchange(true)) {
                return;
            }
            TrySetPromise(promise, message);
        })) {
        return {{"error", "Failed to subscribe to response subject"}};
    }

    const std::string subject = "MathCore." + instance_id + "." + subject_suffix;
    if (!nats_manager.Publish(subject, payload)) {
        nats_manager.Unsubscribe(response_subject);
        return {{"error", "Failed to publish MathCore request"}};
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(61);
    while (!stopping_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
            nlohmann::json response = future.get();
            nats_manager.Unsubscribe(response_subject);
            return response;
        }
    }

    if (stopping_.load(std::memory_order_acquire)) {
        nats_manager.Unsubscribe(response_subject);
        return {{"error", "Application shutting down"}};
    }

    if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        nats_manager.Unsubscribe(response_subject);
        logger::log_error() << "Timeout waiting for " << request_name << std::endl;
        return {{"error", "MathCore timeout reached"}};
    }

    nlohmann::json response = future.get();
    nats_manager.Unsubscribe(response_subject);
    return response;
}

std::string FileRequestHandler::GetQueryParam(const std::string& uri, const std::string& key) {
    try {
        Poco::URI parsed(uri);
        for (const auto& param : parsed.getQueryParameters()) {
            if (param.first == key) {
                return param.second;
            }
        }
    } catch (const std::exception& e) {
        logger::log_error() << "Failed to parse URI parameter " << key << ": " << e.what() << std::endl;
    }
    return "";
}

int FileRequestHandler::ParseQueryNumber(const std::string& uri) {
    for (const std::string& key : {"queue", "numTicket", "num"}) {
        const std::string value = GetQueryParam(uri, key);
        if (!value.empty()) {
            try {
                return std::stoi(value);
            } catch (const std::exception&) {
                return 0;
            }
        }
    }
    return 0;
}

nlohmann::json FileRequestHandler::ParseLogsListOptions(const std::string& uri) {
    nlohmann::json options = {{"sort", "name"}, {"order", "desc"}};
    for (const auto& key : {"sort", "order", "instance", "instance_order"}) {
        const std::string value = GetQueryParam(uri, key);
        if (!value.empty()) {
            options[key] = value;
        }
    }
    if (options["sort"] != "name" && options["sort"] != "date") return {{"error", "invalid sort value"}};
    if (options["order"] != "asc" && options["order"] != "desc") return {{"error", "invalid order value"}};
    if (options.contains("instance_order") && options["instance_order"] != "asc" &&
        options["instance_order"] != "desc") {
        return {{"error", "invalid instance_order value"}};
    }
    return options;
}

bool FileRequestHandler::StartMathCoreSubscriptions(NatsManager& nats_manager) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (subscriptions_active_) {
        return true;
    }

    bool ok = true;
    ok = nats_manager.Subscribe("MathCore.Register.Request",
                                [&nats_manager](const std::string&, const nlohmann::json& message) {
                                    FileRequestHandler::OnRegisterRequest(nats_manager, message);
                                }) &&
         ok;
    ok = nats_manager.Subscribe("MathCore.*.Heartbeat",
                                [&nats_manager](const std::string&, const nlohmann::json& message) {
                                    FileRequestHandler::OnHeartbeat(nats_manager, message);
                                }) &&
         ok;
    ok = nats_manager.Subscribe("MathCore.*.Task.*",
                                [&nats_manager](const std::string& subject, const nlohmann::json& message) {
                                    FileRequestHandler::OnTaskEvent(nats_manager, subject, message);
                                }) &&
         ok;
    ok = nats_manager.Subscribe("MathCore.*.Logs.Updated", [](const std::string&, const nlohmann::json& message) {
        FileRequestHandler::OnLogsUpdated(message);
    }) && ok;
    subscriptions_active_ = ok;
    if (ok && !health_monitor_scheduled_) {
        health_monitor_scheduled_ = true;
        StartBackgroundWorker([]() {
            while (!FileRequestHandler::stopping_.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> worker_lock(FileRequestHandler::workers_mutex_);
                const bool stopping = FileRequestHandler::workers_cv_.wait_for(
                    worker_lock, FileRequestHandler::settings_.mathcore_health_check_interval, []() {
                        return FileRequestHandler::stopping_.load(std::memory_order_acquire);
                    });
                worker_lock.unlock();
                if (stopping) {
                    return;
                }

                std::lock_guard<std::mutex> state_lock(FileRequestHandler::state_mutex_);
                FileRequestHandler::MarkUnavailableInstancesLocked();
            }
        });
    }
    if (ok && !assignment_grace_dispatch_scheduled_) {
        assignment_grace_dispatch_scheduled_ = true;
        StartBackgroundWorker([&nats_manager]() {
            std::unique_lock<std::mutex> worker_lock(FileRequestHandler::workers_mutex_);
            FileRequestHandler::workers_cv_.wait_until(worker_lock, FileRequestHandler::assignment_enabled_at_, []() {
                return FileRequestHandler::stopping_.load(std::memory_order_acquire);
            });
            worker_lock.unlock();
            if (FileRequestHandler::stopping_.load(std::memory_order_acquire)) {
                return;
            }
            std::lock_guard<std::mutex> lock(FileRequestHandler::state_mutex_);
            FileRequestHandler::DispatchPendingLocked(nats_manager);
        });
    }
    return ok;
}

void FileRequestHandler::ResetMathCoreSubscriptions() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    subscriptions_active_ = false;
}

void FileRequestHandler::handleRequest(Poco::Net::HTTPServerRequest& request, Poco::Net::HTTPServerResponse& response) {
    response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
    response.setContentType("application/json");

    std::ostream& ostr = response.send();
    try {
        const std::string uri = request.getURI();
        if (uri.find("/start") == 0) {
            HandleStart(request, ostr);
        } else if (uri.find("/state") == 0) {
            HandleState(ostr, ParseQueryNumber(uri));
        } else if (uri.find("/logslist") == 0 || uri.find("/loglist") == 0) {
            nlohmann::json options = ParseLogsListOptions(uri);
            if (options.contains("error")) {
                ostr << options.dump();
            } else {
                HandleLogsList(ostr, options);
            }
        } else if (uri.find("/getlog") == 0) {
            const std::string id = GetQueryParam(uri, "id");
            if (id.empty()) {
                ostr << nlohmann::json({{"error", "invalid or missing id"}}).dump();
            } else {
                HandleGetLog(ostr, id, GetQueryParam(uri, "instance"));
            }
        } else if (uri.find("/getqueue") == 0) {
            HandleGetQueue(ostr);
        } else if (uri.find("/stopcalculation") == 0) {
            std::string id = GetQueryParam(uri, "id");
            if (id.empty()) {
                const int queue = ParseQueryNumber(uri);
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto entry = FindQueueEntryByNumberLocked(queue);
                if (entry.has_value()) {
                    id = entry->task_id;
                }
            }
            if (id.empty()) {
                ostr << nlohmann::json({{"error", "invalid or missing id/queue"}}).dump();
            } else {
                HandleStopCalculation(ostr, id);
            }
        } else if (uri.find("/natslogslist") == 0 || uri.find("/natsloglist") == 0) {
            HandleNatsLogsList(ostr);
        } else if (uri.find("/natsgetlog") == 0) {
            HandleNatsGetLog(ostr, GetQueryParam(uri, "id"));
        } else {
            ostr << nlohmann::json({{"error", "unknown command"}}).dump();
        }
    } catch (const std::exception& e) {
        logger::log_error() << "Unhandled HTTP handler exception: " << e.what() << std::endl;
        ostr << nlohmann::json({{"error", "internal error"}, {"details", e.what()}}).dump();
    }
}

void FileRequestHandler::HandleStart(Poco::Net::HTTPServerRequest& request, std::ostream& ostr) {
    std::ostringstream body;
    body << request.stream().rdbuf();
    if (body.str().empty()) {
        ostr << nlohmann::json({{"error", "Message is empty"}}).dump();
        return;
    }

    try {
        nlohmann::json message = nlohmann::json::parse(body.str());
        const std::string task_id = RequestIdBuilder().Build(message);
        int queue_number = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            queue_number = AllocateQueueNumberLocked();
            const std::int64_t now = NowEpochMilliseconds();
            QueueEntry entry;
            entry.queue_number = queue_number;
            entry.task_id = task_id;
            entry.payload = message;
            entry.created_epoch_ms = now;
            entry.updated_epoch_ms = now;
            entry.state = "pending";
            queue_entries_[queue_number] = std::move(entry);
            task_queue_numbers_[task_id] = queue_number;
            PersistQueueStateLocked();
            DispatchPendingLocked(nats_manager_);
        }
        ostr << GenerateResponse(queue_number, task_id, Status::Ok, "BUFFERED").dump();
    } catch (const std::exception& e) {
        logger::log_error() << "Invalid JSON in Start request: " << e.what() << std::endl;
        ostr << GenerateErrorResponse(0, "Invalid JSON body").dump();
    }
}

void FileRequestHandler::HandleState(std::ostream& ostr, int queue_number) {
    QueueEntry entry;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto found = FindQueueEntryByNumberLocked(queue_number);
        if (!found.has_value()) {
            ostr << GenerateResponse(queue_number, "", Status::Error, "Wrong query number").dump();
            return;
        }
        TouchQueueEntryLocked(found->task_id);
        entry = *FindQueueEntryByNumberLocked(queue_number);
    }

    if (entry.instance_id.empty() || entry.state == "pending") {
        ostr << GenerateResponse(queue_number, entry.task_id, Status::Ok, "Task in connector queue").dump();
        return;
    }
    if (entry.state == "mathcore unavailable/restarted") {
        ostr << GenerateResponse(
                    queue_number, entry.task_id, Status::Error, "MathCore instance was restarted or unavailable")
                    .dump();
        return;
    }

    nlohmann::json response = RequestMathCore(nats_manager_,
                                              entry.instance_id,
                                              "State.Request." + entry.task_id,
                                              nlohmann::json::object(),
                                              "State " + entry.task_id);
    if (response.contains("message") || response.contains("error")) {
        nlohmann::json state;
        state["solutions"] = nlohmann::json::array();
        state["state"] = response.contains("error")
                           ? GenerateResponse(queue_number, entry.task_id, Status::Error, response["error"])
                           : GenerateResponse(queue_number, entry.task_id, Status::Ok, response["message"]);
        ostr << state.dump();
        return;
    }
    if (response.contains("state") && response["state"].is_object()) {
        response["state"]["query"] = queue_number;
    }
    ostr << response.dump();
}

void FileRequestHandler::HandleLogsList(std::ostream& ostr, const nlohmann::json& options) {
    std::vector<LogEntry> logs;
    bool include_instance_prefix = true;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        MarkUnavailableInstancesLocked();
        // Keep instance-qualified log names while unavailable MathCores are retained.
        // Prefixes can be removed only after release leaves exactly one registered instance.
        include_instance_prefix = instances_.size() != 1;
        logs = logs_index_;
    }

    const std::string instance_filter = options.value("instance", "");
    if (!instance_filter.empty()) {
        logs.erase(
            std::remove_if(
                logs.begin(), logs.end(), [&](const LogEntry& entry) { return entry.instance_id != instance_filter; }),
            logs.end());
    }

    const std::string sort_by = options.value("sort", "name");
    const std::string order = options.value("order", "desc");
    const bool asc = order == "asc";
    std::sort(logs.begin(), logs.end(), [&](const LogEntry& lhs, const LogEntry& rhs) {
        if (options.contains("instance_order") && lhs.instance_id != rhs.instance_id) {
            return options["instance_order"] == "asc" ? lhs.instance_id < rhs.instance_id
                                                      : lhs.instance_id > rhs.instance_id;
        }
        if (sort_by == "date" && lhs.modified_time != rhs.modified_time) {
            return asc ? lhs.modified_time < rhs.modified_time : lhs.modified_time > rhs.modified_time;
        }
        return asc ? lhs.name < rhs.name : lhs.name > rhs.name;
    });

    nlohmann::json response;
    response["files"] = nlohmann::json::array();
    for (const auto& entry : logs) {
        response["files"].push_back(include_instance_prefix ? entry.instance_id + "::" + entry.name : entry.name);
    }
    ostr << response.dump();
}

void FileRequestHandler::HandleGetLog(std::ostream& ostr, const std::string& id, const std::string& instance_filter) {
    std::string instance_id = instance_filter;
    std::string real_id = StripInstancePrefix(id, instance_id);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (instance_id.empty()) {
            auto task = FindQueueEntryByTaskIdLocked(real_id);
            if (task.has_value()) {
                instance_id = task->instance_id;
            }
        }
        if (instance_id.empty()) {
            auto it = std::find_if(
                logs_index_.begin(), logs_index_.end(), [&](const LogEntry& entry) { return entry.name == real_id; });
            if (it != logs_index_.end()) {
                instance_id = it->instance_id;
            }
        }
    }
    if (instance_id.empty()) {
        ostr << nlohmann::json({{"error", "Log file not found"}}).dump();
        return;
    }
    ostr << RequestMathCore(
                nats_manager_, instance_id, "GetLog.Request." + real_id, nlohmann::json::object(), "GetLog " + real_id)
                .dump();
}

void FileRequestHandler::HandleGetQueue(std::ostream& ostr) {
    nlohmann::json response;
    response["0. " + FormatSystemTime(std::chrono::system_clock::now())] = "Current time";
    response["1. " + FormatSystemTime(application_start_time_)] = "nats-connector restart time";
    response["2. Queue"] = nlohmann::json::array();
    response["3. MathCores"] = nlohmann::json::object();

    std::lock_guard<std::mutex> lock(state_mutex_);
    CleanupExpiredQueueEntriesLocked();
    MarkUnavailableInstancesLocked();
    if (queue_entries_.empty()) {
        response["2. Queue"].push_back("Queue empty");
    } else {
        std::vector<int> numbers;
        for (const auto& [number, _] : queue_entries_) numbers.push_back(number);
        std::sort(numbers.begin(), numbers.end());
        for (int number : numbers) {
            const auto& entry = queue_entries_[number];
            response["2. Queue"].push_back(std::to_string(number) + ": " + entry.task_id +
                                           ", assigned: " + (entry.instance_id.empty() ? "none" : entry.instance_id) +
                                           ", state: " + entry.state);
        }
    }

    for (const auto& [id, instance] : instances_) {
        response["3. MathCores"][id] = {
            {"Restart time", FormatEpochSeconds(instance.started_at)},
            {"State", instance.state},
            {"Current task",
             instance.current_task_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(instance.current_task_id)},
            {"Version", instance.version},
            {"Commit hash", instance.commit}};
    }
    ostr << response.dump();
}

void FileRequestHandler::HandleStopCalculation(std::ostream& ostr, const std::string& id) {
    std::string instance_id;
    const std::string task_id = StripInstancePrefix(id, instance_id);
    QueueEntry entry;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto found = FindQueueEntryByTaskIdLocked(task_id);
        if (!found.has_value()) {
            ostr << nlohmann::json({{"id", task_id}, {"status", "Not found"}}).dump();
            return;
        }
        TouchQueueEntryLocked(task_id);
        entry = *FindQueueEntryByTaskIdLocked(task_id);
        if (entry.instance_id.empty() || entry.state == "pending") {
            RemoveQueueEntryLocked(task_id);
            ostr << nlohmann::json({{"id", task_id}, {"status", "Removed from queue"}}).dump();
            return;
        }
        if (instance_id.empty()) {
            instance_id = entry.instance_id;
        }
    }

    nlohmann::json request;
    request["id"] = task_id;
    request["task_id"] = task_id;
    nlohmann::json response = RequestMathCore(
        nats_manager_, instance_id, "StopCalculation.Request." + task_id, request, "StopCalculation " + task_id);
    if (response.value("status", "") == "Not found") {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto found = FindQueueEntryByTaskIdLocked(task_id);
        if (found.has_value()) {
            QueueEntry& stored = queue_entries_[found->queue_number];
            stored.state = "not found on " + instance_id;
            stored.updated_epoch_ms = NowEpochMilliseconds();
            PersistQueueStateLocked();
        }
    }
    ostr << response.dump();
}

void FileRequestHandler::HandleNatsLogsList(std::ostream& ostr) {
    nlohmann::json response;
    response["files"] = nlohmann::json::array();
    const std::filesystem::path logs_dir("logs/nats-connector");
    if (std::filesystem::exists(logs_dir)) {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator
                 it(logs_dir, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             it != end;
             it.increment(ec)) {
            if (!ec && it->is_regular_file(ec)) {
                response["files"].push_back(it->path().stem().string());
            }
        }
    }
    ostr << response.dump();
}

void FileRequestHandler::HandleNatsGetLog(std::ostream& ostr, const std::string& id) {
    const std::filesystem::path logs_dir("logs/nats-connector");
    const std::string target = std::filesystem::path(id).stem().string();
    std::filesystem::path found;
    if (std::filesystem::exists(logs_dir)) {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator
                 it(logs_dir, std::filesystem::directory_options::skip_permission_denied, ec),
             end;
             it != end;
             it.increment(ec)) {
            if (!ec && it->is_regular_file(ec) && it->path().stem().string() == target) {
                found = it->path();
                break;
            }
        }
    }
    if (found.empty()) {
        ostr << nlohmann::json({{"error", "Log file not found"}}).dump();
        return;
    }
    ostr << PackLogFileToJson(found).dump();
}

nlohmann::json FileRequestHandler::PackLogFileToJson(const std::filesystem::path& log_path) {
    nlohmann::json response;
    response["filename"] = log_path.filename().string();
    response["lines"] = nlohmann::json::array();
    std::ifstream input(log_path);
    std::string line;
    while (std::getline(input, line)) {
        response["lines"].push_back(line);
    }
    return response;
}

int ServerApp::main(const std::vector<std::string>&) {
    AppSettings settings = LoadAppSettings();
    FileRequestHandler::Configure(settings);
    int port = 9000;

    NatsManager nats_manager;
    Poco::Net::ServerSocket svs(port);
    Poco::Net::HTTPServer srv(new FileRequestHandlerFactory(nats_manager), svs, new Poco::Net::HTTPServerParams);
    srv.start();
    logger::log() << "HTTP Server started on port " << port << std::endl;
    nats_manager.StartReconnectLoop(
        settings.nats_url,
        settings.nats_reconnect_interval,
        [&]() {
            logger::log() << "Connected to NATS server at " << settings.nats_url << std::endl;
            FileRequestHandler::StartMathCoreSubscriptions(nats_manager);
        },
        [&]() { FileRequestHandler::ResetMathCoreSubscriptions(); });

    waitForTerminationRequest();
    srv.stop();
    nats_manager.StopReconnectLoop();
    FileRequestHandler::ShutdownBackgroundWorkers();
    nats_manager.Disconnect();

    return Application::EXIT_OK;
}
