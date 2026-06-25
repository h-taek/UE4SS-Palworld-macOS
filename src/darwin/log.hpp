#pragma once
#include <string>
namespace mac {
void log_init(const char* tag);
void logf(const char* fmt, ...) __attribute__((format(printf,1,2)));
const std::string& log_path();
}
