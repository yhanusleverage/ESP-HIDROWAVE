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
// 1 = cierre command_ack + relay/state via MQTT → bridge Node (sem HTTPS no hot path)
#ifndef MQTT_COMMAND_BRIDGE_ONLY
#define MQTT_COMMAND_BRIDGE_ONLY 1
#endif
// 1 = nao poll get_and_lock se MQTT conectado >60s
#ifndef COMMAND_POLL_DISABLED_IF_MQTT_OK
#define COMMAND_POLL_DISABLED_IF_MQTT_OK 1
#endif
// 1 = nao sync HTTPS relay_master/slaves periodico se MQTT OK
#ifndef RELAY_HTTPS_SYNC_DISABLED_IF_MQTT_OK
#define RELAY_HTTPS_SYNC_DISABLED_IF_MQTT_OK 1
#endif
#ifndef MQTT_COMMAND_PATH_STABLE_MS
#define MQTT_COMMAND_PATH_STABLE_MS 60000UL
#endif
#ifndef CONFIG_POLL_INTERVAL_MQTT_OK_MS
#define CONFIG_POLL_INTERVAL_MQTT_OK_MS 60000UL  // Auto EC/pH ON-OFF: 1 min (era 5 min; no starve por MQTT OK)
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
#define COMMAND_POLL_INTERVAL_MS 20000  // fallback HTTPS quando MQTT offline
#endif
#ifndef COMMAND_POLL_INTERVAL_MQTT_OK_MS
#define COMMAND_POLL_INTERVAL_MQTT_OK_MS 60000UL  // backup lento se MQTT online
#endif
#ifndef COMMAND_POLL_INTERVAL_MQTT_DOWN_MS
#define COMMAND_POLL_INTERVAL_MQTT_DOWN_MS 20000UL
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

// Master STA+ESP-NOW: no cambiar canal WiFi dinámicamente (evita peer channel mismatch)
#ifndef ESPNOW_LOCK_WIFI_CHANNEL
#define ESPNOW_LOCK_WIFI_CHANNEL 1
#endif

/** Bancada: canal esperado del slave (debe coincidir con WiFi.channel() del router) */
#ifndef ESPNOW_FIXED_CHANNEL
#define ESPNOW_FIXED_CHANNEL 11
#endif

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
#define PH_PIN 35                      // legado: pH analógico ADC; con Modbus no se usa
#define TDS_PIN 33                     // EC analógico (GPIO 33 — validado ESP-SENSORS)
#define EC_SENSOR_ANALOG_PIN TDS_PIN
#define PH_RS485_RX_PIN 34             // RO del módulo TTL-RS485
#define PH_RS485_TX_PIN 23             // DI del módulo TTL-RS485
#define PH_RS485_DE_RE_PIN 32          // DE+RE unidos (LOW = escuchar)
#define PH_MODBUS_BAUD 9600
#define PH_MODBUS_ADDR 1
#define PH_MODBUS_TEMP_REG 0x0000
#define PH_MODBUS_TEMP_SCALE 10.0f     // reg0 / 10 = temperatura °C
#define PH_MODBUS_REG 0x0001           // reg1: pH (reg0 = temperatura)
#define PH_MODBUS_SCALE 10.0f          // pH x10 (0.1 pH)
/** Fase A: barrido Modbus al boot (1=activo). 0 en produccion: reg0=temp, reg1=pH ya confirmados. */
#ifndef PH_MODBUS_DISCOVERY
#define PH_MODBUS_DISCOVERY 0
#endif
#define PH_MODBUS_DISCOVERY_REG_START 0x0000u
#define PH_MODBUS_DISCOVERY_REG_END 0x000Fu
#define PH_MODBUS_DISCOVERY_REG_DELAY_MS 80u
/** EC diagnostico ADC (GPIO 35). Libre — YFB5 está en GPIO4. */
#define EC_ADC_CMP_PIN 35
/** 1 = muestra EC instantanea cada ventana (diagnostico). OK con YFB5 en GPIO4. */
#ifndef EC_FAST_DEBUG
#define EC_FAST_DEBUG 0
#endif
// UART EC legado — no usado por EcAnalogSensor
// #define TDS_RX_PIN 36
// #define TDS_TX_PIN 17
#define TEMP_PIN 4                     // Legacy DS18 — OBSOLETO; pin = YF-B5 (FLOW_SENSOR_PIN)
// Legacy GPIO nivel — NO usar con RS485 (32=DE/RE, 33=EC); niveles vía PCF8574 P0-P3
#define TANK_LOW_PIN 32
#define TANK_HIGH_PIN 33

