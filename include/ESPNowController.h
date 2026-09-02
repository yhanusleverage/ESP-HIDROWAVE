#ifndef ESPNOW_CONTROLLER_H
#define ESPNOW_CONTROLLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>

// ✅ CRÍTICO: Incluir ESPNowTypes.h ANTES de outros headers para garantir que SingleRelayState esteja definido
#include "ESPNowTypes.h"

// Incluir configurações se disponível
#ifndef CONFIG_H
    #include "Config.h"
#endif

// Incluir WiFiCredentialsManager para estrutura WiFiCredentialsData
#include "WiFiCredentialsManager.h"

// ===== 🎨 ADAPTER PATTERN: Include do Backend =====
// Forward declaration para evitar dependência circular
class ESPNowTask;

/**
 * @brief Tipos de mensagem ESP-NOW
 */
enum class MessageType : uint8_t {
    RELAY_COMMAND = 0x01,       // Comando de relé
    RELAY_STATUS = 0x02,        // Status de relé
    DEVICE_INFO = 0x03,         // Informações do dispositivo
    PING = 0x04,                // Ping/Pong para testar conectividade
    PONG = 0x05,                // Resposta ao ping
    BROADCAST = 0x06,           // Mensagem broadcast
    ACK = 0x07,                 // Confirmação de recebimento
    ERROR = 0x08,               // Mensagem de erro
    WIFI_CREDENTIALS = 0x09,    // Credenciais WiFi
    HANDSHAKE_REQUEST = 0x0A,   // Solicitação de handshake
    HANDSHAKE_RESPONSE = 0x0B,  // Resposta ao handshake
    CONNECTIVITY_CHECK = 0x0C,  // Verificação de conectividade
    CONNECTIVITY_REPORT = 0x0D, // Relatório de conectividade
    ALL_RELAYS_STATUS = 0x0E,   // slave → Master: estado real 8 relés
    SET_RELAY_MASK = 0x0F,       // Master → slave: máscara atómica (contrato SLAVE)
    CHANNEL_CHANGE = 0x10,       // Master → slave: novo canal operativo
    PERSISTENT_STATE_SYNC = 0x11 // NVS persistente (não usar 0x0F)
};

/**
 * @brief Estrutura base para mensagens ESP-NOW
 */
struct ESPNowMessage {
    MessageType type;           // Tipo da mensagem
    uint8_t senderId[6];       // MAC do remetente
    uint8_t targetId[6];       // MAC do destinatário (FF:FF:FF:FF:FF:FF para broadcast)
    uint32_t messageId;        // ID único da mensagem
    uint32_t timestamp;        // Timestamp da mensagem
    uint8_t dataSize;          // Tamanho dos dados
    uint8_t data[200];         // Dados da mensagem (máximo ESP-NOW: 250 bytes total)
    uint8_t checksum;          // Checksum simples para validação
} __attribute__((packed));

/**
 * @brief Estrutura para comando de relé
 */
/** Master → slave: bit i = relé i ON. duration 0 = permanente. */
struct RelayMaskCommandData {
    uint8_t mask;
    uint8_t pad;
    uint16_t durationSec;
    uint32_t commandId;
} __attribute__((packed));

struct RelayCommandData {
    int relayNumber;           // Número do relé (0-7)
    bool state;               // Estado desejado
    int duration;             // Duração em segundos (0 = sem timer)
    char action[12];          // "on", "off", "toggle", "status", "timed_on", "cycle", ...
    uint32_t commandId;       // ID do comando (master) para ACK
    int cycleOffDuration;     // OFF seconds for cycle mode (0 = not cycle)
    char mode[12];            // "instant","timed_on","timed_off","cycle","cycle_stop"
} __attribute__((packed));

#ifndef RELAY_STATUS_DATA_DEFINED
#define RELAY_STATUS_DATA_DEFINED
/**
 * @brief Estrutura para status de relé
 */
struct RelayStatusData {
    int relayNumber;          // Número do relé
    bool state;              // Estado atual
    bool hasTimer;           // Tem timer ativo
    int remainingTime;       // Tempo restante em segundos
    char name[32];           // Nome do relé
} __attribute__((packed));
#endif

