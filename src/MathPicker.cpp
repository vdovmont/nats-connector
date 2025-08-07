#include "MathPicker.h"

bool MathPicker::initialize(const std::string& serverUrl, const std::string& subject) {
    bool status = manager_.connect(serverUrl);
    if (!status) {
        return false;
    }

    status = manager_.subscribe(subject, [this](const std::string& msgSubject, const std::string& message) { this->onMessageTrigger(msgSubject, message); });
    if (!status) {
        return false;
    }

    return true;
}

void MathPicker::onMessageTrigger(const std::string& subject, const std::string& message) {
    std::string responseSubject = "Answers.";
    responseSubject += subject; // so in theory it should be something like "Answers.Clients.SomeClient"

    std::string responseMessage = "Your ticket is: someNumber, and message is:\n"; // Placeholder for actual response logic
    responseMessage += message;
    responseMessage += "\n";
    if (!manager_.publish(responseSubject, responseMessage)) {
        std::cerr << "Failed to return response message to subject " << responseSubject << "\n";
        return;
    }

    // Here we can implement the logic to select the appropriate math functionality based on the message received.
    // Maybe it can be even new method - for future discussion.
    std::cout << "Math selector functionality not implemented yet.\n"; // Placeholder for future functionality
}