#ifndef USE_PH_MODBUS_SENSOR
#define USE_PH_MODBUS_SENSOR 1
#endif
/** DS18/OneWire: desactivado para siempre. Temp agua = Modbus pH. GPIO4 solo YF-B5. */
#ifndef HIDRO_ENABLE_DS18B20_FALLBACK
#define HIDRO_ENABLE_DS18B20_FALLBACK 0
#endif
#if HIDRO_ENABLE_DS18B20_FALLBACK
#error "DS18/OneWire desactivado en producto — deja HIDRO_ENABLE_DS18B20_FALLBACK=0 (GPIO4=YF-B5)"
#endif

// ===== 4 SONDAS NPN VIA PCF8574 #1 (P0-P3) =====
#define LEVEL_DEBOUNCE_MS 300
#define LEVEL_POLL_MS 200              // Paridad 4level_sensors LevelSensor(200)
#define LEVEL_LOG_MS 1000              // Línea LEVEL en serial (no 10 s)
#define LEVEL_NPN_ACTIVE_LOW 1         // NPN ON → LOW no PCF (directo, sin PC817)
// L1 base (P3) → L4 topo (P0). Cable físico igual: P0=arriba, P3=abajo.
// Histórico V1 (L1=P0 topo): docs/handoffs/hydraulics/LEVEL_LOGIC_VERSIONS.md
#define LEVEL_SENSOR_PCF_PINS 3, 2, 1, 0
    // Sensor de nível PNP

// I2C - Barramento compartilhado (100 kHz como bancada CAT6)
#define I2C_SDA 21
#define I2C_SCL 22
#ifndef I2C_CLOCK_HZ
#define I2C_CLOCK_HZ 100000
#endif

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
#ifndef SENSOR_READING_STALE_MS
#define SENSOR_READING_STALE_MS 12000UL
#endif

// Auto pH: 1 = prototipo (relaja guards G4/G5/G9-G11/G14); 0 = producción
#ifndef PH_PROTOTYPE_RELAX_GUARDS
#define PH_PROTOTYPE_RELAX_GUARDS 1
#endif

// Banco/dev: 1 = sin interlocks por sensor (métricas + telemetría parcial); 0 = producción
#ifndef HIDRO_DEV_RELAX_SENSORS
#define HIDRO_DEV_RELAX_SENSORS 1
#endif

#ifndef HIDRO_SIMULATE_WATER_LEVELS
#define HIDRO_SIMULATE_WATER_LEVELS 0  // 1 solo bancada sin sondas; producción = PCF L1–L4 reales
#endif

// 1 = sin pH Modbus válido → EC analógica también inválida (bus 12 V común)
// 0 = bancada: EC analógica independiente del pH Modbus
#ifndef HIDRO_EC_REQUIRES_PH_MODBUS
#define HIDRO_EC_REQUIRES_PH_MODBUS 0
#endif

