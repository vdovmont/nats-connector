#pragma once

#include <functional>
#include <iostream>

#include "nats.h"
#include "nlohmann/json.hpp"

class NatsManager {
  public:
    NatsManager();
    ~NatsManager();

    bool Connect(const std::string& serverUrl);
    bool Publish(const std::string& subject, const nlohmann::json& message);
    bool Subscribe(const std::string& subject,
                   std::function<void(const std::string& subject, const nlohmann::json& message)> handler);
    void Disconnect();

    // for testing purposes
    natsConnection* get_connection() const { return conn_; }
    natsSubscription* get_subscription() const { return sub_; }

  private:
    natsConnection* conn_;
    natsSubscription* sub_;
    std::function<void(const std::string&, const nlohmann::json&)> callback_;

    static void Callback(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure);
};