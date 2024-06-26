#include "debug.hpp"
namespace DiStore::Debug {
    auto info(const char *fmt, ...) -> void {
#ifdef __INFO__
        va_list args;
        va_start(args, fmt);
        // green
        printf("\x1B[32m[[ Info: \x1B[0m");
        vprintf(fmt, args);
        va_end(args);
#endif
    }

    auto warn(const char *fmt, ...) -> void {
        va_list args;
        va_start(args, fmt);
        // yellow
        printf("\x1B[33m[[ Warning: \x1B[0m");
        vprintf(fmt, args);
        va_end(args);
    }

    auto error(const char *fmt, ...) -> void {
        va_list args;
        va_start(args, fmt);
        // red
        fprintf(stderr, "\x1B[31m[[ Error: \x1B[0m");
        vfprintf(stderr, fmt, args);
        va_end(args);
    }
}