// Diluição EC — YF-B5 Hall (GPIO4; temp agua = Modbus pH). Paridad ESP-SENSORS.
// Amarillo ~5 V/0 V → divisor 10k serie + 20k a GND → ~3.3 V en GPIO4. Sin task FreeRTOS.
#ifndef FLOW_SENSOR_PIN
#define FLOW_SENSOR_PIN 4
#endif
// YF-B5 Seeed: F = 6.6 * Q (Hz, L/min), rango 1..30 L/min, ~396 pulsos/L.
#ifndef FLOW_HZ_PER_LPM
#define FLOW_HZ_PER_LPM 6.6f
#endif
#ifndef FLOW_PULSE_FACTOR
#define FLOW_PULSE_FACTOR FLOW_HZ_PER_LPM  // alias legado
#endif
#ifndef FLOW_Q_MIN_LPM
#define FLOW_Q_MIN_LPM 1.0f
#endif
#ifndef FLOW_Q_MAX_LPM
#define FLOW_Q_MAX_LPM 30.0f
#endif
#ifndef FLOW_WINDOW_MS
#define FLOW_WINDOW_MS 1000UL
#endif
/** Anti-rebote ISR. 100 µs (flowmeter lab) es corto en Master ruidoso; 2 ms deja margen vs máx ~198 Hz (~5 ms). */
#ifndef FLOW_ISR_DEBOUNCE_US
#define FLOW_ISR_DEBOUNCE_US 2000UL
#endif
/**
 * SUPREME: dt real YF-B5 ~5–150 ms. Si dt_min de pulsos aceptados &lt; esto → EMI/rebote, no sumar litros.
 * (Tu log: dt_min≈100–125 µs con total subiendo sin soplar.)
 */
#ifndef FLOW_MIN_PULSE_GAP_US
#define FLOW_MIN_PULSE_GAP_US 3000UL
#endif
#ifndef FLOW_IDLE_HZ
#define FLOW_IDLE_HZ 0.5f
#endif
#ifndef FLOW_MIN_HZ
#define FLOW_MIN_HZ (FLOW_HZ_PER_LPM * FLOW_Q_MIN_LPM)  // 6.6
#endif
#ifndef FLOW_MAX_HZ
#define FLOW_MAX_HZ (FLOW_HZ_PER_LPM * FLOW_Q_MAX_LPM * 1.05f)  // ~207.9 (+5%)
#endif
#ifndef FLOW_PULSES_PER_LITER
#define FLOW_PULSES_PER_LITER (FLOW_HZ_PER_LPM * 60.0f)  // 396
#endif
/** K de campo: L_real / L_leido (balde). 1.0 = datasheet. No auto-ajustar por EC post-dilución. */
#ifndef FLOW_CALIBRATION_FACTOR
#define FLOW_CALIBRATION_FACTOR 1.0f
#endif
#ifndef FLOW_FILTER_ENABLE
#define FLOW_FILTER_ENABLE 1
#endif
/** 1 = línea extra [FLOW dbg] why/raw/deb/dt (SUPREME). 0 = producción. */
#ifndef FLOW_DEBUG
#define FLOW_DEBUG 0
#endif
#ifndef FLOW_MIN_LPM
#define FLOW_MIN_LPM 0.05f  // stall dilución (legacy); filtro Hz usa FLOW_MIN_HZ
#endif
/** 1 = log serial [FLOW] handoff (~1s). 0 = producción. */
#ifndef FLOW_SERIAL_DEBUG
#define FLOW_SERIAL_DEBUG 0
#endif
/** 1 = ISR YF-B5 solo durante dilución/dreno. 0 = ISR siempre (bancada). */
#ifndef FLOW_ISR_ONLY_WHEN_DILUTING
#define FLOW_ISR_ONLY_WHEN_DILUTING 1
#endif
/** 1 = telemetría [RES] heap/HWM/loop-s (contención Master). 0 = producción. */
#ifndef RESOURCE_SERIAL_DEBUG
#define RESOURCE_SERIAL_DEBUG 1
#endif
#ifndef RESOURCE_LOG_MS
#define RESOURCE_LOG_MS 10000UL
#endif
#if EC_FAST_DEBUG && (FLOW_SENSOR_PIN == EC_ADC_CMP_PIN)
#error "FLOW_SENSOR_PIN coincide con EC_ADC_CMP_PIN — cambia uno (ver docs/sensors/SENSOR_FLUJO_YFB5.md)"
#endif
#ifndef FLOWMETER_PULSE_PIN
#define FLOWMETER_PULSE_PIN FLOW_SENSOR_PIN
#endif
#ifndef FLOWMETER_PULSES_PER_LITER
#define FLOWMETER_PULSES_PER_LITER FLOW_PULSES_PER_LITER
// Calibración bancada: pulsos ÷ litros reales → flowmeter_pulses_per_liter (UI).
// EC post-dilución valida la fórmula de dilución, NO el Hall / K.
#endif
#ifndef DILUTION_FILL_FLOW_LPS
#define DILUTION_FILL_FLOW_LPS 0.5f
#endif
/** Bancada: con niveles simulados, tras este tiempo el fill trata "HIGH" (E2E). */
#ifndef DILUTION_FILL_SIM_HIGH_MS
#define DILUTION_FILL_SIM_HIGH_MS 5000UL
#endif
#ifndef DILUTION_DRAIN_RELAY_DEFAULT
#define DILUTION_DRAIN_RELAY_DEFAULT -1
#endif
#ifndef DILUTION_FILL_RELAY_DEFAULT
#define DILUTION_FILL_RELAY_DEFAULT -1
#endif
#ifndef DILUTION_MAX_VOLUME_L_DEFAULT
#define DILUTION_MAX_VOLUME_L_DEFAULT 50.0f
#endif
/** Esperar ACK ESP-NOW de válvula de dreno antes de contar litros. Fill reintenta, no aborta. */
#ifndef DILUTION_VALVE_ACK_TIMEOUT_MS
#define DILUTION_VALVE_ACK_TIMEOUT_MS 10000UL
#endif

