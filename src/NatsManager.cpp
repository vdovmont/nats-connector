#include "NatsManager.h"

NatsManager::NatsManager() : conn_(nullptr), sub_(nullptr), callback_([](const std::string& a, const std::string& b) {}) {}

NatsManager::~NatsManager() {
    disconnect();
}

bool NatsManager::connect(const std::string& serverUrl) {
    natsStatus status = natsConnection_ConnectTo(&conn_, serverUrl.c_str());
    if (status != NATS_OK) {
        std::cerr << "NATS connect failed: " << natsStatus_GetText(status) << "\n";
        return false;
    }
    return true;
}

bool NatsManager::publish(const std::string& subject, const std::string& message) {
    if (!conn_) {
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

bool NatsManager::subscribe(const std::string& subject, std::function<void(const std::string&, const std::string&)> handler) {
    if (!conn_) {
        std::cerr << "Not connected to NATS server.\n";
        return false;
    }

    callback_ = handler;

    natsStatus status = natsConnection_Subscribe(&sub_, conn_, subject.c_str(), onMessage, this);
    // We pass 'this' so that, inside the callback, we can get back the current object instance (NatsSubscriber*).
    // This gives us access to private member like callback_ or other methods.
    // for more information - google "bridging C callbacks with C++ member functions".
    // Basically in our case - it allows us pass down whatever function we want to deal with the messages.
    if (status != NATS_OK) {
        std::cerr << "Subscribe failed: " << natsStatus_GetText(status) << "\n";
        return false;
    }

    return true;
}

void NatsManager::onMessage(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure) {
    // Retrieve the NatsSubscriber instance from the closure pointer.
    NatsManager* self = static_cast<NatsManager*>(closure);
    // If the instance and callback are valid, invoke the callback with the message data.
    if (self && self->callback_) {
        std::string subject = natsMsg_GetSubject(msg);
        std::string data(natsMsg_GetData(msg), natsMsg_GetDataLength(msg));
        self->callback_(subject, data);
    }
    natsMsg_Destroy(msg);  // This is default nats behavior - to destroy the message after processing.
}

void NatsManager::disconnect() {
    if (conn_) {
        natsConnection_Destroy(conn_);
        conn_ = nullptr;
    }
    if (sub_) {
        natsSubscription_Destroy(sub_);
        sub_ = nullptr;
    }
}