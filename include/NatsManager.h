#pragma once

#include "INatsManager.h"
#include <nats/nats.h>
#include <functional>
#include <iostream>

class NatsManager : public INatsManager {
public:
    NatsManager();
    ~NatsManager() override;

    bool connect(const std::string& serverUrl) override;
    bool publish(const std::string& subject, const std::string& message) override;
    bool subscribe(const std::string& subject, std::function<void(const std::string& subject, const std::string& message)> handler) override;
    void disconnect() override;

private:
    natsConnection* conn_;
    natsSubscription* sub_;
    std::function<void(const std::string&, const std::string&)> callback_;

    static void onMessage(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure);
};