#pragma once

#include <fstream>
#include <iostream>
#include <ostream>
#include <string>

#include "nlohmann/json.hpp"

namespace logger {

constexpr int json_indent = 4;

class LogStream {
  public:
    explicit LogStream(const std::string& filename, std::ostream& stream = std::cout);
    ~LogStream();

    template <typename T>
    LogStream& operator<<(const T& value) {
        WritePrefixIfNeeded();
        (*stream_) << value;
        if (file_.is_open()) {
            file_ << value;
        }
        return *this;
    }

    LogStream& operator<<(std::ostream& (*manip)(std::ostream&));

  private:
    void WritePrefixIfNeeded();

    std::ofstream file_;
    std::ostream* stream_;
    bool at_line_start_ = true;
};

// Usage: log("path/to/file.txt") << "value" << std::endl;
LogStream log(const std::string& filename);
LogStream log();
LogStream log_error(const std::string& filename);
LogStream log_error();

}  // namespace logger