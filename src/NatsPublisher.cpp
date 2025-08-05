#include "NatsPublisher.h"

NatsPublisher::NatsPublisher() : conn_(nullptr) {}

NatsPublisher::~NatsPublisher() {
    disconnect();
}

bool NatsPublisher::connect(const std::string& serverUrl) {
    natsStatus status = natsConnection_ConnectTo(&conn_, serverUrl.c_str());
    if (status != NATS_OK) {
        std::cerr << "NATS connect failed: " << natsStatus_GetText(status) << "\n";
        return false;
    }
    return true;
}

bool NatsPublisher::publish(const std::string& subject, const std::string& message) {
    if (!conn_){
        std::cerr << "Not connected to NATS server.\n";
        return false;
    }

    natsStatus status = natsConnection_PublishString(conn_, subject.c_str(), message.c_str());
    if (status != NATS_OK) {
        std::cerr << "Publish failed: " << natsStatus_GetText(status) << "\n";
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