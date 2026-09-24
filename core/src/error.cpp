#include "rumi/rumi.hpp"

#include <cstdarg>
#include <cstdio>

namespace rumi {

std::string vformat_message(const char* fmt, std::va_list ap)
{
    // Avoid a heap allocation for ordinary messages. When the stack buffer is
    // too small, measure once and keep the full path or decoder detail.
    std::va_list again;
    va_copy(again, ap);
    char small[256];
    const int n = std::vsnprintf(small, sizeof small, fmt, ap);
    if (n < 0 || static_cast<std::size_t>(n) < sizeof small) {
        va_end(again);
        return n < 0 ? std::string(fmt)
                     : std::string(small, static_cast<std::size_t>(n));
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    std::vsnprintf(&out[0], out.size() + 1, fmt, again);
    va_end(again);
    return out;
}

std::string format_message(const char* fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    std::string out = vformat_message(fmt, ap);
    va_end(ap);
    return out;
}

std::unexpected<std::string> errf(const char* fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    std::string out = vformat_message(fmt, ap);
    va_end(ap);
    return std::unexpected(std::move(out));
}

}  // namespace rumi
