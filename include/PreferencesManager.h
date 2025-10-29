#ifndef PREFERENCES_MANAGER_H
#define PREFERENCES_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "ConfigUnified.h"

/**
 * @brief Gerenciador unificado de preferências para todos os projetos ESP-NOW
 * 
 * Este classe fornece uma interface unificada para salvar e carregar configurações
 * usando tanto Arduino Preferences quanto NVS nativo do ESP32.
 */
class PreferencesManager {
public:
    // ===== INICIALIZAÇÃO =====
    
    /**
     * @brief Inicializa o sistema de preferências
     * @return true se inicialização foi bem-sucedida
     */
    static bool begin();
    
    /**
     * @brief Finaliza o sistema de preferências
     */
    static void end();
    
    // ===== CREDENCIAIS WIFI =====
    
    /**
     * @brief Salva credenciais WiFi
     * @param ssid Nome da rede WiFi
     * @param password Senha da rede WiFi
     * @param channel Canal WiFi
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveWiFiCredentials(const String& ssid, const String& password, uint8_t channel);
    
    /**
     * @brief Carrega credenciais WiFi
     * @param ssid Referência para armazenar SSID
     * @param password Referência para armazenar senha
     * @param channel Referência para armazenar canal
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadWiFiCredentials(String& ssid, String& password, uint8_t& channel);
    
    /**
     * @brief Remove credenciais WiFi
     * @return true se remoção foi bem-sucedida
     */
    static bool clearWiFiCredentials();
    
    // ===== CONFIGURAÇÕES DE RELÉS =====
    
    /**
     * @brief Salva configuração de um relé
     * @param relayNumber Número do relé (0-15)
     * @param name Nome do relé
     * @param enabled Se o relé está habilitado
     * @param defaultDuration Duração padrão em segundos
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveRelayConfig(int relayNumber, const String& name, bool enabled, int defaultDuration = 0);
    
    /**
     * @brief Carrega configuração de um relé
     * @param relayNumber Número do relé (0-15)
     * @param name Referência para armazenar nome
     * @param enabled Referência para armazenar status
     * @param defaultDuration Referência para armazenar duração padrão
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadRelayConfig(int relayNumber, String& name, bool& enabled, int& defaultDuration);
    
    /**
     * @brief Salva configurações de todos os relés
     * @param relayNames Array de nomes dos relés
     * @param relayEnabled Array de status dos relés
     * @param relayDurations Array de durações padrão
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveAllRelayConfigs(const String relayNames[], const bool relayEnabled[], const int relayDurations[]);
    
    /**
     * @brief Carrega configurações de todos os relés
     * @param relayNames Array para armazenar nomes
     * @param relayEnabled Array para armazenar status
     * @param relayDurations Array para armazenar durações
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadAllRelayConfigs(String relayNames[], bool relayEnabled[], int relayDurations[]);
    
    // ===== CONFIGURAÇÕES DE DISPOSITIVO =====
    
    /**
     * @brief Salva configurações do dispositivo
     * @param deviceName Nome do dispositivo
     * @param deviceType Tipo do dispositivo
     * @param deviceMode Modo do dispositivo (Master/Slave/Standalone)
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveDeviceConfig(const String& deviceName, const String& deviceType, const String& deviceMode);
    
    /**
     * @brief Carrega configurações do dispositivo
     * @param deviceName Referência para armazenar nome
     * @param deviceType Referência para armazenar tipo
     * @param deviceMode Referência para armazenar modo
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadDeviceConfig(String& deviceName, String& deviceType, String& deviceMode);
    
    // ===== CONFIGURAÇÕES DE SENSORES =====
    
    /**
     * @brief Salva calibração de sensor
     * @param sensorType Tipo do sensor (TEMP, PH, TDS, etc.)
     * @param calibrationValue Valor de calibração
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveSensorCalibration(const String& sensorType, float calibrationValue);
    
    /**
     * @brief Carrega calibração de sensor
     * @param sensorType Tipo do sensor
     * @param calibrationValue Referência para armazenar valor
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadSensorCalibration(const String& sensorType, float& calibrationValue);
    
    /**
     * @brief Salva configurações de sensores
     * @param updateInterval Intervalo de atualização em ms
     * @param enabledSensors Array de sensores habilitados
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveSensorConfig(uint32_t updateInterval, const bool enabledSensors[]);
    
    /**
     * @brief Carrega configurações de sensores
     * @param updateInterval Referência para armazenar intervalo
     * @param enabledSensors Array para armazenar status
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadSensorConfig(uint32_t& updateInterval, bool enabledSensors[]);
    
    // ===== CONFIGURAÇÕES ESP-NOW =====
    
    /**
     * @brief Salva configurações ESP-NOW
     * @param channel Canal WiFi
     * @param maxPeers Número máximo de peers
     * @param encryptionEnabled Se criptografia está habilitada
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveESPNowConfig(uint8_t channel, uint8_t maxPeers, bool encryptionEnabled);
    
    /**
     * @brief Carrega configurações ESP-NOW
     * @param channel Referência para armazenar canal
     * @param maxPeers Referência para armazenar max peers
     * @param encryptionEnabled Referência para armazenar status de criptografia
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadESPNowConfig(uint8_t& channel, uint8_t& maxPeers, bool& encryptionEnabled);
    
    /**
     * @brief Salva lista de peers ESP-NOW
     * @param peerMacs Array de MACs dos peers
     * @param peerNames Array de nomes dos peers
     * @param peerCount Número de peers
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveESPNowPeers(const uint8_t peerMacs[][6], const String peerNames[], uint8_t peerCount);
    
    /**
     * @brief Carrega lista de peers ESP-NOW
     * @param peerMacs Array para armazenar MACs
     * @param peerNames Array para armazenar nomes
     * @param peerCount Referência para armazenar número de peers
     * @param maxPeers Número máximo de peers a carregar
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadESPNowPeers(uint8_t peerMacs[][6], String peerNames[], uint8_t& peerCount, uint8_t maxPeers);
    
    // ===== CONFIGURAÇÕES DE API =====
    
    /**
     * @brief Salva configurações de API
     * @param apiUrl URL da API
     * @param apiKey Chave da API
     * @param updateInterval Intervalo de atualização
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveAPIConfig(const String& apiUrl, const String& apiKey, uint32_t updateInterval);
    
    /**
     * @brief Carrega configurações de API
     * @param apiUrl Referência para armazenar URL
     * @param apiKey Referência para armazenar chave
     * @param updateInterval Referência para armazenar intervalo
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadAPIConfig(String& apiUrl, String& apiKey, uint32_t& updateInterval);
    
    // ===== CONFIGURAÇÕES GERAIS =====
    
    /**
     * @brief Salva configuração geral
     * @param key Chave da configuração
     * @param value Valor da configuração
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveConfig(const String& key, const String& value);
    
    /**
     * @brief Carrega configuração geral
     * @param key Chave da configuração
     * @param value Referência para armazenar valor
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadConfig(const String& key, String& value);
    
    /**
     * @brief Salva configuração numérica
     * @param key Chave da configuração
     * @param value Valor numérico
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveConfigInt(const String& key, int32_t value);
    
    /**
     * @brief Carrega configuração numérica
     * @param key Chave da configuração
     * @param value Referência para armazenar valor
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadConfigInt(const String& key, int32_t& value);
    
    /**
     * @brief Salva configuração float
     * @param key Chave da configuração
     * @param value Valor float
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveConfigFloat(const String& key, float value);
    
    /**
     * @brief Carrega configuração float
     * @param key Chave da configuração
     * @param value Referência para armazenar valor
     * @return true se carregamento foi bem-sucedido
     */
    static bool loadConfigFloat(const String& key, float& value);
    
