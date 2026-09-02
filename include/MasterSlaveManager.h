#ifndef MASTER_SLAVE_MANAGER_H
#define MASTER_SLAVE_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <vector>
#include <functional>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "ESPNowController.h"
#include "ESPNowTypes.h"  // ✅ Para SlaveRelayStatesCache y CachedSlaveRelayState  // 🔄 FASE 2: Para RelayCommandAck
#include "Config.h"
#include "DeviceID.h"

/**
 * @brief Status de um Slave na lista confiável
 */
enum class SlaveStatus : uint8_t {
    UNKNOWN = 0,        // Status desconhecido
    DISCOVERED = 1,     // Descoberto via broadcast
    PING_RECEIVED = 2,  // Recebeu PING do Slave
    HANDSHAKE_OK = 3,   // Handshake completado
    ONLINE = 4,         // Online e operacional
    OFFLINE = 5,        // Offline (timeout)
    ERROR = 6          // Erro de comunicação
};

/**
 * @brief Informações detalhadas de um Slave confiável
 */
struct TrustedSlave {
    uint8_t macAddress[6];      // MAC address do Slave
    String deviceName;          // Nome do dispositivo
    String deviceType;          // Tipo (RelayBox, SensorBox, etc.)
    SlaveStatus status;         // Status atual
    unsigned long lastSeen;     // Último contato (timestamp)
    unsigned long firstSeen;     // Primeiro contato (timestamp)
    int rssi;                   // Força do sinal
    uint8_t numRelays;          // Número de relés disponíveis
    bool operational;           // Status operacional
    uint32_t uptime;           // Uptime do Slave
    uint32_t freeHeap;         // Memória livre do Slave
    uint8_t wifiChannel;        // 🚨 CRÍTICO: Canal WiFi do Slave (1-13)
    uint32_t messageCount;     // Contador de mensagens recebidas
    uint32_t lastPingId;       // ID do último PING recebido
    uint32_t lastPongId;       // ID do último PONG enviado
    bool waitingForPong;       // Se está aguardando PONG
    unsigned long pongTimeout; // Timeout para PONG
    
    // Estatísticas de comunicação
    uint32_t pingsReceived;    // PINGs recebidos do Slave (quando Slave envia PING)
    uint32_t pingsSent;        // PINGs enviados ao Slave (quando Master envia PING)
    uint32_t pongsReceived;    // PONGs recebidos do Slave (quando Slave responde PING)
    uint32_t pongsSent;        // PONGs enviados ao Slave (quando Master responde PING)
    uint32_t messagesReceived; // Mensagens recebidas
    uint32_t messagesLost;     // Mensagens perdidas
    uint32_t errors;           // Erros de comunicação
    
    // ⭐ POTENCIA MÁXIMA: Estado dos relés remotos do slave
    struct RemoteRelayState {
        bool state;            // Estado: true=ON, false=OFF
        bool hasTimer;         // Tem timer ativo
        uint16_t remainingTime; // Tempo restante em segundos
        unsigned long lastUpdate; // Última atualização
        String name;           // Nome do relé (recebido do slave via ESP-NOW)
    };
    RemoteRelayState relayStates[8]; // Estado dos 8 relés (0-7)
    
    /**
     * @brief Construtor padrão
     */
    TrustedSlave() {
        memset(macAddress, 0, 6);
        deviceName = "Unknown";
        deviceType = "Unknown";
        status = SlaveStatus::UNKNOWN;
        lastSeen = 0;
        firstSeen = 0;
        rssi = -100;
        numRelays = 0;
        operational = false;
        uptime = 0;
        freeHeap = 0;
        wifiChannel = 0;  // 0 = desconhecido
        messageCount = 0;
        lastPingId = 0;
        lastPongId = 0;
        waitingForPong = false;
        pongTimeout = 0;
        pingsReceived = 0;
        pingsSent = 0;
        pongsReceived = 0;
        pongsSent = 0;
        messagesReceived = 0;
        messagesLost = 0;
        errors = 0;
        
        // Inicializar estados dos relés
        for (int i = 0; i < 8; i++) {
            relayStates[i].state = false;
            relayStates[i].hasTimer = false;
            relayStates[i].remainingTime = 0;
            relayStates[i].lastUpdate = 0;
            relayStates[i].name = "";  // ✅ Inicializar nome vazio
        }
    }
    
