#include "std_err_capture.h"

StderrCapture::StderrCapture() { testing::internal::CaptureStderr(); }

StderrCapture::~StderrCapture() { captured_output_ = testing::internal::GetCapturedStderr(); }

const std::string& StderrCapture::Output() const { return captured_output_; }