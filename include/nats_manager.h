#pragma once

#include <nats/nats.h>

#include <functional>
#include <iostream>

#include "i_nats_manager.h"

class NatsManager : public INatsManager {
  public:
    NatsManager();
    ~NatsManager() override;

    bool Connect(const std::string& serverUrl) override;
    bool Publish(const std::string& subject, const std::string& message) override;
    bool Subscribe(const std::string& subject,
                   std::function<void(const std::string& subject, const std::string& message)> handler) override;
    void Disconnect() override;

    // for testing purposes
    natsConnection* get_connection() const { return conn_; }
    natsSubscription* get_subscription() const { return sub_; }

  private:
    natsConnection* conn_;
    natsSubscription* sub_;
    std::function<void(const std::string&, const std::string&)> callback_;

    static void Callback(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure);
};