#pragma once

#include <string>
#include <functional>

class INatsSubscriber {
public:
    virtual ~INatsSubscriber() = default;

    // Connect to the NATS server
    virtual bool connect(const std::string& serverUrl) = 0;

    // Subscribe to a subject with a message handler callback
    virtual bool subscribe(const std::string& subject, std::function<void(const std::string& message)> handler) = 0;

    // Disconnect from the NATS server
    virtual void disconnect() = 0;
};