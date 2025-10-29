#ifndef MASTER_ESPNOW_RELAY_CONTROLLER_H
#define MASTER_ESPNOW_RELAY_CONTROLLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>

// Incluir as classes do projeto
#include "ESPNowController.h"
#include "RelayCommandBox.h"
#include "ESPNowBridge.h"

/**
 * MASTER ESP-NOW RELAY CONTROLLER
 * Sistema Master para controle de múltiplos dispositivos Slave via ESP-NOW
 * Baseado no projeto ESP-HIDROWAVE
 */

class MasterRelayController {
private:
    // Instâncias principais
    ESPNowController* espNow;
    RelayCommandBox* localRelayBox;  // 8 relés via PCF8574
    ESPNowBridge* bridge;
    
    // Configurações
    String masterName;
    int wifiChannel;
    bool systemInitialized;
    
    // Lista de dispositivos escravos
    std::vector<RemoteDevice> slaveDevices;
    std::map<String, uint8_t[6]> knownDevices; // Nome -> MAC
    
    // Estatísticas
    unsigned long lastDiscovery;
    unsigned long lastStatusCheck;
    unsigned long lastCommandSent;
    
    // Configurações de timing
    static const unsigned long DISCOVERY_INTERVAL = 30000;    // 30 segundos
    static const unsigned long STATUS_CHECK_INTERVAL = 10000; // 10 segundos
    static const unsigned long COMMAND_INTERVAL = 1000;       // 1 segundo
    
    // Callbacks
    void (*onSlaveDiscovered)(const String& name, const String& type, bool operational);
    void (*onSlaveStatusChanged)(const String& deviceName, int relay, bool state, int remainingTime);
    void (*onCommandResult)(const String& deviceName, int relay, bool success, const String& error);

public:
    /**
     * @brief Construtor
     * @param name Nome do Master
     * @param channel Canal WiFi
     * @param localPCFAddress Endereço do PCF8574 local
     */
    MasterRelayController(const String& name, int channel = 1, uint8_t localPCFAddress = 0x20);
    
    /**
     * @brief Destrutor
     */
    ~MasterRelayController();
    
    /**
     * @brief Inicializa o sistema
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
    
    // ===== COMANDOS PARA DISPOSITIVOS ESPECÍFICOS =====
    
    /**
     * @brief Envia comando para dispositivo específico
     * @param deviceName Nome do dispositivo
     * @param relayNumber Número do relé
     * @param action Ação ("on", "off", "toggle", "status")
     * @param duration Duração em segundos (0 = sem timer)
     * @return true se comando foi enviado
     */
    bool sendCommandToSlave(const String& deviceName, int relayNumber, const String& action, int duration = 0);
    
    /**
     * @brief Envia comando para dispositivo por MAC
     * @param targetMac MAC address do dispositivo
     * @param relayNumber Número do relé
     * @param action Ação
     * @param duration Duração
     * @return true se comando foi enviado
     */
    bool sendCommandToSlaveByMac(const uint8_t* targetMac, int relayNumber, const String& action, int duration = 0);
    
    // ===== COMANDOS BROADCAST =====
    
    /**
     * @brief Envia comando para todos os dispositivos
     * @param relayNumber Número do relé
     * @param action Ação
     * @param duration Duração
     * @return Número de dispositivos que receberam o comando
     */
    int broadcastCommand(int relayNumber, const String& action, int duration = 0);
    
    /**
     * @brief Envia comando para dispositivos por tipo
     * @param deviceType Tipo do dispositivo
     * @param relayNumber Número do relé
     * @param action Ação
     * @param duration Duração
     * @return Número de dispositivos que receberam o comando
     */
    int broadcastCommandToType(const String& deviceType, int relayNumber, const String& action, int duration = 0);
    
    // ===== DESCOBERTA DE DISPOSITIVOS =====
    
