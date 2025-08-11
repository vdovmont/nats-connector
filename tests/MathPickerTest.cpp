#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "MathPicker.h"
#include "NatsManager.h"
#include "StderrCapture.h"

class MathPickerIntegrationTest : public ::testing::Test {
   protected:
    StderrCapture stderr_capture_;
    MathPicker math_picker_;
    NatsManager nats_;
    std::string server_url = "nats://localhost:4222";
    std::string test_channel = "test.subject";

    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(MathPickerIntegrationTest, Initialize) {
    ASSERT_TRUE(math_picker_.initialize(server_url, test_channel)) << "Failed to initialize with error:\n" << stderr_capture_.output();
}

TEST_F(MathPickerIntegrationTest, onMessageTrigger) {
    std::string received_message;
    std::string responseMessage = "Your ticket is: someNumber, and message is:\n";
    std::string sending_message = "something";

    ASSERT_TRUE(nats_.connect(server_url)) << "Failed to connect to NATS. Stderr:\n" << stderr_capture_.output();
    ASSERT_TRUE(nats_.subscribe("Answers." + test_channel, [&](const std::string& subject, const std::string& message) { received_message = message; }))
        << "Failed to subscribe. Stderr:\n"
        << stderr_capture_.output();

    ASSERT_TRUE(math_picker_.initialize(server_url, test_channel)) << "Failed to initialize with error:\n" << stderr_capture_.output();
    math_picker_.onMessageTrigger(test_channel, sending_message);

    // Since subscribe callbacks are async, wait a little to receive the message.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(received_message, responseMessage + sending_message + "\n");
}