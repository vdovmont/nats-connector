#pragma once

#include "INatsPublisher.h"
#include <nats/nats.h>
#include <iostream>

class NatsPublisher : public INatsPublisher {
public:
    NatsPublisher();
    ~NatsPublisher() override;

    bool connect(const std::string& serverUrl) override;
    bool publish(const std::string& subject, const std::string& message) override;
    void disconnect() override;

private:
    natsConnection* conn_;
};