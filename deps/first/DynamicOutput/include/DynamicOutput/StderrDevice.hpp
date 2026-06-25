#ifndef UE4SS_REWRITTEN_STDERRDEVICE_HPP
#define UE4SS_REWRITTEN_STDERRDEVICE_HPP

// macOS stderr sink for the DynamicOutput system.
//
// SystemStringType on macOS is std::string (UTF-8 char), so receive() can
// call fputs/fprintf directly without any wide-to-narrow conversion.
// On Windows SystemStringType is std::wstring — guard with #ifndef WIN32 at
// the call site (UE4SSProgram.cpp) so this device is only registered on mac.

#include <cstdio>
#include <string>

#include <DynamicOutput/Common.hpp>
#include <DynamicOutput/Macros.hpp>
#include <DynamicOutput/OutputDevice.hpp>

namespace RC::Output
{
    // Writes every log line that passes through the UE4SS Output system to
    // stderr.  The launcher script redirects stderr to smoke-load.log, so
    // this makes the full UE4SS log visible even when iCloud blocks the
    // NewFileDevice from creating UE4SS.log.
    class StderrDevice : public OutputDevice
    {
      public:
        ~StderrDevice() override = default;

        auto receive(SystemStringViewType fmt) const -> void override
        {
            // m_formatter prepends a timestamp; call it for consistent output.
            auto formatted = m_formatter(fmt);

#ifndef WIN32
            // macOS/Linux: SystemStringType is std::string (UTF-8), direct write is safe.
            fputs(formatted.c_str(), stderr);
            // Ensure the line ends with a newline so log lines don't run together.
            if (formatted.empty() || formatted.back() != '\n')
            {
                fputc('\n', stderr);
            }
            fflush(stderr);
#else
            // Windows fallback (not expected to be used, but keeps the file
            // compilable on all platforms).
            fwprintf(stderr, L"%ls", formatted.c_str());
            fflush(stderr);
#endif
        }
    };

} // namespace RC::Output

#endif // UE4SS_REWRITTEN_STDERRDEVICE_HPP