    /**
     * @brief Construtor com MAC
     */
    TrustedSlave(const uint8_t* mac) {
        memcpy(macAddress, mac, 6);
        deviceName = "Slave-" + ESPNowController::macToString(mac).substring(12);
        deviceType = "RelayBox";
        status = SlaveStatus::DISCOVERED;
        lastSeen = millis();
        firstSeen = millis();
        rssi = -50;
        numRelays = 8;
        operational = true;
        uptime = 0;
        freeHeap = 0;
        messageCount = 0;
        lastPingId = 0;
        lastPongId = 0;
        waitingForPong = false;
        pongTimeout = 0;
        pingsReceived = 0;
        pingsSent = 0;
        pongsReceived = 0;
        pongsSent = 0;
        messagesReceived = 0;
        messagesLost = 0;
        errors = 0;
        
        // Inicializar estados dos relés
        for (int i = 0; i < 8; i++) {
            relayStates[i].state = false;
            relayStates[i].hasTimer = false;
            relayStates[i].remainingTime = 0;
            relayStates[i].lastUpdate = 0;
            relayStates[i].name = "";  // ✅ Inicializar nome vazio
        }
    }
    
    /**
     * @brief Verifica se o Slave está online
     */
    bool isOnline() const {
        return status == SlaveStatus::ONLINE || status == SlaveStatus::HANDSHAKE_OK;
    }
    
    /**
     * @brief Verifica se o Slave está offline há muito tempo
     */
    bool isOfflineTimeout(unsigned long timeoutMs = 60000) const {
        return (millis() - lastSeen) > timeoutMs;
    }
    
    /**
     * @brief Atualiza timestamp de último contato
     */
    void updateLastSeen() {
        lastSeen = millis();
        if (firstSeen == 0) {
            firstSeen = lastSeen;
        }
    }
    
    /**
     * @brief Obtém tempo desde último contato
     */
    unsigned long getTimeSinceLastSeen() const {
        return millis() - lastSeen;
    }
    
    /**
     * @brief Obtém tempo desde primeiro contato
     */
    unsigned long getUptime() const {
        return millis() - firstSeen;
    }
};

/**
 * @brief Gerenciador Master-Slave para comunicação bidirecional ESP-NOW
 * 
 * Esta classe implementa:
 * - Recebimento de PINGs do SLAVE
 * - Lista confiável de SLAVEs
 * - Sistema de ACKs e confirmação de pacotes
 * - Handshake bidirecional
 * - Monitoramento de status online/offline
 */
class MasterSlaveManager {
public:
    /**
     * @brief Construtor
     * @param espNowController Instância do ESPNowController
     */
    MasterSlaveManager(ESPNowController* espNowController);
    
    /**
     * @brief Inicializa o gerenciador
     * @return true se inicialização foi bem sucedida
     */
    bool begin();
    
    /**
     * @brief Atualiza o sistema (chamar no loop principal)
     */
    void update();
    
    /**
     * @brief Para o sistema
     */
    void end();
    
    // ===== GERENCIAMENTO DE SLAVES =====
    
    /**
     * @brief Adiciona Slave à lista confiável
     * @param macAddress MAC address do Slave
     * @param deviceName Nome do dispositivo
     * @param deviceType Tipo do dispositivo
     * @return true se Slave foi adicionado
     */
    bool addTrustedSlave(const uint8_t* macAddress, const String& deviceName = "", const String& deviceType = "RelayBox");

