#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nats.h"
#include "nlohmann/json.hpp"

struct MsgGuard {
    natsMsg* m;
    ~MsgGuard() {
        if (m) natsMsg_Destroy(m);
    }
};

class NatsManager {
  public:
    NatsManager();
    ~NatsManager();

    bool Connect(const std::string& server_url);
    bool IsConnected();
    bool CheckConnection();
    void StartReconnectLoop(const std::string& server_url,
                            std::function<void()> on_connected = {},
                            std::function<void()> on_disconnected = {});
    void StopReconnectLoop();
    bool Publish(const std::string& subject, const nlohmann::json& message);
    bool Subscribe(const std::string& subject,
                   std::function<void(const std::string& subject, const nlohmann::json& message)> handler);
    bool Unsubscribe(const std::string& subject);
    void Disconnect();

    // for testing purposes
    natsConnection* get_connection() const { return conn_; }

  private:
    void DisconnectLocked(std::vector<natsSubscription*>& subs_to_destroy, natsConnection*& conn_to_destroy);
    void FinalizeDisconnect(std::vector<natsSubscription*>& subs_to_destroy, natsConnection* conn_to_destroy);
    void WaitForCallbacks();

    natsConnection* conn_;
    std::atomic<bool> connected_;
    mutable std::mutex conn_mutex_;
    std::atomic<bool> destroying_{false};
    std::mutex callback_mutex_;
    std::condition_variable callback_cv_;
    size_t active_callbacks_{0};
    std::atomic<bool> stop_reconnect_{false};
    std::thread reconnect_thread_;
    std::string server_url_;
    std::unordered_map<std::string, natsSubscription*> subs_;
    std::unordered_map<natsSubscription*, std::function<void(const std::string&, const nlohmann::json&)>> callbacks_;

    static void Callback(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure);
};