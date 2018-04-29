#include "Log.h"
#include "Client.h"

void Log::log(Level /* level */, const std::string &string)
{
    assert(!string.empty());
    fwrite(string.c_str(), 1, string.size(), stderr);
    if (string.at(string.size() - 1) != '\n')
        fwrite("\n", 1, 1, stderr);
}

void Log::log(Level level, const char *fmt, va_list args)
{
    log(level, Client::vformat(fmt, args));
}

void Log::debug(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(Debug, fmt, args);
    va_end(args);
}

void Log::info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(Info, fmt, args);
    va_end(args);
}

void Log::warning(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(Warning, fmt, args);
    va_end(args);
}

void Log::error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(Error, fmt, args);
    va_end(args);
}