/**
 * @brief Estrutura para handshake bidirecional
 */
struct HandshakeData {
    uint32_t sessionId;         // ID único da sessão
    uint32_t timestamp;         // Timestamp do handshake
    uint8_t deviceType;        // 0=Master, 1=Slave
    char deviceName[32];       // Nome do dispositivo
    uint8_t protocolVersion;   // Versão do protocolo
    bool wifiConnected;        // Status WiFi
    uint8_t validationCode;    // Código de validação
} __attribute__((packed));

/**
 * @brief Estrutura para relatório de conectividade
 */
struct ConnectivityReportData {
    uint32_t sessionId;         // ID da sessão
    uint32_t timestamp;         // Timestamp do relatório
    bool wifiConnected;         // Status WiFi
    int32_t wifiRSSI;          // Força do sinal WiFi
    uint8_t wifiChannel;        // Canal WiFi
    uint32_t uptime;            // Tempo de funcionamento
    uint32_t freeHeap;          // Memória livre
    uint8_t messageCount;       // Contador de mensagens
    bool operational;           // Status operacional
} __attribute__((packed));

// WiFiCredentialsData agora está definido em WiFiCredentialsManager.h (incluído acima)

#ifndef DEVICE_INFO_DATA_DEFINED
#define DEVICE_INFO_DATA_DEFINED
/**
 * @brief Estrutura para informações do dispositivo
 */
struct DeviceInfoData {
    char deviceName[32];     // Nome do dispositivo
    char deviceType[16];     // Tipo do dispositivo
    uint8_t numRelays;       // Número de relés
    bool operational;        // Status operacional
    uint32_t uptime;         // Uptime em milissegundos
    uint32_t freeHeap;       // Memória livre
    uint8_t wifiChannel;     // ✅ Canal WiFi do dispositivo
    uint8_t padding[3];      // Alinhamento (total: 64 bytes)
} __attribute__((packed));
#endif

/**
 * @brief Informações de peer (dispositivo conectado)
 */
struct PeerInfo {
    uint8_t macAddress[6];   // MAC address do peer
    String deviceName;       // Nome do dispositivo
    String deviceType;       // Tipo do dispositivo
    bool online;            // Status online/offline
    unsigned long lastSeen; // Último contato
    int rssi;               // Força do sinal
};

/**
 * @brief Classe para controle de comunicação ESP-NOW
 */
class ESPNowController {
public:
    // 🔄 CORREÇÃO: Permitir acesso de MasterSlaveManager a métodos internos
    friend class MasterSlaveManager;
    
    // ===== 🎨 ADAPTER PATTERN: Novo Construtor =====
    /**
     * @brief Construtor com backend externo (RECOMENDADO para Master)
     * 
     * Use este construtor quando já existe um ESPNowTask criado.
     * O backend roda no Core 1, mantendo performance máxima.
     * 
     * @param existingBackend Ponteiro para ESPNowTask já inicializado
     * @param deviceName Nome do dispositivo local
     */
    ESPNowController(ESPNowTask* existingBackend, const String& deviceName = "ESP32Device");
    
    /**
     * @brief Construtor standalone (COMPATIBILIDADE)
     * 
     * Use este construtor quando ESPNowController é o único usuário.
     * Cria um backend interno automaticamente.
     * 
     * @param deviceName Nome do dispositivo local
     * @param channel Canal WiFi a usar (1-14)
     */
    ESPNowController(const String& deviceName = "ESP32Device", uint8_t channel = 1);
    
    /**
     * @brief Inicializa ESP-NOW
     * @return true se inicialização foi bem sucedida
     */
    bool begin();
    
    /**
     * @brief Atualiza sistema (chamar no loop principal)
     */
    void update();
    
    /**
     * @brief Para o sistema ESP-NOW
     */
    void end();
    
    /**
     * @brief Atualiza o nome do dispositivo
     * @param newDeviceName Novo nome do dispositivo
     */
    void setDeviceName(const String& newDeviceName);
    
