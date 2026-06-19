#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include "LCDConfig.h"
#include "DebugConfig.h"

// ===== CONFIGURAÇÕES DE ARQUIVOS =====
#define WIFI_CONFIG_FILE "/wifi_config.json"
#define CONFIG_FILE_PATH "/config.json"

// ===== CONFIGURAÇÕES GERAIS =====
#define SYSTEM_VERSION "2.1"
// DEVICE_ID será generado automáticamente usando MAC address
// Ver función generateDeviceID() en main.cpp
#define FIRMWARE_VERSION "2.1.0"

// Painel HTTP local na porta 80 (index.html + APIs). 0 = produção (Supabase + portal AP WiFi).
#ifndef ENABLE_LOCAL_ADMIN_HTTP
#define ENABLE_LOCAL_ADMIN_HTTP 0
#endif

// ===== MQTT (MVP telemetria) — secrets.ini: mqtt_enabled=0|1 =====
#ifndef ENABLE_MQTT
#define ENABLE_MQTT 0
#endif

#if ENABLE_MQTT
#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASS
#define MQTT_PASS ""
#endif
#ifndef MQTT_TELEMETRY_INTERVAL_MS
#define MQTT_TELEMETRY_INTERVAL_MS 30000UL
#endif
#ifndef MQTT_HEARTBEAT_INTERVAL_MS
#define MQTT_HEARTBEAT_INTERVAL_MS 60000UL
#endif
// 0 = bivalente: MQTT telemetry (hydro + environment) + HTTPS paralelo
// 1 = só MQTT; HTTPS hydro/environment só se MQTT desconectado (fallback procedural)
#ifndef MQTT_HYDRO_ONLY
#define MQTT_HYDRO_ONLY 0
#endif
// 1 = saúde: MQTT heartbeat se broker OK; HTTPS device_status só se MQTT cair (igual hydro)
// 0 = bivalente: MQTT + HTTPS updateDeviceStatus em paralelo
#ifndef MQTT_HEALTH_ONLY
#define MQTT_HEALTH_ONLY 0
#endif
#endif

// ===== CONFIGURAÇÕES DA API =====
// Credenciais Supabase: definidas em secrets.ini (ver secrets.ini.example)
#ifndef SUPABASE_URL
#error "SUPABASE_URL não definido. Copie secrets.ini.example para secrets.ini e preencha as credenciais."
#endif
#ifndef SUPABASE_ANON_KEY
#error "SUPABASE_ANON_KEY não definido. Copie secrets.ini.example para secrets.ini e preencha as credenciais."
#endif

// Tabelas do banco de dados
#define SUPABASE_ENVIRONMENT_TABLE "environment_data"
#define SUPABASE_HYDRO_TABLE "hydro_measurements"
#define SUPABASE_RELAY_TABLE "relay_commands"
#define SUPABASE_STATUS_TABLE "device_status"

// Configurações de API
#define API_RETRY_ATTEMPTS 3UL
#define SUPABASE_TIMEOUT_MS 7000       // ✅ Otimizado: 7s (reduzido de 10s para resposta mais rápida)
#ifndef COMMAND_POLL_INTERVAL_MS
#define COMMAND_POLL_INTERVAL_MS 10000  // fallback HTTPS quando MQTT offline (fase 3)
#endif
#ifndef COMMAND_POLL_INTERVAL_MQTT_OK_MS
#define COMMAND_POLL_INTERVAL_MQTT_OK_MS 60000UL  // backup lento se MQTT online
#endif
#ifndef COMMAND_POLL_INTERVAL_MQTT_DOWN_MS
#define COMMAND_POLL_INTERVAL_MQTT_DOWN_MS 10000UL
#endif

// Headers HTTP para Supabase
#define SUPABASE_CONTENT_TYPE "application/json"
#define SUPABASE_PREFER "return=minimal"

// ===== LIMITES DO SISTEMA =====
#define MAX_RELAYS 8   // Sistema Master ESP-NOW com 8 relés
#define MAX_SENSORS 8
#define MAX_RETRY_ATTEMPTS 3

// ===== CONFIGURAÇÕES ESP-NOW UNIFICADAS =====
#define ESPNOW_CHANNEL 1                    // Canal WiFi (1-14)
#define MAX_ESPNOW_PEERS 10                 // Máximo de peers ESP-NOW
#define MESSAGE_TIMEOUT_MS 300000            // Timeout para mensagens (5 minutos)
#define PEER_OFFLINE_TIMEOUT 60000          // Timeout para considerar peer offline (60s)
#define DISCOVERY_INTERVAL_MS 30000         // Intervalo de descoberta (30 segundos)
#define STATUS_BROADCAST_INTERVAL 30000    // Intervalo de broadcast de status (30s)