    void startEspNowLockWindow();
    bool isEspNowLockWindowActive() const;
    unsigned long getLastRxAgeMs();
    
    /**
     * @brief Remove Slave da lista confiável
     * @param macAddress MAC address do Slave
     * @return true se Slave foi removido
     */
    bool removeTrustedSlave(const uint8_t* macAddress);
    
    /**
     * @brief Obtém Slave da lista confiável
     * @param macAddress MAC address do Slave
     * @return Ponteiro para TrustedSlave ou nullptr se não encontrado
     */
    TrustedSlave* getTrustedSlave(const uint8_t* macAddress);

    /** false = mutex timeout; true = mutex held (*out may be nullptr if absent) */
    bool lookupTrustedSlave(const uint8_t* macAddress, TrustedSlave** out);

    /**
     * @brief Registra contato radio com slave (atualiza lastSeen + ONLINE)
     */
    void touchSlaveLink(const uint8_t* mac, const char* reason);

    /**
     * @brief Slave alcançavel: ONLINE ou lastSeen recente
     */
    bool isSlaveReachable(const TrustedSlave& slave) const;

    /**
     * @brief Copia snapshot de relés sob mutex (seguro para MQTT publish)
     */
    bool readSlaveRelaySnapshot(const uint8_t* macAddress, bool states[8], bool timers[8],
                                int remaining[8], uint8_t& numRelays, bool& linkOnline,
                                uint16_t& linkLastSeenS);

    /**
     * @brief Log de falha consecutiva esp_now_send (chamado de onDataSent)
     */
    void notifyEspNowSendFail(const uint8_t* mac, uint8_t consecutiveFails);
    
    /**
     * @brief Obtém lista de todos os Slaves confiáveis
     * @return Vector com todos os Slaves
     */
    std::vector<TrustedSlave> getAllTrustedSlaves();

    /**
     * Tras WiFi GOT_IP: re-add broadcast + slaves no canal STA atual.
     * Não faz esp_now_deinit nem muda o canal do rádio.
     */
    void refreshEspNowPeersOnCurrentChannel();

    /**
     * @brief Itera slaves confiáveis sob mutex — sem alocar vector (seguro em heap baixo)
     * @return false se timeout no mutex
     */
    bool forEachTrustedSlave(const std::function<void(const TrustedSlave&)>& visitor);
    
    /**
     * @brief Obtém lista de Slaves online
     * @return Vector com Slaves online
     */
    std::vector<TrustedSlave> getOnlineSlaves();
    
    /**
     * @brief Obtém número de Slaves confiáveis
     * @return Número de Slaves
     */
    int getTrustedSlaveCount();
    
    /**
     * @brief Obtém número de Slaves online
     * @return Número de Slaves online
     */
    int getOnlineSlaveCount();
    
    // ===== COMUNICAÇÃO BIDIRECIONAL =====
    
    /**
     * @brief Envia PING para Slave específico
     * @param macAddress MAC address do Slave
     * @return true se PING foi enviado
     */
    bool sendPingToSlave(const uint8_t* macAddress);
    
    /**
     * @brief Envia PING para todos os Slaves online
     * @return Número de PINGs enviados
     */
    int sendPingToAllSlaves();
    
    /**
     * @brief Envia comando de relé para Slave específico
     * @param macAddress MAC address do Slave
     * @param relayNumber Número do relé
     * @param action Ação ("on", "off", "toggle")
     * @param duration Duração em segundos
     * @param supabaseCommandId ID do comando no Supabase (0 = não vem do Supabase)
     * @param updateStatus Se true, atualiza status após comando (default: true). Use false para operações em lote (on_all, off_all)
     * @return commandId do ESP-NOW (0 se falhou). Use > 0 para sucesso (compatível com código antigo que espera bool)
     */
    uint32_t sendRelayCommandToSlave(const uint8_t* macAddress, int relayNumber, const String& action,
                                     int duration = 0, int supabaseCommandId = 0, bool updateStatus = true,
                                     int cycleOffDuration = 0, const String& commandMode = "");

