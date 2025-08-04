#pragma once

#include <string>

class INatsPublisher {
public:
    virtual ~INatsPublisher() = default;

    // Connect to the NATS server
    virtual bool connect(const std::string& serverUrl) = 0;

    // Publish a message to a subject
    virtual bool publish(const std::string& subject, const std::string& message) = 0;

    // Disconnect from the NATS server
    virtual void disconnect() = 0;
};