    // ===== ENVIO DE MENSAGENS =====
    
    /**
     * @brief Envia comando de relé para dispositivo específico
     * @param targetMac MAC address do dispositivo alvo
     * @param relayNumber Número do relé
     * @param action Ação ("on", "off", "toggle", "status")
     * @param duration Duração em segundos (0 = sem timer)
     * @return true se mensagem foi enviada
     */
    bool sendRelayCommand(const uint8_t* targetMac, int relayNumber, const String& action, int duration = 0,
                          uint32_t commandId = 0, int cycleOffDuration = 0, const String& mode = "");

    bool sendSetRelayMask(const uint8_t* targetMac, uint8_t mask, uint16_t durationSec, uint32_t commandId);

    /**
     * @brief Envia ACK de comando de relé (slave → master, formato TaskESPNowMessage)
     */
    bool sendRelayCommandAck(const uint8_t* targetMac, const RelayCommandAck& ack);
    
    /**
     * @brief Envia status de relé
     * @param targetMac MAC address do destinatário (nullptr para broadcast)
     * @param relayNumber Número do relé
     * @param state Estado atual
     * @param hasTimer Tem timer ativo
     * @param remainingTime Tempo restante
     * @param name Nome do relé
     * @return true se mensagem foi enviada
     */
    bool sendRelayStatus(const uint8_t* targetMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name);
    
    /**
     * @brief Envia estado completo de TODOS os relés (ALL_RELAYS_STATUS)
     * @param targetMac MAC address do destinatário (nullptr para broadcast)
     * @param relayStates Array com estados dos 8 relés
     * @param numRelays Número total de relés (geralmente 8)
     * @return true se mensagem foi enviada
     */
    bool sendAllRelaysStatus(const uint8_t* targetMac, const SingleRelayState relayStates[8], uint8_t numRelays = 8);
    
    /**
     * @brief Envia estados persistentes para sincronização (NVS)
     * @param targetMac MAC address do destinatário (nullptr para broadcast)
     * @param states Estados persistentes dos relés
     * @return true se mensagem foi enviada
     */
    bool sendPersistentStateSync(const uint8_t* targetMac, const PersistentRelayStateData& states);
    
    /**
     * @brief Envia informações do dispositivo
     * @param targetMac MAC address do destinatário (nullptr para broadcast)
     * @param deviceType Tipo do dispositivo
     * @param numRelays Número de relés
     * @param operational Status operacional
     * @param uptime Uptime
     * @param freeHeap Memória livre
     * @return true se mensagem foi enviada
     */
    bool sendDeviceInfo(const uint8_t* targetMac, const String& deviceType, uint8_t numRelays, bool operational, uint32_t uptime, uint32_t freeHeap);
    
    /**
     * @brief Envia ping para dispositivo
     * @param targetMac MAC address do dispositivo
     * @return true se ping foi enviado
     */
    bool sendPing(const uint8_t* targetMac);
    
    /**
     * @brief Envia mensagem broadcast para descoberta de dispositivos
     * @return true se broadcast foi enviado
     */
    bool sendDiscoveryBroadcast();

    /**
     * @brief Escaneia canais WiFi e sincroniza com o Master (modo Slave sem WiFi STA)
     * @return true se Master foi detectado num canal
     */
    bool scanAndSyncToMasterChannel();

    /**
     * @brief Indica se houve contacto recente com o Master
     */
    bool hasRecentMasterContact() const;
    
    /**
     * @brief Envia credenciais WiFi em broadcast para todos os dispositivos
     * @param ssid Nome da rede WiFi
     * @param password Senha da rede WiFi
     * @param channel Canal WiFi (opcional, se 0 usa canal atual)
     * @return true se credenciais foram enviadas
     */
    bool sendWiFiCredentialsBroadcast(const String& ssid, const String& password, uint8_t channel = 0);

    /**
     * @brief Salta temporariamente para um canal WiFi (sem desconectar STA)
     */
    bool hopToChannel(uint8_t channel);

    /**
     * @brief Canal RF actual del ESP32
     */
    uint8_t getCurrentRadioChannel() const;