    /** Un paquete SET_RELAY_MASK (bit i = relé i). mask 0xFF = on_all, 0x00 = off_all. */
    uint32_t sendRelayMaskToSlave(const uint8_t* macAddress, uint8_t mask,
                                  int durationSec = 0, int supabaseCommandId = 0);
    int applyRelayMaskToAllOnlineSlaves(uint8_t mask);
    
    /**
     * @brief Solicita status de todos os relés de um Slave
     * @param macAddress MAC address do Slave
     * @return true se solicitação foi enviada
     */
    bool requestSlaveStatus(const uint8_t* macAddress);
    
    /**
     * @brief Solicita status de todos os relés de todos os slaves online
     * @details Após um comando, atualiza o status de todos os relés para manter frontend sincronizado
     */
    void requestAllSlavesRelayStatus();
    
    /**
     * @brief Solicita informações do dispositivo Slave
     * @param macAddress MAC address do Slave
     * @return true se solicitação foi enviada
     */
    bool requestSlaveInfo(const uint8_t* macAddress);
    
    /**
     * @brief Define flag de processamento de resposta de status (proteção contra loop infinito)
     * @param processing true se está processando resposta de status
     */
    void setProcessingStatusResponse(bool processing);

    /** Drena sinal pendente antes de solicitar ALL_RELAYS_STATUS */
    void drainAllRelaysStatusWait();

    /** Aguarda ALL_RELAYS_STATUS (timeout ms) após requestSlaveStatus */
    bool waitForAllRelaysStatus(uint32_t timeoutMs);

    /** Sinaliza sync loop — chamado ao completar ALL_RELAYS_STATUS */
    void notifyAllRelaysStatusReceived(const uint8_t* senderMac = nullptr,
                                       const bool relayStates[8] = nullptr,
                                       uint8_t numRelays = 0);

    using AllRelaysSnapshotCallback = std::function<void(const uint8_t* mac, const bool states[8], uint8_t numRelays)>;
    void setAllRelaysSnapshotCallback(AllRelaysSnapshotCallback callback) {
        allRelaysSnapshotCallback = callback;
    }
    
    // ===== ✅ PASSO 1: PROCESSAMENTO DE COMANDOS DO SUPABASE PARA SLAVES =====
    
    /**
     * @brief Converte device_id de slave para MAC address
     * @param deviceId Device ID no formato ESP32_SLAVE_XX_XX_XX_XX_XX_XX
     * @param macAddress Array de 6 bytes para armazenar o MAC
     * @return true se conversão foi bem sucedida
     */
    bool deviceIdToMacAddress(const String& deviceId, uint8_t* macAddress);
    
    /**
     * @brief Processa comandos de relé do Supabase para slaves ESP-NOW
     * @details Busca comandos pendentes onde device_id LIKE 'ESP32_SLAVE_%'
     *          e envia via ESP-NOW
     */
    void processSlaveRelayCommands();
    
    // ===== SISTEMA DE ACKs =====
    
    /**
     * @brief Envia ACK para confirmação de recebimento
     * @param macAddress MAC address do destinatário
     * @param messageId ID da mensagem confirmada
     * @return true se ACK foi enviado
     */
    bool sendAck(const uint8_t* macAddress, uint32_t messageId);
    
    /**
     * @brief Verifica se está aguardando ACK de uma mensagem
     * @param macAddress MAC address do destinatário
     * @param messageId ID da mensagem
     * @return true se está aguardando ACK
     */
    bool isWaitingForAck(const uint8_t* macAddress, uint32_t messageId);
    
    /**
     * @brief Marca mensagem como confirmada (ACK recebido)
     * @param macAddress MAC address do remetente
     * @param messageId ID da mensagem
     */
    void markMessageAcknowledged(const uint8_t* macAddress, uint32_t messageId);
    
