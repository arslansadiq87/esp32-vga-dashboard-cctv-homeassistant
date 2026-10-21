// The pioarduino ESP32-S3 linker wraps Arduino's log_printf symbol, but the
// Arduino core archive does not provide the corresponding __wrap entry point.
// Forward the wrapped variadic call to the core's existing va_list formatter.
#include <stdarg.h>

extern "C" int log_printfv(const char *format, va_list args);

extern "C" int __wrap_log_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int result = log_printfv(format, args);
  va_end(args);
  return result;
}