    /**
     * @brief Força descoberta de dispositivos
     */
    void discoverSlaveDevices();
    
    /**
     * @brief Adiciona dispositivo manualmente
     * @param deviceName Nome do dispositivo
     * @param macAddress MAC address
     * @return true se dispositivo foi adicionado
     */
    bool addSlaveDevice(const String& deviceName, const uint8_t* macAddress);
    
    /**
     * @brief Remove dispositivo
     * @param deviceName Nome do dispositivo
     * @return true se dispositivo foi removido
     */
    bool removeSlaveDevice(const String& deviceName);
    
    // ===== INFORMAÇÕES DO SISTEMA =====
    
    /**
     * @brief Obtém lista de dispositivos escravos
     * @return Vector com dispositivos
     */
    std::vector<RemoteDevice> getSlaveDevices();
    
    /**
     * @brief Obtém número de dispositivos online
     * @return Número de dispositivos online
     */
    int getOnlineSlaveCount();
    
    /**
     * @brief Obtém dispositivo por nome
     * @param deviceName Nome do dispositivo
     * @return Ponteiro para dispositivo ou nullptr
     */
    RemoteDevice* getSlaveDevice(const String& deviceName);
    
    /**
     * @brief Obtém status do sistema em JSON
     * @return String JSON com status
     */
    String getSystemStatusJSON();
    
    /**
     * @brief Imprime status do sistema
     */
    void printSystemStatus();
    
    // ===== CALLBACKS =====
    
    /**
     * @brief Define callback para descoberta de dispositivos
     * @param callback Função a ser chamada
     */
    void setSlaveDiscoveredCallback(void (*callback)(const String& name, const String& type, bool operational));
    
    /**
     * @brief Define callback para mudanças de status
     * @param callback Função a ser chamada
     */
    void setSlaveStatusChangedCallback(void (*callback)(const String& deviceName, int relay, bool state, int remainingTime));
    
    /**
     * @brief Define callback para resultados de comandos
     * @param callback Função a ser chamada
     */
    void setCommandResultCallback(void (*callback)(const String& deviceName, int relay, bool success, const String& error));
    
    // ===== UTILITÁRIOS =====
    
    /**
     * @brief Obtém nome do Master
     * @return String com nome
     */
    String getMasterName() const { return masterName; }
    
    /**
     * @brief Verifica se sistema está inicializado
     * @return true se inicializado
     */
    bool isInitialized() const { return systemInitialized; }
    
    /**
     * @brief Obtém canal WiFi
     * @return Canal WiFi
     */
    int getWifiChannel() const { return wifiChannel; }

private:
    // ===== MÉTODOS PRIVADOS =====
    
    /**
     * @brief Configura callbacks do sistema
     */
    void setupCallbacks();
    
    /**
     * @brief Trata comando recebido
     * @param senderMac MAC do remetente
     * @param relayNumber Número do relé
     * @param action Ação
     * @param duration Duração
     */
    void handleReceivedCommand(const uint8_t* senderMac, int relayNumber, const String& action, int duration);
    
    /**
     * @brief Trata status recebido
     * @param senderMac MAC do remetente
     * @param relayNumber Número do relé
     * @param state Estado
     * @param hasTimer Tem timer
     * @param remainingTime Tempo restante
     * @param name Nome do relé
     */
    void handleReceivedStatus(const uint8_t* senderMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name);
    
    /**
     * @brief Trata informações de dispositivo recebidas
     * @param senderMac MAC do remetente
     * @param deviceName Nome do dispositivo
     * @param deviceType Tipo do dispositivo
     * @param numRelays Número de relés
     * @param operational Status operacional
     */
    void handleReceivedDeviceInfo(const uint8_t* senderMac, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational);
    
    /**
     * @brief Verifica status dos dispositivos escravos
     */
    void checkSlaveDevicesStatus();
};

#endif // MASTER_ESPNOW_RELAY_CONTROLLER_H