    // ===== CALLBACKS =====
    
    /**
     * @brief Define callback para quando Slave é descoberto
     * @param callback Função a ser chamada
     */
    void setSlaveDiscoveredCallback(std::function<void(const uint8_t* macAddress, const String& deviceName, const String& deviceType)> callback);
    
    /**
     * @brief Define callback para quando Slave fica online
     * @param callback Função a ser chamada
     */
    void setSlaveOnlineCallback(std::function<void(const uint8_t* macAddress, const String& deviceName)> callback);
    
    /**
     * @brief Define callback para quando Slave fica offline
     * @param callback Função a ser chamada
     */
    void setSlaveOfflineCallback(std::function<void(const uint8_t* macAddress, const String& deviceName)> callback);
    
    /**
     * @brief Define callback para PING recebido do Slave
     * @param callback Função a ser chamada
     */
    void setPingReceivedCallback(std::function<void(const uint8_t* macAddress, uint32_t pingId)> callback);
    
    /**
     * @brief Define callback para PONG recebido do Slave
     * @param callback Função a ser chamada
     */
    void setPongReceivedCallback(std::function<void(const uint8_t* macAddress, uint32_t pongId)> callback);
    
    /**
     * @brief Define callback para status de relé recebido do Slave
     * @param callback Função a ser chamada
     */
    void setRelayStatusCallback(std::function<void(const uint8_t* macAddress, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name)> callback);
    
    /**
     * @brief Define callback para informações de dispositivo recebidas do Slave
     * @param callback Função a ser chamada
     */
    void setDeviceInfoCallback(std::function<void(const uint8_t* macAddress, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel)> callback);
    
    /**
     * @brief Define callback para ACK recebido
     * @param callback Função a ser chamada
     */
    void setAckReceivedCallback(std::function<void(const uint8_t* macAddress, uint32_t messageId)> callback);
    
    /**
     * @brief Define callback para erro de comunicação
     * @param callback Função a ser chamada
     */
    void setErrorCallback(std::function<void(const uint8_t* macAddress, const String& error)> callback);
    
    // ===== 🔄 FASE 2: CALLBACK PARA ACK =====
    
    /**
     * @brief Define callback para ACK de comando de relay recebido
     * @param callback Função a ser chamada
     */
    void setRelayAckCallback(std::function<void(const uint8_t* macAddress, uint32_t commandId, bool success, uint8_t relayNumber, uint8_t currentState)> callback);
    
    /**
     * @brief Define callback para marcar comandos en Supabase
     * @param callback Función (supabaseCommandId, success, errorMessage)
     */
    void setSupabaseCommandCallback(std::function<void(int supabaseCommandId, bool success, const String& errorMessage)> callback);

    /**
     * @brief Fecha comando slave na nuvem (MQTT command_ack ou HTTPS) com contexto completo
     */
    void setSlaveCommandResolvedCallback(
        std::function<void(int supabaseCommandId, uint32_t espNowCommandId, const uint8_t* mac,
                           int relayNumber, bool currentState)> callback);
    
    /**
     * @brief Define callback para actualizar estados de relés de slaves en Supabase
     * @param callback Función (masterDeviceId, slaveMacAddress, slaveDeviceId, relayNumber, state, hasTimer, remainingTime)
     */
    void setSupabaseRelayStateCallback(std::function<void(const String& masterDeviceId, const String& slaveMacAddress, const String& slaveDeviceId, int relayNumber, bool state, bool hasTimer, int remainingTime)> callback);
    
    // ===== 🔄 FASE 2: PROCESSAMENTO DE ACKs DE RELAY =====
    
    /**
     * @brief Processa ACK de comando de relay recebido do Slave (PÚBLICO para ESPNowController)
     * @param ack Estrutura do ACK
     * @param senderMac MAC do Slave que enviou o ACK
     */
    void processRelayCommandAck(const RelayCommandAck& ack, const uint8_t* senderMac);

