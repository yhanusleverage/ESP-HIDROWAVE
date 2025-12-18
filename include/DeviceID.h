#ifndef DEVICE_ID_H
#define DEVICE_ID_H

#include <Arduino.h>

// ===== GENERACIÓN AUTOMÁTICA DE DEVICE_ID =====

/**
 * @brief Genera un device_id único basado en el MAC address del ESP32
 * @return String con formato "ESP32_HIDRO_XXXXXX" donde XXXXXX son los últimos 6 caracteres del MAC
 * 
 * Ejemplos:
 * - MAC: "24:6F:28:AB:CD:EF" → Device ID: "ESP32_HIDRO_ABCDEF"
 * - MAC: "30:AE:A4:12:34:56" → Device ID: "ESP32_HIDRO_123456"
 */
String generateDeviceID();

/**
 * @brief Obtiene el device_id del dispositivo (con cache)
 * @return String con el device_id generado automáticamente
 * 
 * Esta función mantiene el device_id en cache para evitar regenerarlo
 * en cada llamada. Es la función recomendada para obtener el device_id.
 */
String getDeviceID();

/**
 * @brief Obtiene solo el sufijo MAC (últimos 6 caracteres)
 * @return String con los últimos 6 caracteres del MAC address
 * 
 * Ejemplo: "ABCDEF" para MAC "24:6F:28:AB:CD:EF"
 */
String getMACsuffix();

/**
 * @brief Obtiene el MAC address completo del dispositivo
 * @return String con el MAC address en formato "XX:XX:XX:XX:XX:XX"
 */
String getFullMAC();

/**
 * @brief Información completa del dispositivo para logs/debug
 * @return String con información detallada del dispositivo
 */
String getDeviceInfo();

/**
 * @brief Fuerza la regeneración del device_id limpiando el cache
 */
void forceRegenerateDeviceID();

// ===== CONSTANTES =====
#define DEVICE_ID_PREFIX "ESP32_HIDRO_"
#define MAC_SUFFIX_LENGTH 6

#endif // DEVICE_ID_H