    // ===== UTILITÁRIOS =====
    
    /**
     * @brief Verifica se uma configuração existe
     * @param key Chave da configuração
     * @return true se configuração existe
     */
    static bool configExists(const String& key);
    
    /**
     * @brief Remove uma configuração
     * @param key Chave da configuração
     * @return true se remoção foi bem-sucedida
     */
    static bool removeConfig(const String& key);
    
    /**
     * @brief Limpa todas as configurações
     * @return true se limpeza foi bem-sucedida
     */
    static bool clearAllConfigs();
    
    /**
     * @brief Obtém estatísticas do armazenamento
     * @param usedSpace Espaço usado em bytes
     * @param freeSpace Espaço livre em bytes
     * @return true se estatísticas foram obtidas
     */
    static bool getStorageStats(size_t& usedSpace, size_t& freeSpace);
    
    /**
     * @brief Obtém informações de versão da configuração
     * @param version Referência para armazenar versão
     * @return true se versão foi obtida
     */
    static bool getConfigVersion(uint32_t& version);
    
    /**
     * @brief Salva versão da configuração
     * @param version Versão da configuração
     * @return true se salvamento foi bem-sucedido
     */
    static bool saveConfigVersion(uint32_t version);
    
    // ===== MÉTODOS DE COMPATIBILIDADE =====
    
    /**
     * @brief Migra configurações de versão antiga
     * @param fromVersion Versão de origem
     * @param toVersion Versão de destino
     * @return true se migração foi bem-sucedida
     */
    static bool migrateConfig(uint32_t fromVersion, uint32_t toVersion);
    
    /**
     * @brief Exporta configurações para JSON
     * @param json Referência para armazenar JSON
     * @return true se exportação foi bem-sucedida
     */
    static bool exportToJSON(String& json);
    
    /**
     * @brief Importa configurações de JSON
     * @param json JSON com configurações
     * @return true se importação foi bem-sucedida
     */
    static bool importFromJSON(const String& json);
    
private:
    static Preferences preferences;
    static bool initialized;
    static bool useNVS;
    
    /**
     * @brief Garante que o sistema está inicializado
     * @return true se inicializado
     */
    static bool ensureInitialized();
    
    /**
     * @brief Inicializa NVS se necessário
     * @return true se inicialização foi bem-sucedida
     */
    static bool initNVS();
    
    /**
     * @brief Valida chave de configuração
     * @param key Chave a validar
     * @return true se chave é válida
     */
    static bool validateKey(const String& key);
    
    /**
     * @brief Valida valor de configuração
     * @param value Valor a validar
     * @return true se valor é válido
     */
    static bool validateValue(const String& value);
};

#endif // PREFERENCES_MANAGER_H
