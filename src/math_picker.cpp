#include "math_picker.h"

bool MathPicker::Initialize(const std::string& server_url, const std::string& subject) {
    bool status = manager_.Connect(server_url);
    if (!status) {
        return false;
    }

    status = manager_.Subscribe(subject, [this](const std::string& msg_subject, const std::string& message) {
        this->OnMessageTrigger(msg_subject, message);
    });
    if (!status) {
        return false;
    }

    return true;
}

void MathPicker::OnMessageTrigger(const std::string& subject, const std::string& message) {
    std::string response_subject = "Answers.";
    response_subject += subject;  // so in theory it should be something like "Answers.Clients.SomeClient"

    std::string response_message =
        "Your ticket is: someNumber, and message is:\n";  // Placeholder for actual response logic
    response_message += message;
    response_message += "\n";
    if (!manager_.Publish(response_subject, response_message)) {
        std::cerr << "Failed to return response message to subject " << response_subject << "\n";
        return;
    }

    // Here we can implement the logic to select the appropriate math functionality based on the message received.
    // Maybe it can be even new method - for future discussion.
    std::cout << "Math selector functionality not implemented yet.\n";  // Placeholder for future functionality
}