    void addToRetryQueue(const uint8_t* targetMac, int relayNumber, const String& action, int duration,
                         uint32_t commandId, int supabaseCommandId = 0, bool waitingForAck = false,
                         int cycleOffDuration = 0, const String& commandMode = "");
    void removeFromRetryQueue(uint32_t commandId, bool currentState = false, bool notifySupabase = true);
    
    /**
     * @brief Obtém instância estática do MasterSlaveManager (PÚBLICO para ESPNowController)
     * @return Ponteiro para instância ou nullptr
     */
    static MasterSlaveManager* getInstance() { return instance; }

    /** Comandos ESP-NOW pendentes (retry/ACK) — usado para defer SSL */
    size_t getPendingRelayCommandCount() const;
    
    // ===== UTILITÁRIOS =====
    
    /**
     * @brief Obtém estatísticas de comunicação
     * @return String JSON com estatísticas
     */
    String getStatsJSON();
    
    /**
     * @brief Imprime status do sistema
     */
    void printStatus();
    
    /**
     * @brief Imprime lista de Slaves confiáveis
     */
    void printTrustedSlaves();
    
    /**
     * @brief Força limpeza de Slaves offline
     */
    void cleanupOfflineSlaves();
    
    /**
     * @brief Força re-descoberta de todos os Slaves
     */
    void rediscoverSlaves();

    /**
     * @brief Ventana CONFIG: envia credenciais + discovery no canal marco cero
     */
    void runProvisioningIfNeeded();

    uint8_t getLastEspNowChannel() const { return lastEspNowChannel; }
    void saveEspNowLastChannel(uint8_t channel);

private:
    ESPNowController* espNowController;  // Instância do ESPNowController
    bool initialized;                  // Status de inicialização
    unsigned long espnowLockWindowUntil;
    uint8_t lastEspNowChannel;
    
    // ✅ PROTEÇÃO MULTI-CORE: Mutex para proteger trustedSlaves
    SemaphoreHandle_t trustedSlavesMutex;  // Mutex para acesso thread-safe
    
    // Lista de Slaves confiáveis
    std::vector<TrustedSlave> trustedSlaves;
    
    // ✅ Função helper interna (sem mutex - para uso dentro de funciones que já têm mutex)
    TrustedSlave* findTrustedSlaveUnsafe(const uint8_t* macAddress);
    
    // ===== 🎯 CACHE NVS DE ESTADOS DE SLAVES =====
    /**
     * @brief Guarda estados de relés de slaves em NVS (cache local)
     * @return true se guardado com sucesso
     */
    bool saveSlaveRelayStatesToNVS();
    
    /**
     * @brief Carrega estados de relés de slaves de NVS (cache local)
     * @return true se carregado com sucesso
     */
    bool loadSlaveRelayStatesFromNVS();

    bool saveTrustedPeersToNVS();
    bool loadTrustedPeersFromNVS();
    
    /**
     * @brief Valida estados cacheados comparando com estados reais
     * Solicita atualização via ESP-NOW se necessário
     */
    void validateCachedStates();
    
    // Contadores de mensagens pendentes de ACK
    struct PendingAck {
        uint8_t macAddress[6];
        uint32_t messageId;
        unsigned long timestamp;
        int retryCount;
    };
    std::vector<PendingAck> pendingAcks;
    