// ===== CONFIGURAÇÕES DE ROBUSTEZ ESP-NOW =====
#define MAX_RETRY_ATTEMPTS 3                // Máximo de tentativas de retry
#define RETRY_DELAY_MS 1000                 // Delay entre tentativas (1 segundo)
#define RETRY_BACKOFF_MULTIPLIER 2          // Multiplicador de backoff
#define CRITICAL_COMMAND_TIMEOUT 5000       // Timeout para comandos críticos (5s)
#define SIGNAL_QUALITY_THRESHOLD -70        // Threshold de qualidade de sinal (dBm)
#define PACKET_LOSS_THRESHOLD 0.1          // Threshold de perda de pacotes (10%)
#define HEALTH_CHECK_INTERVAL 10000         // Intervalo de verificação de saúde (10s)
#define RECOVERY_ATTEMPTS 3                 // Tentativas de recuperação

// ===== CONFIGURAÇÕES DE AUTOMAÇÃO ESP-NOW =====
#define AUTO_DISCOVERY_ENABLED true          // Habilitar descoberta automática
#define DISCOVERY_RETRY_INTERVAL 5000        // Intervalo entre tentativas (5s)
#define MAX_DISCOVERY_ATTEMPTS 10          // Máximo de tentativas
#define PEER_AUTO_RECONNECT true            // Reconexão automática
#define RECONNECT_INTERVAL 30000           // Intervalo de reconexão (30s)
#define TARGET_DEVICE_TYPE "RelayCommandBox" // Tipo de dispositivo alvo
#define CONNECTION_TIMEOUT 60000           // Timeout de conexão (60s)

// ===== PINOS DO HARDWARE =====
// Sensores
#define DHT_PIN 15                     // Sensor DHT22
#define DHT_TYPE DHT22                 // Tipo do sensor DHT
#define PH_PIN 35                      // Sensor de pH
#define TDS_PIN 34                     // Sensor TDS (Analógico)
#define TDS_RX_PIN 36                  // Sensor TDS (RX) - CORRIGIDO: pino ADC válido
#define TDS_TX_PIN 17                  // Sensor TDS (TX)
#define TEMP_PIN 4                     // Sensor de temperatura DS18B20
#define TANK_LOW_PIN 32                // Sensor de nível baixo (legacy GPIO)
#define TANK_HIGH_PIN 33               // Sensor de nível alto (legacy GPIO)

// ===== 4 SONDAS NPN VIA PCF8574 #1 (P0-P3) =====
#define LEVEL_DEBOUNCE_MS 300
#define LEVEL_NPN_ACTIVE_LOW 1         // NPN ON → LOW no PCF (após opto)
#define LEVEL_SENSOR_PCF_PINS 0, 1, 2, 3
#define WATER_TEMP_PIN 25              // Sensor de temperatura da água
#define WATER_LEVEL_NPN_PIN 32         // Sensor de nível NPN
#define WATER_LEVEL_PNP_PIN 33         // Sensor de nível PNP

// I2C - Barramento compartilhado
#define I2C_SDA 21
#define I2C_SCL 22

// Status LED
#define STATUS_LED_PIN 2               // LED de status (built-in)

// ===== ENDEREÇOS I2C =====
#define PCF8574_ADDR_1 0x20           // Primeiro PCF8574
#define PCF8574_ADDR_2 0x24           // Segundo PCF8574 (se usado)
#define LCD_ADDR 0x27                 // Display LCD

// ===== INTERVALOS DE TEMPO (em milissegundos) =====
#define SENSOR_READ_INTERVAL_MS 30000     // 30 segundos
#define RELAY_CHECK_INTERVAL_MS 5000      // 5 segundos
#define STATUS_PRINT_INTERVAL_MS 60000    // 1 minuto
#define WIFI_RETRY_INTERVAL_MS 10000      // 10 segundos
#define API_RETRY_INTERVAL_MS 5000        // 5 segundos
#define SUPABASE_STATUS_INTERVAL_MS 30000 // 30 segundos

// ===== CONFIGURAÇÕES DE REDE =====
// 🔓 Configurações de AP definidas em WiFiManager.h
// Evitando redefinição para prevenir conflitos

// ⚠️ Para ambientes que exigem segurança extra:
// Altere diretamente em WiFiManager.h: #define AP_PASSWORD "hidro123"

// ===== LIMITES DE SENSORES =====
#define MIN_PH 0.0
#define MAX_PH 14.0

