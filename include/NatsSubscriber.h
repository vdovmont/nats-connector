#pragma once

#include "INatsSubscriber.h"
#include <nats/nats.h>
#include <functional>
#include <iostream>

class NatsSubscriber : public INatsSubscriber {
public:
    NatsSubscriber();
    ~NatsSubscriber();

    bool connect(const std::string& serverUrl) override;
    bool subscribe(const std::string& subject, std::function<void(const std::string& message)> handler) override;
    void disconnect() override;

private:
    natsConnection* conn_;
    natsSubscription* sub_;
    std::function<void(const std::string&)> callback_;

    static void onMessage(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure);
};