#include "StderrCapture.h"

StderrCapture::StderrCapture() {
    testing::internal::CaptureStderr();
}

StderrCapture::~StderrCapture() {
    captured_output_ = testing::internal::GetCapturedStderr();
}

const std::string& StderrCapture::output() const {
    return captured_output_;
}