// Auto pH: 1 = prototipo (relaja guards G4/G5/G9-G11/G14); 0 = producción
#ifndef PH_PROTOTYPE_RELAX_GUARDS
#define PH_PROTOTYPE_RELAX_GUARDS 1
#endif

// Banco/dev: 1 = sin interlocks por sensor (métricas + telemetría parcial); 0 = producción
#ifndef HIDRO_DEV_RELAX_SENSORS
#define HIDRO_DEV_RELAX_SENSORS 1
#endif

// Grow cycle P1: pausar Auto EC/pH mientras script tanque (priority >= umbral)
#define TANK_SCRIPT_PRIORITY_THRESHOLD 80
#define TANK_SCRIPT_HOLD_MIN_MS 60000UL
#define TANK_SCRIPT_HOLD_BUFFER_MS 30000UL
#define TANK_SCRIPT_HOLD_DEFAULT_MS 120000UL

#ifndef CIRCULATION_RELAY_DEFAULT
#define CIRCULATION_RELAY_DEFAULT 7
#endif
#define MIN_TDS 0.0
#define MAX_TDS 5000.0
#define MIN_TEMP 0.0
#define MAX_TEMP 50.0
#define MIN_HUMIDITY 0.0
#define MAX_HUMIDITY 100.0

// ===== CONFIGURAÇÕES DOS SENSORES =====
// TDS
#define TDS_VREF 5.0
#define TDS_CALIBRATION_FACTOR 1.0

// pH
#define PH_VREF 3.3
#define PH_CALIBRATION_FACTOR 1.0
#define PH_CAL_7 2.56   // Voltagem para pH 7 (~2.5V)
#define PH_CAL_4 3.3    // Voltagem para pH 4 (~3.3V)
#define PH_CAL_10 2.05  // Voltagem para pH 10 (~2.0V)
#define PH_SAMPLES 10   // Número de amostras para média
#define PH_SAMPLE_INTERVAL 10  // Intervalo entre amostras (ms)

// ===== CONFIGURAÇÕES DE RELÉS =====
#define MAX_RELAYS 8   // Sistema Master ESP-NOW com 8 relés

// Mapeamento de relés para pinos PCF8574 (permite pular pinos defeituosos)
// Formato: {pcf_chip, pin_number, enabled}
// pcf_chip: 0 = PCF1 (0x20), 1 = PCF2 (0x24)
// pin_number: 0-7 para cada PCF
// enabled: true = funcional, false = pino defeituoso (pular)
struct RelayPinMap {
    uint8_t pcf_chip;    // 0 ou 1
    uint8_t pin_number;  // 0-7
    bool enabled;        // true se o pino funciona
};

// Mapeamento flexível - pode ser modificado se houver pinos defeituosos
static const RelayPinMap RELAY_PIN_MAPPING[MAX_RELAYS] = {
    // Relés 0-7 no PCF1 (0x20) - Sistema Master ESP-NOW
    {0, 0, true},   // Relé 0 -> PCF1 P0
    {0, 1, true},   // Relé 1 -> PCF1 P1  
    {0, 2, true},   // Relé 2 -> PCF1 P2
    {0, 3, true},   // Relé 3 -> PCF1 P3
    {0, 4, true},   // Relé 4 -> PCF1 P4
    {0, 5, true},   // Relé 5 -> PCF1 P5
    {0, 6, true},   // Relé 6 -> PCF1 P6
    {0, 7, true}    // Relé 7 -> PCF1 P7
};

// Exemplo de como marcar pinos defeituosos:
// Se o pino P3 do PCF1 estiver quebrado, mude para:
// {0, 3, false},  // Relé 3 -> PCF1 P3 (DEFEITUOSO)

// ===== CONFIGURAÇÕES DA API E BANCO DE DADOS =====

// ===== CONFIGURAÇÕES DO SAVEMANAGER =====
#define PREFERENCES_NAMESPACE "espnow_cfg"  // Namespace para Preferences
#define CONFIG_VERSION 1                     // Versão da configuração

// ===== MACROS DE DEBUG =====
#ifndef HYDRO_DEBUG_PRINTLN
    #ifdef DEBUG_MODE
        #define HYDRO_DEBUG_PRINTLN(x) Serial.println(x)
    #else
        #define HYDRO_DEBUG_PRINTLN(x)
    #endif
#endif

// ===== FUNCIONES GLOBAIS DO SISTEMA =====
/**
 * @brief Retorna o contador de reinícios atual do dispositivo
 * @return Valor atual do contador de reinícios (persistido em NVS)
 * @note O contador é incrementado automaticamente a cada reinício do ESP32
 */
int getRebootCount();
void resetRebootCount();

#endif // CONFIG_H 