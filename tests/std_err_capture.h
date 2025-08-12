#pragma once

#include <gtest/gtest.h>

#include <string>

class StderrCapture {
  public:
    StderrCapture();
    ~StderrCapture();

    const std::string& Output() const;

  private:
    std::string captured_output_;
};