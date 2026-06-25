#include "darwin/log.hpp"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <mutex>
namespace mac {
static std::string g_path;
static std::mutex  g_mtx;

void log_init(const char* tag) {
    const char* home = getenv("HOME");           // 샌드박스 안 = 컨테이너 Data 경로
    g_path = std::string(home ? home : ".") + "/ue4ss-mac-spike.log";
    logf("==== %s start (HOME=%s) ====", tag, home ? home : "(null)");
}
const std::string& log_path() { return g_path; }

void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_mtx);
    char buf[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "%s\n", buf); fflush(stderr);
    if (!g_path.empty()) { if (FILE* f = fopen(g_path.c_str(), "a")) { fprintf(f, "%s\n", buf); fclose(f); } }
}
}
