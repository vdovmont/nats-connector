#pragma once

#include <functional>
#include <string>

class INatsManager {
  public:
    virtual ~INatsManager() = default;

    // Connect to the NATS server
    virtual bool Connect(const std::string& serverUrl) = 0;

    // Publish a message to a subject
    virtual bool Publish(const std::string& subject, const std::string& message) = 0;

    // Subscribe to a subject with a message handler callback
    virtual bool Subscribe(const std::string& subject,
                           std::function<void(const std::string& subject, const std::string& message)> handler) = 0;

    // Disconnect from the NATS server
    virtual void Disconnect() = 0;
};