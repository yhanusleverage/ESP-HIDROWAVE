#ifndef DEBUG_CONFIG_H
#define DEBUG_CONFIG_H

// ===== CONFIGURAÇÕES DE DEBUG =====
#define ENABLE_SERIAL_DEBUG true
#define SERIAL_BAUD_RATE 115200
#define ENABLE_VERBOSE_LOGGING false

// Debug condicional
#if ENABLE_SERIAL_DEBUG
  #define HYDRO_DEBUG_PRINT(x) Serial.print(x)
  #define HYDRO_DEBUG_PRINTLN(x) Serial.println(x)
  #define HYDRO_DEBUG_PRINTF(format, ...) Serial.printf(format, ##__VA_ARGS__)
#else
  #define HYDRO_DEBUG_PRINT(x)
  #define HYDRO_DEBUG_PRINTLN(x)
  #define HYDRO_DEBUG_PRINTF(format, ...)
#endif

// Logging verboso
#if ENABLE_VERBOSE_LOGGING
  #define HYDRO_VERBOSE_PRINT(x) Serial.print(x)
  #define HYDRO_VERBOSE_PRINTLN(x) Serial.println(x)
#else
  #define HYDRO_VERBOSE_PRINT(x)
  #define HYDRO_VERBOSE_PRINTLN(x)
#endif

#endif // DEBUG_CONFIG_H 