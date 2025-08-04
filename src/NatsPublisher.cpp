#include "NatsPublisher.h"

NatsPublisher::NatsPublisher() : conn_(nullptr) {}

NatsPublisher::~NatsPublisher() {
    disconnect();
}

bool NatsPublisher::connect(const std::string& serverUrl) {
    natsStatus s = natsConnection_ConnectTo(&conn_, serverUrl.c_str());
    if (s != NATS_OK) {
        std::cerr << "NATS connect failed: " << natsStatus_GetText(s) << "\n";
        return false;
    }
    return true;
}

bool NatsPublisher::publish(const std::string& subject, const std::string& message) {
    if (!conn_) return false;

    natsStatus s = natsConnection_PublishString(conn_, subject.c_str(), message.c_str());
    if (s != NATS_OK) {
        std::cerr << "Publish failed: " << natsStatus_GetText(s) << "\n";
        return false;
    }
    return true;
}

void NatsPublisher::disconnect() {
    if (conn_) {
        natsConnection_Destroy(conn_);
        conn_ = nullptr;
    }
}