    /**
     * @brief Notifica slaves no canal antigo sobre mudança de canal (3× broadcast)
     */
    bool sendChannelChangeNotification(uint8_t oldChannel, uint8_t newChannel, uint8_t reason = 1);
    
    /**
     * @brief Valida credenciais WiFi recebidas (verifica checksum)
     * @param credentials Estrutura com credenciais
     * @return true se credenciais são válidas
     */
    bool validateWiFiCredentials(const WiFiCredentialsData& credentials);
    
    /**
     * @brief Inicia handshake bidirecional com dispositivo
     * @param targetMac MAC address do dispositivo alvo
     * @return true se handshake foi iniciado
     */
    bool initiateHandshake(const uint8_t* targetMac);
    
    /**
     * @brief Responde a handshake recebido
     * @param targetMac MAC address do dispositivo que solicitou
     * @param sessionId ID da sessão recebida
     * @return true se resposta foi enviada
     */
    bool respondToHandshake(const uint8_t* targetMac, uint32_t sessionId);
    
    /**
     * @brief Envia relatório de conectividade
     * @param targetMac MAC address do destinatário (nullptr para broadcast)
     * @param sessionId ID da sessão
     * @return true se relatório foi enviado
     */
    bool sendConnectivityReport(const uint8_t* targetMac, uint32_t sessionId);
    
    /**
     * @brief Solicita verificação de conectividade
     * @param targetMac MAC address do dispositivo alvo
     * @return true se solicitação foi enviada
     */
    bool requestConnectivityCheck(const uint8_t* targetMac);
    
    /**
     * @brief Valida handshake recebido
     * @param handshake Estrutura do handshake
     * @return true se handshake é válido
     */
    bool validateHandshake(const HandshakeData& handshake);
    
    /**
     * @brief Gera ID único de sessão
     * @return ID de sessão único
     */
    uint32_t generateSessionId();
    
    // ===== GERENCIAMENTO DE PEERS =====
    
    /**
     * @brief Adiciona peer manualmente
     * @param macAddress MAC address do peer
     * @param deviceName Nome do dispositivo (opcional)
     * @return true se peer foi adicionado
     */
    bool addPeer(const uint8_t* macAddress, const String& deviceName = "");
    
    /**
     * @brief Adiciona peer com canal específico (CORRETO!)
     * @param macAddress MAC address do peer
     * @param peerChannel Canal WiFi do peer (1-13)
     * @param deviceName Nome do dispositivo (opcional)
     * @return true se peer foi adicionado
     */
    bool addPeerWithChannel(const uint8_t* macAddress, uint8_t peerChannel, const String& deviceName = "");
    
    /**
     * @brief Adiciona peer com verificação de canal (método seguro)
     * Garante que o peer seja adicionado no canal correto
     * @param macAddress MAC address do peer
     * @param deviceName Nome do dispositivo (opcional)
     * @return true se peer foi adicionado
     */
    bool addPeerSafe(const uint8_t* macAddress, const String& deviceName = "");
    
    /**
     * @brief Remove peer
     * @param macAddress MAC address do peer
     * @return true se peer foi removido
     */
    bool removePeer(const uint8_t* macAddress);
    
    /**
     * @brief Obtém lista de peers
     * @return Vector com informações dos peers
     */
    std::vector<PeerInfo> getPeerList();
    
    /**
     * @brief Verifica se peer existe
     * @param macAddress MAC address do peer
     * @return true se peer existe
     */
    bool peerExists(const uint8_t* macAddress);
    
    /**
     * @brief Obtém número de peers conectados
     * @return Número de peers
     */
    int getPeerCount();
    
    // ===== CALLBACKS =====
    
    /**
     * @brief Define callback para comandos de relé recebidos
     * @param callback Função a ser chamada
     */
    void setRelayCommandCallback(std::function<void(const uint8_t* senderMac, uint32_t commandId, int relayNumber,
                                                    const String& action, int duration, const String& mode,
                                                    int cycleOffDuration)> callback);

