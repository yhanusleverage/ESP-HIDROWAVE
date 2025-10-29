#ifndef CONFIG_UNIFIED_H
#define CONFIG_UNIFIED_H

#include <Arduino.h>

// ===== CONFIGURAÇÕES DE DEBUG =====
#define SERIAL_DEBUG_ENABLED true
#define SERIAL_BAUD_RATE 115200

// ===== MACROS DE DEBUG UNIFICADAS =====
#ifndef DEBUG_PRINTLN
    #define DEBUG_PRINTLN(x) if(SERIAL_DEBUG_ENABLED) Serial.println(x)
#endif

#ifndef DEBUG_PRINTF
    #define DEBUG_PRINTF(format, ...) if(SERIAL_DEBUG_ENABLED) Serial.printf(format, ##__VA_ARGS__)
#endif

#ifndef DEBUG_PRINT
    #define DEBUG_PRINT(x) if(SERIAL_DEBUG_ENABLED) Serial.print(x)
#endif

// ===== CONFIGURAÇÕES DE PERSISTÊNCIA =====
#define PREFERENCES_NAMESPACE "espnow_unified"
#define CONFIG_VERSION 2
#define CONFIG_FILE_PATH "/config.json"
#define WIFI_CONFIG_FILE "/wifi_config.json"

// ===== CONFIGURAÇÕES GERAIS =====
#define SYSTEM_VERSION "2.2"
#define FIRMWARE_VERSION "2.2.0"
#define DEVICE_NAME_PREFIX "ESP-HIDROWAVE"

// ===== CONFIGURAÇÕES ESP-NOW =====
#define ESPNOW_CHANNEL_DEFAULT 1
#define ESPNOW_MAX_PEERS 20
#define ESPNOW_MESSAGE_TIMEOUT 10000
#define ESPNOW_PEER_OFFLINE_TIMEOUT 120000  // 2 minutos
#define ESPNOW_PING_INTERVAL 30000          // 30 segundos
#define ESPNOW_CLEANUP_INTERVAL 60000       // 1 minuto

// ===== CONFIGURAÇÕES DE RELÉS =====
#define NUM_RELAYS 16
#define RELAY_TIMER_MAX 3600  // 1 hora em segundos
#define RELAY_SAFETY_TIMEOUT 7200  // 2 horas máximo

// ===== CONFIGURAÇÕES DE SENSORES =====
#define SENSOR_UPDATE_INTERVAL 5000        // 5 segundos
#define SENSOR_READ_TIMEOUT 2000           // 2 segundos
#define SENSOR_CALIBRATION_SAMPLES 10
#define SENSOR_ERROR_THRESHOLD 3

// ===== CONFIGURAÇÕES DE TEMPERATURA =====
#define TEMP_MIN_VALID 0.0
#define TEMP_MAX_VALID 100.0
#define TEMP_DEFAULT_CALIBRATION 0.0

// ===== CONFIGURAÇÕES DE PH =====
#define PH_MIN_VALID 0.0
#define PH_MAX_VALID 14.0
#define PH_DEFAULT_CALIBRATION 0.0
#define PH_CALIBRATION_LOW 4.0
#define PH_CALIBRATION_HIGH 7.0

// ===== CONFIGURAÇÕES DE TDS/EC =====
#define TDS_MIN_VALID 0.0
#define TDS_MAX_VALID 2000.0
#define TDS_DEFAULT_CALIBRATION 0.0
#define TDS_TO_EC_FACTOR 0.64

// ===== CONFIGURAÇÕES DE NÍVEL DE ÁGUA =====
#define WATER_LEVEL_MIN 10    // 10% mínimo
#define WATER_LEVEL_MAX 90    // 90% máximo
#define WATER_LEVEL_CRITICAL 5 // 5% crítico

// ===== CONFIGURAÇÕES DE LCD =====
#define LCD_I2C_ADDRESS 0x27
#define LCD_COLS 20
#define LCD_ROWS 4
#define LCD_UPDATE_INTERVAL 1000

// ===== CONFIGURAÇÕES DE I2C =====
#define I2C_SCAN_INTERVAL 30000  // 30 segundos
#define I2C_TIMEOUT 1000
#define PCF8574_DEFAULT_ADDRESS 0x20

// ===== CONFIGURAÇÕES DE WATCHDOG =====
#define WATCHDOG_TIMEOUT 30  // 30 segundos
#define MEMORY_CHECK_INTERVAL 60000  // 1 minuto
#define MIN_FREE_HEAP 10000  // 10KB mínimo

// ===== CONFIGURAÇÕES DE API =====
#define API_RETRY_ATTEMPTS 3
#define API_TIMEOUT 10000
#define API_RETRY_DELAY 2000