    // ===== 🔄 FASE 1: SISTEMA DE RETRY AUTOMÁTICO =====
    /**
     * @brief Estrutura para comandos pendentes de retry
     * Quando um comando falha ao enviar, ele é guardado aqui para retentar
     */
    struct PendingRelayCommand {
        uint8_t targetMac[6];        // MAC do Slave destino
        int relayNumber;             // Número do relé (0-7)
        String action;               // Ação: "on", "off", "toggle", "timed_on", "cycle", ...
        int duration;                // Duração em segundos
        int cycleOffDuration;        // OFF phase for cycle mode
        String commandMode;          // instant, timed_on, cycle, ...
        unsigned long enqueuedAt;        // Quando foi criado o comando
        unsigned long ackWaitStartedAt;  // Quando entrou em waitingForAck
        unsigned long nextRetry;     // Quando fazer próximo retry
        uint8_t retryCount;          // Quantas vezes já tentou
        uint32_t commandId;          // ID único do comando
        bool waitingForAck;          // Se está aguardando confirmação
        int supabaseCommandId;       // ID do comando en Supabase (0 = no viene de Supabase)
    };
    std::vector<PendingRelayCommand> pendingRelayCommands;
    mutable SemaphoreHandle_t pendingRelayCommandsMutex = nullptr;
    
    bool lockPendingQueue(TickType_t timeout = portMAX_DELAY) const;
    void unlockPendingQueue() const;
    static constexpr uint8_t MAX_RELAY_RETRIES = 3;
    static constexpr size_t MAX_PENDING_RELAY_COMMANDS = 8;
    static constexpr unsigned long RETRY_INTERVAL = 2000;
    static constexpr unsigned long SLAVE_REACHABLE_MS = 120000;
    static constexpr unsigned long SLAVE_OFFLINE_TIMEOUT_MS = 180000;
    static constexpr unsigned long SLAVE_QUEUE_OFFLINE_TIMEOUT_MS = 60000;
    static constexpr unsigned long ACK_WAIT_TIMEOUT_MS = 30000;
    static constexpr unsigned long MIN_ESPNOW_SEND_GAP_MS = 500;

    uint8_t lastEspNowSendMac_[6];
    unsigned long lastEspNowSendAt_;

    void logSlaveLink(const char* event, const uint8_t* mac, long lastSeenDeltaMs = -1);
    bool hasInFlightForMac(const uint8_t* mac) const;
    /** Caller already holds pendingRelayCommandsMutex (non-recursive). */
    bool hasInFlightForMacLocked(const uint8_t* mac) const;
    bool canEspNowSendToMac(const uint8_t* mac) const;
    void markEspNowSendToMac(const uint8_t* mac);
    
    uint32_t commandIdCounter;  // Contador para IDs únicos de comandos
    
    // ✅ Proteção contra loop infinito de callbacks
    bool processingStatusResponse;  // Flag para evitar solicitar status quando já está processando resposta
    SemaphoreHandle_t allRelaysStatusSem;  // Sinaliza ALL_RELAYS_STATUS recebido (sync loop)
    
    // Estatísticas gerais
    uint32_t totalPingsReceived;
    uint32_t totalPongsSent;
    uint32_t totalAcksSent;
    uint32_t totalAcksReceived;
    uint32_t totalErrors;
    
    // Callbacks
    std::function<void(const uint8_t* macAddress, const String& deviceName, const String& deviceType)> slaveDiscoveredCallback = nullptr;
    std::function<void(const uint8_t* macAddress, const String& deviceName)> slaveOnlineCallback = nullptr;
    std::function<void(const uint8_t* macAddress, const String& deviceName)> slaveOfflineCallback = nullptr;
    std::function<void(const uint8_t* macAddress, uint32_t pingId)> pingReceivedCallback = nullptr;
    std::function<void(const uint8_t* macAddress, uint32_t pongId)> pongReceivedCallback = nullptr;
    std::function<void(const uint8_t* macAddress, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name)> relayStatusCallback = nullptr;
    std::function<void(const uint8_t* macAddress, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel)> deviceInfoCallback = nullptr;
    std::function<void(const uint8_t* macAddress, uint32_t messageId)> ackReceivedCallback = nullptr;
    std::function<void(const uint8_t* macAddress, const String& error)> errorCallback = nullptr;
    std::function<void(const uint8_t* macAddress, uint32_t commandId, bool success, uint8_t relayNumber, uint8_t currentState)> relayAckCallback = nullptr;  // 🔄 FASE 2
    std::function<void(int supabaseCommandId, bool success, const String& errorMessage)> supabaseCommandCallback = nullptr;  // Callback para Supabase
    std::function<void(int supabaseCommandId, uint32_t espNowCommandId, const uint8_t* mac,
                       int relayNumber, bool currentState)> slaveCommandResolvedCallback = nullptr;
    std::function<void(const String& masterDeviceId, const String& slaveMacAddress, const String& slaveDeviceId, int relayNumber, bool state, bool hasTimer, int remainingTime)> supabaseRelayStateCallback = nullptr;  // ✅ Callback para actualizar estados de relés en Supabase
    AllRelaysSnapshotCallback allRelaysSnapshotCallback = nullptr;
    