    /**
     * @brief Callback para sincronização de estados persistentes (PERSISTENT_STATE_SYNC)
     */
    void setPersistentStateCallback(std::function<void(const uint8_t* senderMac, const PersistentRelayStateData& states)> callback);
    
    /**
     * @brief Define callback para status de relé recebido
     * @param callback Função a ser chamada
     */
    void setRelayStatusCallback(std::function<void(const uint8_t* senderMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name)> callback);
    
    /**
     * @brief Define callback para informações de dispositivo recebidas
     * @param callback Função a ser chamada
     */
    void setDeviceInfoCallback(std::function<void(const uint8_t* senderMac, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel)> callback);
    
    /**
     * @brief Define callback para ping recebido
     * @param callback Função a ser chamada
     */
    void setPingCallback(void (*callback)(const uint8_t* senderMac));

    /**
     * @brief Define callback para PONG recebido (master respondeu ao ping do slave)
     */
    void setPongCallback(void (*callback)(const uint8_t* senderMac));
    
    /**
     * @brief Define callback para credenciais WiFi recebidas
     * @param callback Função a ser chamada
     */
    void setWiFiCredentialsCallback(void (*callback)(const String& ssid, const String& password, uint8_t channel));
    
    /**
     * @brief Define callback para handshake recebido
     * @param callback Função a ser chamada
     */
    void setHandshakeCallback(void (*callback)(const uint8_t* senderMac, uint32_t sessionId, const String& deviceName, bool wifiConnected));
    
    /**
     * @brief Define callback para relatório de conectividade recebido
     * @param callback Função a ser chamada
     */
    void setConnectivityReportCallback(void (*callback)(const uint8_t* senderMac, uint32_t sessionId, bool wifiConnected, int32_t rssi, uint32_t uptime));
    
    /**
     * @brief Define callback para solicitação de verificação de conectividade
     * @param callback Função a ser chamada
     */
    void setConnectivityCheckCallback(void (*callback)(const uint8_t* senderMac));
    
    /**
     * @brief Define callback para mensagens de erro
     * @param callback Função a ser chamada
     */
    void setErrorCallback(void (*callback)(const String& error));
    
    // ===== UTILITÁRIOS =====
    
    /**
     * @brief Converte MAC address para string
     * @param mac Array de 6 bytes com MAC
     * @return String formatada (XX:XX:XX:XX:XX:XX)
     */
    static String macToString(const uint8_t* mac);
    
    /**
     * @brief Converte string para MAC address
     * @param macStr String no formato XX:XX:XX:XX:XX:XX
     * @param mac Array de 6 bytes para armazenar resultado
     * @return true se conversão foi bem sucedida
     */
    static bool stringToMac(const String& macStr, uint8_t* mac);
    
    /**
     * @brief Obtém MAC address local
     * @param mac Array de 6 bytes para armazenar MAC local
     */
    void getLocalMac(uint8_t* mac);
    
    /**
     * @brief Obtém MAC address local como string
     * @return String com MAC local
     */
    String getLocalMacString();
    
    /**
     * @brief Verifica se sistema está inicializado
     * @return true se ESP-NOW está funcionando
     */
    bool isInitialized() { return initialized; }
    
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
     * @brief Envia mensagem ESP-NOW (método público para MasterSlaveManager)
     * @param message Estrutura da mensagem
     * @param targetMac MAC de destino (nullptr para broadcast)
     * @return true se mensagem foi enviada
     */
    bool sendMessage(const ESPNowMessage& message, const uint8_t* targetMac);
    
    /**
     * @brief Calcula checksum da mensagem (método público para MasterSlaveManager)
     * @param message Mensagem para calcular checksum
     * @return Valor do checksum
     */
    uint8_t calculateChecksum(const ESPNowMessage& message);

private:
    // ===== 🎨 ADAPTER PATTERN: Backend =====
    ESPNowTask* backend;                // Ponteiro para backend real (Core 1)
    bool ownsBackend;                   // True se criou o backend, false se recebeu externo
    