// Grow cycle P1: scripts de tanque (priority >= umbral) pausan Auto EC/pH
// mientras el procedimiento secuencial está activo — sin timers HOLD_*.
#define TANK_SCRIPT_PRIORITY_THRESHOLD 80

#ifndef CIRCULATION_RELAY_DEFAULT
#define CIRCULATION_RELAY_DEFAULT 7
#endif
#define MIN_TDS 0.0
#define MAX_TDS 5000.0
#define EC_MIN_PLAUSIBLE 100.0f
#define EC_MAX_PLAUSIBLE 10000.0f
#define MIN_EC 0.0f
#define MAX_EC (EC_SENSOR_RANGE_US_CM * 1.1f)
#define MIN_TEMP 0.0
#define MAX_TEMP 50.0
#define MIN_HUMIDITY 0.0
#define MAX_HUMIDITY 100.0

// ===== CONFIGURAÇÕES DOS SENSORES =====
// EC analógico (EcAnalogSensor)
#define ESP32_ADC_MAX_VOLTS 3.3f
#if defined(ESP32) || defined(CONFIG_IDF_TARGET_ESP32)
#define ESP32_ADC_CONFIGURE_PIN(pin)          \
    do {                                      \
        analogSetPinAttenuation((pin), ADC_11db); \
        analogReadResolution(12);             \
    } while (0)
#else
#define ESP32_ADC_CONFIGURE_PIN(pin) ((void)(pin))
#endif
#define EC_SENSOR_RANGE_US_CM 4400.0f
#define EC_SENSOR_VMAX_PIN_V ESP32_ADC_MAX_VOLTS
#define EC_SAMPLE_INTERVAL_MS 200UL
#define EC_SAMPLES_PER_WINDOW 30
#define EC_BATCH_PERIOD_MS (EC_SAMPLE_INTERVAL_MS * EC_SAMPLES_PER_WINDOW)
#define TDS_VREF EC_SENSOR_VMAX_PIN_V
#define TDS_CALIBRATION_FACTOR 1.0f

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