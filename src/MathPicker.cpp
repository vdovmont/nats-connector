#include "MathPicker.h"

bool MathPicker::initialize(const std::string& serverUrl, const std::string& subject) {
    bool status = publisher_.connect(serverUrl);
    if (!status) {
        return false;
    }
    status = subscriber_.connect(serverUrl);
    if (!status) {
        return false;
    }

    status = subscriber_.subscribe(subject, [this](const std::string& msgSubject, const std::string& message) { this->onMessageTrigger(msgSubject, message); });
    if (!status) {
        return false;
    }

    return true;
}

void MathPicker::onMessageTrigger(const std::string& subject, const std::string& message) {
    std::string responseSubject = "Answers.";
    responseSubject += subject; // so in theory it should be something like "Answers.Clients.SomeClient"

    std::string responseMessage = "Your ticket is: someNumber.\n"; // Placeholder for actual response logic
    if (!publisher_.publish(responseSubject, responseMessage)) {
        std::cerr << "Failed to return response message.\n";
        return;
    }

    // Here we can implement the logic to select the appropriate math functionality based on the message received.
    // Maybe it can be even new method - for future discussion.
    std::cout << "Math selector functionality not implemented yet.\n"; // Placeholder for future functionality
}