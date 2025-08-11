#include <chrono>
#include <thread>
#include "MathPicker.h"

// For now as a stopgap, but this function should prevent process from exiting and maybe checking for NatsSubscriber health (connection) (?).
// In the future should be replaced.
void sleepingFunction() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

int main() {
    std::string serverUrl = "nats://localhost:4222";  //example server URL
    std::string clientsChannel = "Clients.*";         //example of common channel for clients
    MathPicker mathPicker;

    bool status = mathPicker.initialize(serverUrl, clientsChannel);

    if (!status) {
        std::cerr << "Failed to initialize NATS service.\n";
        return 1;
    } /*else { sleepingFunction(); // commented for the sake of testing (next block of code). otherwise here should be called a function for keeping the process alive.
    }*/

    // for testing purposes
    // testing should be done if we already have nats-service running up
    NatsManager testingObject;
    std::string someClient = "Clients.SomeClient";
    std::string someClientAnswers = "Answers.Clients.SomeClient";
    testingObject.connect(serverUrl);
    testingObject.subscribe(someClientAnswers, [](const std::string& subject, const std::string& message) {
        std::cout << "Received message on subject " << subject << ": " << message << "\n";
    });
    testingObject.publish(someClient, "Hello from NatsPublisher!");

    sleepingFunction();

    return 0;
}