    // ===== MÉTODOS PRIVADOS =====
    
    /**
     * @brief Processa PING recebido do Slave
     * @param senderMac MAC do Slave
     * @param pingId ID do PING
     */
    void processPingReceived(const uint8_t* senderMac, uint32_t pingId);
    
    /**
     * @brief Processa PONG recebido do Slave
     * @param senderMac MAC do Slave
     * @param pongId ID do PONG
     */
    void processPongReceived(const uint8_t* senderMac, uint32_t pongId);
    
    /**
     * @brief Processa ACK recebido do Slave
     * @param senderMac MAC do Slave
     * @param messageId ID da mensagem confirmada
     */
    void processAckReceived(const uint8_t* senderMac, uint32_t messageId);
    
    /**
     * @brief Processa informações de dispositivo recebidas do Slave
     * @param senderMac MAC do Slave
     * @param deviceName Nome do dispositivo
     * @param deviceType Tipo do dispositivo
     * @param numRelays Número de relés
     * @param operational Status operacional
     */
    void processDeviceInfoReceived(const uint8_t* senderMac, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel = 0);
    
    /**
     * @brief Processa status de relé recebido do Slave
     * @param senderMac MAC do Slave
     * @param relayNumber Número do relé
     * @param state Estado atual
     * @param hasTimer Tem timer ativo
     * @param remainingTime Tempo restante
     * @param name Nome do relé
     */
    void processRelayStatusReceived(const uint8_t* senderMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name);
    
    /**
     * @brief Atualiza informações de um Slave confiável
     * @param macAddress MAC do Slave
     * @param deviceName Nome do dispositivo
     * @param deviceType Tipo do dispositivo
     * @param status Novo status
     */
    void updateTrustedSlave(const uint8_t* macAddress, const String& deviceName = "", const String& deviceType = "", SlaveStatus status = SlaveStatus::UNKNOWN);
    
    /**
     * @brief Verifica e atualiza status de Slaves offline
     */
    void checkSlaveStatus();
    
    /**
     * @brief Limpa ACKs pendentes expirados
     */
    void cleanupExpiredAcks();
    
    /**
     * @brief Reenvia ACKs pendentes se necessário
     */
    void resendPendingAcks();

    void processRetryQueue();
    
    void sendPendingCommandsToSlave(const uint8_t* macAddress);
    
    uint32_t generateCommandId();
    
    /**
     * @brief Callbacks estáticos para ESPNowController
     */
    static void onPingReceivedStatic(const uint8_t* senderMac);
    static void onPongReceivedStatic(const uint8_t* senderMac);
    static void onDeviceInfoReceivedStatic(const uint8_t* senderMac, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel = 0);
    static void onRelayStatusReceivedStatic(const uint8_t* senderMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name);
    static void onErrorReceivedStatic(const String& error);
    
    // Instância estática para callbacks
    static MasterSlaveManager* instance;
};

#endif // MASTER_SLAVE_MANAGER_H