// ===== CONFIGURAÇÕES SUPABASE (OPCIONAL) =====
#ifdef ENABLE_SUPABASE
    #define SUPABASE_URL "https://mbrwdpqndasborhosewl.supabase.co"
    #define SUPABASE_ANON_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im1icndkcHFuZGFzYm9yaG9zZXdsIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDgxNDI3MzEsImV4cCI6MjA2MzcxODczMX0.ouRWHqrXv0Umk8SfbyGJoc-TA2vPaGDoC_OS-auj1-A"
    
    // Tabelas do banco de dados
    #define SUPABASE_ENVIRONMENT_TABLE "environment_data"
    #define SUPABASE_HYDRO_TABLE "hydro_measurements"
    #define SUPABASE_RELAY_TABLE "relay_commands"
    #define SUPABASE_STATUS_TABLE "device_status"
#endif

// ===== CONFIGURAÇÕES DE WEB SERVER (OPCIONAL) =====
#ifdef ENABLE_WEB_SERVER
    #define WEB_SERVER_PORT 80
    #define WEB_SOCKET_PORT 81
    #define WEB_SERVER_TIMEOUT 30000
    #define MAX_CLIENTS 4
#endif

// ===== CONFIGURAÇÕES DE THINGSPEAK (OPCIONAL) =====
#ifdef ENABLE_THINGSPEAK
    #define THINGSPEAK_CHANNEL_ID 0
    #define THINGSPEAK_API_KEY ""
    #define THINGSPEAK_UPDATE_INTERVAL 15000  // 15 segundos
#endif

// ===== CONFIGURAÇÕES DE MODOS DE OPERAÇÃO =====
// Descomente apenas UM modo por vez:
//#define MASTER_MODE    // Modo Master ESP-NOW
//#define SLAVE_MODE     // Modo Slave ESP-NOW
//#define STANDALONE_MODE // Modo independente (sem ESP-NOW)

// ===== CONFIGURAÇÕES DE FUNCIONALIDADES OPCIONAIS =====
// Descomente as funcionalidades desejadas:
//#define ENABLE_SUPABASE
//#define ENABLE_WEB_SERVER
//#define ENABLE_THINGSPEAK
//#define ENABLE_LCD_DISPLAY
//#define ENABLE_SENSORS
//#define ENABLE_RELAY_CONTROL
//#define ENABLE_WATER_LEVEL_SENSOR
//#define ENABLE_DHT_SENSOR
//#define ENABLE_PH_SENSOR
//#define ENABLE_TDS_SENSOR

// ===== CONFIGURAÇÕES DE PINOS (AJUSTAR CONFORME HARDWARE) =====
// Pinos I2C (padrão ESP32)
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

// Pinos de sensores (ajustar conforme hardware)
#define TEMP_SENSOR_PIN 4
#define PH_SENSOR_PIN 34
#define TDS_SENSOR_PIN 35
#define WATER_LEVEL_PIN 36
#define DHT_PIN 2

// Pinos de LEDs de status
#define STATUS_LED_PIN 2
#define ERROR_LED_PIN 4

// ===== CONFIGURAÇÕES DE TIMEOUTS =====
#define WIFI_CONNECT_TIMEOUT 30000
#define SENSOR_READ_TIMEOUT 2000
#define RELAY_COMMAND_TIMEOUT 5000
#define ESPNOW_SEND_TIMEOUT 1000

// ===== CONFIGURAÇÕES DE LIMITES =====
#define MAX_DEVICE_NAME_LENGTH 32
#define MAX_WIFI_SSID_LENGTH 32
#define MAX_WIFI_PASSWORD_LENGTH 64
#define MAX_RELAY_NAME_LENGTH 16
#define MAX_MESSAGE_SIZE 250

// ===== CONFIGURAÇÕES DE VALIDAÇÃO =====
#define VALIDATE_MAC_ADDRESS(mac) (mac != nullptr && mac[0] != 0xFF)
#define VALIDATE_RELAY_NUMBER(relay) (relay >= 0 && relay < NUM_RELAYS)
#define VALIDATE_CHANNEL(channel) (channel >= 1 && channel <= 14)

// ===== CONFIGURAÇÕES DE LOGGING =====
#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARNING 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_DEBUG 3

#ifndef LOG_LEVEL
    #define LOG_LEVEL LOG_LEVEL_INFO
#endif

#define LOG_ERROR(x) if(LOG_LEVEL >= LOG_LEVEL_ERROR) DEBUG_PRINTLN("[ERROR] " + String(x))
#define LOG_WARNING(x) if(LOG_LEVEL >= LOG_LEVEL_WARNING) DEBUG_PRINTLN("[WARNING] " + String(x))
#define LOG_INFO(x) if(LOG_LEVEL >= LOG_LEVEL_INFO) DEBUG_PRINTLN("[INFO] " + String(x))
#define LOG_DEBUG(x) if(LOG_LEVEL >= LOG_LEVEL_DEBUG) DEBUG_PRINTLN("[DEBUG] " + String(x))

// ===== CONFIGURAÇÕES DE COMPATIBILIDADE =====
// Para compatibilidade com projetos antigos
#ifndef CONFIG_H
    #define CONFIG_H
#endif

// ===== CONFIGURAÇÕES DE VERSÃO =====
#define CONFIG_MAJOR_VERSION 2
#define CONFIG_MINOR_VERSION 2
#define CONFIG_PATCH_VERSION 0

#endif // CONFIG_UNIFIED_H
