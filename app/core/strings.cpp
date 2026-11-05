#include "core/strings.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>

std::string format(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int size = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string result(size > 0 ? size_t(size) : 0, '\0');
    if (size > 0) {
        std::vsnprintf(result.data(), result.size() + 1, fmt, args);
    }
    va_end(args);
    return result;
}

std::string toLower(std::string s)
{
    for (char& c : s) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool hasPrefix(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool hasSuffix(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
