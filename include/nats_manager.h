#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>

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
    bool Publish(const std::string& subject, const nlohmann::json& message);
    bool Subscribe(const std::string& subject,
                   std::function<void(const std::string& subject, const nlohmann::json& message)> handler);
    bool Unsubscribe(const std::string& subject);
    void Disconnect();

    // for testing purposes
    natsConnection* get_connection() const { return conn_; }

  private:
    natsConnection* conn_;
    std::unordered_map<std::string, natsSubscription*> subs_;
    std::unordered_map<natsSubscription*, std::function<void(const std::string&, const nlohmann::json&)>> callbacks_;

    static void Callback(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure);
};