    // ===== MEMBROS ORIGINAIS =====
    String deviceName;                    // Nome do dispositivo local
    uint8_t wifiChannel;                 // Canal WiFi
    bool initialized;                    // Status de inicialização
    uint32_t messageCounter;            // Contador de mensagens enviadas
    
    // Estatísticas
    uint32_t messagesSent;
    uint32_t messagesReceived;
    uint32_t messagesLost;
    uint32_t lastMessageId;

    // Anti-replay local (messageId por remetente — não depende de millis() cross-device)
    struct RecentSenderEntry {
        uint8_t mac[6];
        uint32_t lastMessageId;
        unsigned long lastSeenMs;
    };
    static const size_t MAX_RECENT_SENDERS = 12;
    RecentSenderEntry recentSenders[MAX_RECENT_SENDERS];
    size_t recentSenderCount;

    unsigned long lastMasterContactMs;
    bool masterContactEstablished;

    bool isDuplicateMessage(const ESPNowMessage& message);
    void recordMessageId(const ESPNowMessage& message);
    bool setWifiChannel(uint8_t channel);
    
    // Lista de peers conhecidos
    std::vector<PeerInfo> knownPeers;
    
    // Callbacks
    std::function<void(const uint8_t* senderMac, uint32_t commandId, int relayNumber, const String& action,
                       int duration, const String& mode, int cycleOffDuration)> relayCommandCallback = nullptr;
    std::function<void(const uint8_t* senderMac, const PersistentRelayStateData& states)> persistentStateCallback = nullptr;
    std::function<void(const uint8_t* senderMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name)> relayStatusCallback = nullptr;
     std::function<void(const uint8_t* senderMac, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel)> deviceInfoCallback = nullptr;
    void (*pingCallback)(const uint8_t* senderMac) = nullptr;
    void (*pongCallback)(const uint8_t* senderMac) = nullptr;
    void (*errorCallback)(const String& error) = nullptr;
    void (*wifiCredentialsCallback)(const String& ssid, const String& password, uint8_t channel) = nullptr;
    void (*handshakeCallback)(const uint8_t* senderMac, uint32_t sessionId, const String& deviceName, bool wifiConnected) = nullptr;
    void (*connectivityReportCallback)(const uint8_t* senderMac, uint32_t sessionId, bool wifiConnected, int32_t rssi, uint32_t uptime) = nullptr;
    void (*connectivityCheckCallback)(const uint8_t* senderMac) = nullptr;
    
    // ===== MÉTODOS PRIVADOS =====
    
    /**
     * @brief Processa mensagem recebida
     * @param message Mensagem recebida
     * @param senderMac MAC do remetente
     */
    void processReceivedMessage(const ESPNowMessage& message, const uint8_t* senderMac);
    
    /**
     * @brief Valida mensagem recebida
     * @param message Mensagem para validar
     * @return true se mensagem é válida
     */
    bool validateMessage(const ESPNowMessage& message, const uint8_t* senderMac = nullptr);
    
    /**
     * @brief Atualiza informações do peer
     * @param macAddress MAC do peer
     * @param deviceName Nome do dispositivo
     * @param deviceType Tipo do dispositivo
     */
    void updatePeerInfo(const uint8_t* macAddress, const String& deviceName, const String& deviceType);
    
    /**
     * @brief Remove peers offline (não vistos há muito tempo)
     */
    void cleanupOfflinePeers();
    
    /**
     * @brief Gera código de validação genérico (usado para handshakes)
     * @param text1 Primeiro texto para XOR
     * @param text2 Segundo texto para XOR
     * @param value Valor numérico para XOR
     * @return Código de validação
     */
    uint8_t generateValidationCode(const String& text1, const String& text2, uint32_t value);
    
    /**
     * @brief Callback estático para recebimento de mensagens ESP-NOW
     */
    static void onDataReceived(const uint8_t* mac, const uint8_t* incomingData, int len);
    
    /**
     * @brief Callback estático para confirmação de envio ESP-NOW
     */
    static void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status);
    
    // Instância estática para callbacks
    static ESPNowController* instance;
};

#endif // ESPNOW_CONTROLLER_H
