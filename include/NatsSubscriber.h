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

    // i'm not sure if we need these methods in current class - maybe it would be better to create another child and move methods there. for discussion.
    // my reason is that current class should be only focused on nats functionality, so anything extra should be moved elsewhere.
    bool initialize(const std::string& serverUrl, const std::string& subject);
    void mathSelecter(const std::string& message);

private:
    natsConnection* conn_;
    natsSubscription* sub_;
    std::function<void(const std::string&)> callback_;
    // used in case we need for different onMessage implementations (e.g. like mathSelecter).
    // if we don't plan to have multiple implementations, we can remove this and use onMessage directly as method for choosing MathCore functionality.
    // in later case, subscribe method should also be changed to have 'subject' as an only parameter (in this and Interface classes).

    static void onMessage(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure);
};