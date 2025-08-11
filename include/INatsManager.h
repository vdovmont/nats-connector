#pragma once

#include <functional>
#include <string>

class INatsManager {
   public:
    virtual ~INatsManager() = default;

    // Connect to the NATS server
    virtual bool connect(const std::string& serverUrl) = 0;

    // Publish a message to a subject
    virtual bool publish(const std::string& subject, const std::string& message) = 0;

    // Subscribe to a subject with a message handler callback
    virtual bool subscribe(const std::string& subject, std::function<void(const std::string& subject, const std::string& message)> handler) = 0;

    // Disconnect from the NATS server
    virtual void disconnect() = 0;
};