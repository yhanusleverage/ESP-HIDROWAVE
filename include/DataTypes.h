#ifndef DATA_TYPES_H
#define DATA_TYPES_H

#include <Arduino.h>
#include "Config.h"

/**
 * @brief Configurações por relé (placeholders — o corte real é só o timer do comando).
 * maxDuration/safetyLock NÃO inventam OFF em ON permanente (evita R0~3600s / R6~60s fantasmas).
 * Política: durationSec==0 → permanente; durationSec>0 → countdown explícito.
 */
struct RelayConfig {
    bool autoMode;                       // Reservado (legado)
    uint32_t maxDuration;               // Placeholder; não aplica corte automático
    bool safetyLock;                    // Placeholder; não força timer

    bool isValid() const {
        return true;
    }

    String getValidationError() const {
        return "";
    }
};

// Fallback de firmware: só índice. Nomes de produto = relay_slaves.relay_names (UI).
static const char* const RELAY_NAMES[MAX_RELAYS] = {
    "Relé 0",
    "Relé 1",
    "Relé 2",
    "Relé 3",
    "Relé 4",
    "Relé 5",
    "Relé 6",
    "Relé 7"
};

// Placeholders uniformes — duração vem só do comando (duration_s / timer UI).
static const RelayConfig RELAY_CONFIGS[MAX_RELAYS] = {
    {false, 86400, false},
    {false, 86400, false},
    {false, 86400, false},
    {false, 86400, false},
    {false, 86400, false},
    {false, 86400, false},
    {false, 86400, false},
    {false, 86400, false}
};

// Verificações em tempo de compilação
static_assert(sizeof(RELAY_NAMES)/sizeof(RELAY_NAMES[0]) == MAX_RELAYS, 
              "RELAY_NAMES deve ter exatamente MAX_RELAYS elementos");
static_assert(sizeof(RELAY_CONFIGS)/sizeof(RELAY_CONFIGS[0]) == MAX_RELAYS, 
              "RELAY_CONFIGS deve ter exatamente MAX_RELAYS elementos");

/**
 * @brief Estrutura para armazenar dados dos sensores
 * Todos os valores são inicializados com valores seguros
 */
struct SensorData {
    float environmentTemp = 0.0;     // Temperatura ambiente em °C
    float environmentHumidity = 0.0; // Umidade ambiente em %
    float waterTemp = 0.0;          // Temperatura da água em °C
    float ph = 7.0;                 // pH da água (0-14)
    float ec = 0.0;                // EC em µS/cm
    bool waterLevelOk = false;     // Status do nível da água
    unsigned long timestamp = 0;    // Timestamp da última leitura
    bool valid = false;            // Indica se os dados são válidos

    // Validação de dados
    bool isValid() const {
        return environmentTemp >= MIN_TEMP && environmentTemp <= MAX_TEMP &&
               environmentHumidity >= MIN_HUMIDITY && environmentHumidity <= MAX_HUMIDITY &&
               waterTemp >= MIN_TEMP && waterTemp <= MAX_TEMP &&
               ph >= MIN_PH && ph <= MAX_PH &&
               ec >= MIN_EC && ec <= MAX_EC;
    }
};

/**
 * @brief Estrutura para armazenar status do sistema
 * Monitora o estado geral do sistema
 */
struct SystemStatus {
    bool wifiConnected = false;           // Status da conexão WiFi
    bool apiConnected = false;            // Status da conexão com a API
    bool sensorsOk = false;               // Status geral dos sensores
    bool relaysOk = false;                // Status dos relés
    unsigned long uptime = 0;             // Tempo de execução em ms
    uint32_t freeHeap = 0;               // Memória heap livre
    int wifiRSSI = 0;                    // Força do sinal WiFi em dBm
    String lastError = "";               // Última mensagem de erro

    // Validação de status
    bool isHealthy() const {
        return wifiConnected && apiConnected && sensorsOk && relaysOk && freeHeap > 10000;
    }
};

struct RelayState {
    bool isOn = false;
    unsigned long startTime = 0;
    int timerSeconds = 0;
    bool hasTimer = false;
    String name = "";  // Nome do relé
    RelayConfig config;  // Incorporar configuração
};

#endif // DATA_TYPES_H 