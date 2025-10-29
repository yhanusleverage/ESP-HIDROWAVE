#include <WiFi.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include "HybridStateManager.h"
#include "Config.h"
#include "DeviceID.h"
#include <esp_task_wdt.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_now.h>

// ===== INCLUDES PARA MODO MASTER ESP-NOW =====
#include "ESPNowBridge.h"
#include "RelayCommandBox.h"
#include "SaveManager.h"
// #include "WiFiCredentialsManager.h"  // DESABILITADO - slaves não precisam WiFi
#include "SafetyWatchdog.h"
#include "AutoCommunicationManager.h"  // 🧠 PILAR INTELIGENTE
#include "ESPNowTask.h"  // 🚀 TASK DEDICADA ESP-NOW
#include "RelayBridge.h"  // 🌉 CAPA DE TRADUCCIÓN SUPABASE ↔ ESP-NOW
#include <vector>

// ===== SISTEMA DE PROTEÇÃO GLOBAL =====
HydroStateManager stateManager;
unsigned long systemStartTime = 0;
uint32_t minHeapSeen = UINT32_MAX;
unsigned long lastMemoryCheck = 0;

// ===== VARIÁVEIS GLOBAIS PARA WEBSERVERMANAGER =====
bool systemInitialized = true;  // Sistema sempre inicializado quando chega ao modo hydro
bool supabaseConnected = false; // Será atualizado pelo HydroSystemCore
bool webServerRunning = false;  // Será atualizado pelo WebServerManager

// ===== VARIÁVEIS GLOBAIS PARA MODO MASTER ESP-NOW =====
// Descomente a linha correspondente ao modo desejado:
//#define MASTER_MODE    // ← Definido em platformio.ini
//#define SLAVE_MODE     // Descomente para modo Slave

// ===== PROTÓTIPOS DE FUNÇÃO =====
#ifdef MASTER_MODE
void setupMasterCallbacks();
void addSlaveToList(const uint8_t* macAddress, const String& deviceName, const String& deviceType, uint8_t numRelays);
uint8_t* findSlaveMac(const String& slaveName);
void printSlavesList();
void controlRelay(const String& slaveName, int relayNumber, const String& action, int duration);
void controlAllRelays(int relayNumber, const String& action, int duration);
void discoverSlaves();
void monitorSlaves();
void handleMasterSerialCommands();
void handleRelayCommand(const String& command);
void printMasterHelp();
void printMasterStatus();
// sendWiFiCredentialsBroadcast() REMOVIDA - slaves não precisam de WiFi
// sendSavedWiFiCredentialsBroadcast() REMOVIDA - slaves não precisam de WiFi
void registerBroadcastPeer();
void monitorSlavesAutomatically();
void attemptSlaveReconnection(RemoteDevice* slave);
void checkSignalQuality();
void implementFallbackStrategy(RemoteDevice* slave);
void automaticCommunicationDecisions();
void checkCommunicationIntegrity();
void cleanupOfflinePeers();
void autoDiscoverAndConnect();
void maintainESPNOWConnection();
void reconnectESPNOWSlaves();
#endif

#ifdef SLAVE_MODE
void handleSlaveSerialCommands();
void printSlaveHelp();
void handleSlaveRelayCommand(const String& command);
#endif

#ifdef MASTER_MODE
    // 🚀 TASK DEDICADA ESP-NOW (Nova arquitetura)
    ESPNowTask* espNowTask = nullptr;          // Task dedicada ESP-NOW
    
    // 🌉 RELAY BRIDGE - Capa de traducción Supabase ↔ ESP-NOW
    RelayBridge* relayBridge = nullptr;        // Bridge Supabase → ESP-NOW
    
    // Instância do ESP-NOW Master usando ESPNowBridge (Legacy - manter para compatibilidade)
    RelayCommandBox* masterRelayBox = nullptr;  // Relés locais do MASTER
    ESPNowBridge* masterBridge = nullptr;       // Ponte ESP-NOW
    
    // Gerenciador de configurações
    SaveManager configManager;
    SafetyWatchdog watchdog;  // Sistema de segurança
    
    // 🧠 SISTEMA INTELIGENTE DE COMUNICAÇÃO
    AutoCommunicationManager* autoComm = nullptr;
    // WiFiCredentialsManager wifiCredManager;  // DESABILITADO - slaves não precisam WiFi
    
    // Lista de slaves conhecidos (Legacy)
    std::vector<RemoteDevice> knownSlaves;
    
    // Sistema de monitoramento automático
    unsigned long lastSlaveCheck = 0;
    unsigned long lastReconnectionAttempt = 0;
    unsigned long lastSignalCheck = 0;
    int failedPingCount = 0;
    int maxFailedPings = 3;
    
    // Buffer para comandos seriais
    static String commandBuffer = "";
#endif

#ifdef SLAVE_MODE
    // Instâncias principais para Slave
    RelayCommandBox* relayBox = nullptr;
    ESPNowBridge* espNowBridge = nullptr;
    
    // Gerenciador de configurações
    SaveManager configManager;
    
    // Buffer para comandos seriais
    static String commandBuffer = "";
#endif

// Função de proteção global de memória SIMPLIFICADA
void globalMemoryProtection() {
    if(millis() - lastMemoryCheck < 10000) return; // A cada 10s
    
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxBlock = ESP.getMaxAllocHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    
    if(freeHeap < minHeapSeen) minHeapSeen = freeHeap;
    
    HydroSystemState currentState = stateManager.getCurrentState();
    
    // DEBUG SIMPLIFICADO POR ESTADO
    Serial.printf("🔄 [%s] Heap: %d bytes (%.1f%%) | Uptime: %ds\n", 
                  stateManager.getStateString().c_str(),
                  freeHeap, 
                  (freeHeap*100.0)/totalHeap,
                  (millis()-systemStartTime)/1000);
    
    // ALERTAS CRÍTICOS
    if(freeHeap < 15000) {
        Serial.println("🚨 ALERTA: Heap crítico! < 15KB");
    }
    
    lastMemoryCheck = millis();
}

// Sistema de reset preventivo EMERGENCIAL
void emergencyProtection() {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxBlock = ESP.getMaxAllocHeap();
    
    // RESET CRÍTICO: Heap muito baixo
    if(freeHeap < 8000) {
        Serial.println("💀 RESET EMERGENCIAL - Heap crítico: " + String(freeHeap) + " bytes");
        delay(1000);
        ESP.restart();
    }
    
    // RESET POR FRAGMENTAÇÃO EXTREMA
    uint32_t fragmentationPercent = freeHeap > 0 ? (100 - (maxBlock*100)/freeHeap) : 100;
    if(freeHeap > 15000 && fragmentationPercent > 85) {
        Serial.println("🧩 RESET EMERGENCIAL - Fragmentação extrema: " + String(fragmentationPercent) + "%");
        delay(1000);
        ESP.restart();
    }
}

// ===== FUNCIONES ESPECÍFICAS PARA MODO MASTER ESP-NOW =====
#ifdef MASTER_MODE

void setupMasterCallbacks() {
    if (!masterBridge) return;
    
    // Callback para status de relés remotos
    masterBridge->setRemoteRelayStatusCallback([](const uint8_t* mac, int relay, bool state, int remainingTime, const String& name) {
        Serial.printf("🔌 Status remoto: %s -> Relé %d = %s", 
                     ESPNowBridge::macToString(mac).c_str(), 
                     relay, state ? "ON" : "OFF");
        
        if (remainingTime > 0) {
            Serial.printf(" (Timer: %ds)", remainingTime);
        }
        Serial.println();
    });
    
    // Callback para descoberta de dispositivos
    masterBridge->setDeviceDiscoveryCallback([](const uint8_t* mac, const String& name, const String& type, bool operational) {
        Serial.println("📱 Novo slave descoberto:");
        Serial.println("   Nome: " + name);
        Serial.println("   Tipo: " + type);
        Serial.println("   MAC: " + ESPNowBridge::macToString(mac));
        Serial.println("   Status: " + String(operational ? "Operacional" : "Com problemas"));
        
        // Adicionar à lista de slaves
        addSlaveToList(mac, name, type, 8);  // Assumir 8 relés
    });
    
    // Callback para erros
    masterBridge->setErrorCallback([](const String& error) {
        Serial.println("❌ Erro ESP-NOW: " + error);
    });
}

void addSlaveToList(const uint8_t* macAddress, const String& deviceName, 
                   const String& deviceType, uint8_t numRelays) {
    // Verificar se já existe
    for (auto& slave : knownSlaves) {
        if (memcmp(slave.mac, macAddress, 6) == 0) {
            slave.online = true;
            slave.lastSeen = millis();
            slave.name = deviceName;
            slave.deviceType = deviceType;
            return;
        }
    }
    
    // Adicionar novo slave
    RemoteDevice newSlave;
    memcpy(newSlave.mac, macAddress, 6);
    newSlave.name = deviceName;
    newSlave.deviceType = deviceType;
    newSlave.online = true;
    newSlave.lastSeen = millis();
    newSlave.rssi = -50; // Valor padrão
    newSlave.numRelays = numRelays;
    newSlave.operational = true;
    
    knownSlaves.push_back(newSlave);
    Serial.println("✅ Novo slave adicionado: " + deviceName);
}

uint8_t* findSlaveMac(const String& slaveName) {
    for (auto& slave : knownSlaves) {
        if (slave.name == slaveName) {
            return slave.mac;
        }
    }
    return nullptr;
}

void printSlavesList() {
    Serial.println("\n📋 === SLAVES CONHECIDOS ===");
    if (knownSlaves.empty()) {
        Serial.println("   ⚠️ Nenhum slave encontrado");
        Serial.println("   💡 Use 'discover' para procurar slaves");
    } else {
        Serial.printf("   Total: %d slave(s)\n\n", knownSlaves.size());
        for (const auto& slave : knownSlaves) {
            String statusIcon = slave.online ? "🟢" : "🔴";
            Serial.printf("   %s %s\n", statusIcon.c_str(), slave.name.c_str());
            Serial.printf("      Tipo: %s\n", slave.deviceType.c_str());
            Serial.printf("      MAC: %s\n", ESPNowBridge::macToString(slave.mac).c_str());
            Serial.printf("      Status: %s\n", slave.online ? "Online" : "Offline");
            if (slave.online) {
                unsigned long timeSinceLastSeen = (millis() - slave.lastSeen) / 1000;
                Serial.printf("      Última comunicação: %lu segundos atrás\n", timeSinceLastSeen);
            }
            Serial.println();
        }
    }
    Serial.println("===========================");
}

void controlRelay(const String& slaveName, int relayNumber, const String& action, int duration) {
    if (!masterBridge) return;
    
    uint8_t* slaveMac = findSlaveMac(slaveName);
    if (!slaveMac) {
        Serial.println("❌ Slave não encontrado: " + slaveName);
        return;
    }
    
    // ===== CORREÇÃO #3: RETRY DE COMANDOS =====
    const int MAX_RETRIES = 3;
    const int COMMAND_RETRY_DELAY = 150;  // Delay entre tentativas (ms)
    bool success = false;
    
    for (int attempt = 1; attempt <= MAX_RETRIES && !success; attempt++) {
        success = masterBridge->sendRelayCommand(slaveMac, relayNumber, action, duration);
        
        if (success) {
            if (attempt > 1) {
                Serial.printf("✅ Comando enviado na tentativa %d/%d: %s -> Relé %d %s\n", 
                             attempt, MAX_RETRIES, slaveName.c_str(), relayNumber, action.c_str());
            } else {
                Serial.printf("✅ Comando enviado: %s -> Relé %d %s\n", 
                             slaveName.c_str(), relayNumber, action.c_str());
            }
        } else {
            if (attempt < MAX_RETRIES) {
                Serial.printf("⚠️ Tentativa %d/%d falhou - retentando em %dms...\n", 
                             attempt, MAX_RETRIES, COMMAND_RETRY_DELAY);
                delay(COMMAND_RETRY_DELAY);
            } else {
                Serial.printf("❌ Falha ao enviar comando após %d tentativas\n", MAX_RETRIES);
                Serial.println("💡 Verifique se o slave está online: list");
            }
        }
    }
}

void controlAllRelays(int relayNumber, const String& action, int duration) {
    if (!masterBridge) return;
    
    Serial.println("📤 Enviando comando para todos os slaves...");
    
    for (const auto& slave : knownSlaves) {
        if (slave.online) {
            Serial.println("📤 Enviando para: " + slave.name);
            masterBridge->sendRelayCommand(slave.mac, relayNumber, action, duration);
            delay(100); // Pequeno delay entre comandos
        }
    }
}

// ===== SISTEMA DE MONITORAMENTO AUTOMÁTICO =====

/**
 * @brief Monitora slaves automaticamente e toma decisões de reconexão
 */
void monitorSlavesAutomatically() {
    unsigned long currentTime = millis();
    
    // Verificar slaves a cada 30 segundos
    if (currentTime - lastSlaveCheck > 30000) {
        Serial.println("🔍 Verificação automática de slaves...");
        
        for (auto& slave : knownSlaves) {
            if (slave.online) {
                // Verificar se slave não respondeu há muito tempo
                unsigned long timeSinceLastSeen = currentTime - slave.lastSeen;
                
                if (timeSinceLastSeen > 120000) { // 2 minutos sem resposta
                    Serial.println("⚠️ Slave offline detectado: " + slave.name);
                    slave.online = false;
                    
                    // Tentar reconexão automática
                    attemptSlaveReconnection(&slave);
                }
            } else {
                // Tentar reconexão de slaves offline a cada 5 minutos
                if (currentTime - lastReconnectionAttempt > 300000) {
                    attemptSlaveReconnection(&slave);
                }
            }
        }
        
        lastSlaveCheck = currentTime;
    }
    
    // Verificar qualidade do sinal a cada 60 segundos
    if (currentTime - lastSignalCheck > 60000) {
        checkSignalQuality();
        lastSignalCheck = currentTime;
    }
}

/**
 * @brief Tenta reconectar com um slave offline
 */
void attemptSlaveReconnection(RemoteDevice* slave) {
    if (!masterBridge) return;
    
    Serial.println("🔄 Tentativa de reconexão com: " + slave->name);
    
    // 1. Enviar ping para testar conectividade
    bool pingSuccess = masterBridge->sendPing(slave->mac);
    
    if (pingSuccess) {
        Serial.println("✅ Ping enviado para: " + slave->name);
        
        // 2. Aguardar resposta por 5 segundos
        unsigned long pingStart = millis();
        bool receivedResponse = false;
        
        while (millis() - pingStart < 5000) {
            if (slave->online) {
                receivedResponse = true;
                break;
            }
            delay(100);
        }
        
        if (receivedResponse) {
            Serial.println("✅ Slave reconectado: " + slave->name);
            slave->online = true;
            slave->lastSeen = millis();
            failedPingCount = 0;
        } else {
            Serial.println("❌ Sem resposta de: " + slave->name);
            failedPingCount++;
            
            if (failedPingCount >= maxFailedPings) {
                Serial.println("🚨 Máximo de tentativas atingido para: " + slave->name);
                // Implementar estratégia de fallback
                implementFallbackStrategy(slave);
            }
        }
    } else {
        Serial.println("❌ Falha ao enviar ping para: " + slave->name);
    }
    
    lastReconnectionAttempt = millis();
}

/**
 * @brief Verifica qualidade do sinal e toma decisões
 */
void checkSignalQuality() {
    if (!masterBridge) return;
    
    Serial.println("📶 Verificando qualidade do sinal...");
    
    for (auto& slave : knownSlaves) {
        if (slave.online) {
            // Simular verificação de RSSI (em implementação real, usar esp_wifi_get_rssi)
            int rssi = -50; // Valor simulado
            
            if (rssi < -80) {
                Serial.println("⚠️ Sinal fraco detectado: " + slave.name + " (RSSI: " + String(rssi) + ")");
                
                // Decidir se deve tentar reconexão
                if (rssi < -90) {
                    Serial.println("🚨 Sinal crítico - tentando reconexão: " + slave.name);
                    attemptSlaveReconnection(&slave);
                }
            }
        }
    }
}

/**
 * @brief Implementa estratégia de fallback quando comunicação falha
 */
void implementFallbackStrategy(RemoteDevice* slave) {
    Serial.println("🔄 Implementando estratégia de fallback para: " + slave->name);
    
    // 1. Tentar descobrir o slave novamente
    Serial.println("   🔍 Tentando descobrir slave novamente...");
    discoverSlaves();
    
    // 2. Se não encontrou, tentar enviar credenciais WiFi
    if (!slave->online) {
        Serial.println("   📶 Tentando enviar credenciais WiFi...");
        if (WiFi.isConnected()) {
            String ssid = WiFi.SSID();
            // Aqui você poderia implementar envio automático de credenciais
            Serial.println("   💡 Credenciais WiFi disponíveis para envio");
        }
    }
    
    // 3. Resetar contador de falhas após estratégia
    failedPingCount = 0;
}

/**
 * @brief Sistema de decisão automática para comunicação
 */
void automaticCommunicationDecisions() {
    unsigned long currentTime = millis();
    
    // Decisão 1: Monitorar slaves automaticamente
    monitorSlavesAutomatically();
    
    // Decisão 2: Verificar integridade da comunicação a cada 2 minutos
    static unsigned long lastIntegrityCheck = 0;
    if (currentTime - lastIntegrityCheck > 120000) {
        checkCommunicationIntegrity();
        lastIntegrityCheck = currentTime;
    }
    
    // Decisão 3: Limpeza automática de peers offline a cada 10 minutos
    static unsigned long lastCleanup = 0;
    if (currentTime - lastCleanup > 600000) {
        cleanupOfflinePeers();
        lastCleanup = currentTime;
    }
}

/**
 * @brief Verifica integridade da comunicação
 */
void checkCommunicationIntegrity() {
    Serial.println("🔍 Verificação de integridade da comunicação...");
    
    int onlineSlaves = 0;
    int totalSlaves = knownSlaves.size();
    
    for (const auto& slave : knownSlaves) {
        if (slave.online) onlineSlaves++;
    }
    
    float integrityPercent = totalSlaves > 0 ? (onlineSlaves * 100.0) / totalSlaves : 0;
    
    Serial.println("📊 Integridade: " + String(integrityPercent, 1) + "% (" + 
                   String(onlineSlaves) + "/" + String(totalSlaves) + ")");
    
    // Decisão automática baseada na integridade
    if (integrityPercent < 50.0) {
        Serial.println("🚨 Integridade baixa - iniciando descoberta automática...");
        discoverSlaves();
    } else if (integrityPercent < 80.0) {
        Serial.println("⚠️ Integridade moderada - verificando slaves...");
        for (auto& slave : knownSlaves) {
            if (!slave.online) {
                attemptSlaveReconnection(&slave);
            }
        }
    }
}

/**
 * @brief Limpa peers offline antigos
 */
void cleanupOfflinePeers() {
    Serial.println("🧹 Limpeza automática de peers offline...");
    
    auto it = knownSlaves.begin();
    while (it != knownSlaves.end()) {
        if (!it->online && (millis() - it->lastSeen > 1800000)) { // 30 minutos offline
            Serial.println("🗑️ Removendo peer offline antigo: " + it->name);
            it = knownSlaves.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * @brief Sistema Automático de Discovery e Conexão (chamado no boot)
 * Aguarda slaves conectarem após receber credenciais WiFi
 */
void autoDiscoverAndConnect() {
    Serial.println("\n🔄 === SISTEMA AUTOMÁTICO DE DESCOBERTA ===");
    
    // ===== PASSO 1: ENVIAR CREDENCIAIS WiFi EM BROADCAST =====
    if (WiFi.isConnected()) {
        String currentSSID = WiFi.SSID();
        Serial.println("📶 WiFi Master conectado: " + currentSSID);
        Serial.println("📡 Enviando credenciais WiFi para slaves...");
        Serial.println("   💡 Use comando 'send_wifi " + currentSSID + " <senha>' para enviar com senha");
        Serial.println("   💡 Ou slaves podem conectar manualmente com 'wifi_connect'");
        
        // Enviar broadcast de descoberta (slaves verão que Master está online)
        if (masterBridge) {
            masterBridge->sendDiscoveryBroadcast();
            Serial.println("✅ Broadcast de descoberta enviado!");
        }
    } else {
        Serial.println("⚠️ Master não está conectado ao WiFi");
        Serial.println("   Slaves precisarão conectar manualmente");
    }
    
    Serial.println("\n⏳ Aguardando slaves conectarem ao WiFi...");
    Serial.println("   (Tempo estimado: 20-30 segundos)");
    
    // Aguardar 20 segundos para slaves conectarem
    unsigned long startTime = millis();
    int countdown = 20;
    
    while (millis() - startTime < 20000) {
        Serial.print("   ");
        Serial.print(countdown);
        Serial.println("s...");
        delay(1000);
        countdown--;
        
        // Alimentar watchdog durante espera
        if (masterBridge) watchdog.feed();
        esp_task_wdt_reset();
    }
    
    Serial.println("✅ Tempo de espera concluído!");
    Serial.println("🔍 Iniciando descoberta automática de slaves...\n");
    
    // Fazer discovery automático
    discoverSlaves();
    
    // Se encontrou slaves, fazer ping inicial
    if (!knownSlaves.empty()) {
        Serial.println("\n🏓 Testando conectividade com slaves encontrados...");
        for (const auto& slave : knownSlaves) {
            if (slave.online && masterBridge) {
                Serial.println("   → " + slave.name);
                masterBridge->sendPing(slave.mac);
                delay(200);
            }
        }
        Serial.println("✅ Sistema de comunicação ESP-NOW ativo!");
    } else {
        Serial.println("\n⚠️ Nenhum slave encontrado!");
        Serial.println("💡 Possíveis causas:");
        Serial.println("   - Slaves ainda não receberam credenciais WiFi");
        Serial.println("   - Slaves fora de alcance");
        Serial.println("   - Slaves não inicializados");
        Serial.println("\n🔄 Sistema continuará tentando automaticamente...");
    }
    
    Serial.println("==========================================\n");
}

/**
 * @brief Mantém conexão ESP-NOW ativa (loop contínuo)
 * Funciona igual ao WiFi.reconnect() - verifica e reconecta automaticamente
 */
void maintainESPNOWConnection() {
    static unsigned long lastConnectionCheck = 0;
    static unsigned long lastAutoDiscovery = 0;
    static bool firstDiscoveryDone = false;
    
    unsigned long currentTime = millis();
    
    // ===== VERIFICAÇÃO #1: Discovery periódico (a cada 5 minutos) =====
    if (currentTime - lastAutoDiscovery > 300000) { // 5 minutos
        Serial.println("\n🔍 Discovery automático periódico...");
        discoverSlaves();
        lastAutoDiscovery = currentTime;
    }
    
    // ===== VERIFICAÇÃO #2: Status de conexão (a cada 30 segundos) =====
    if (currentTime - lastConnectionCheck > 30000) { // 30 segundos
        int onlineCount = 0;
        int offlineCount = 0;
        
        for (auto& slave : knownSlaves) {
            if (slave.online) {
                onlineCount++;
            } else {
                offlineCount++;
            }
        }
        
        // Se tem slaves offline, tentar reconectar
        if (offlineCount > 0) {
            Serial.println("⚠️ " + String(offlineCount) + " slave(s) offline - iniciando reconexão...");
            reconnectESPNOWSlaves();
        }
        
        lastConnectionCheck = currentTime;
    }
    
    // ===== VERIFICAÇÃO #3: Se não tem slaves, fazer discovery =====
    if (knownSlaves.empty() && !firstDiscoveryDone) {
        Serial.println("🔍 Nenhum slave conhecido - fazendo discovery...");
        discoverSlaves();
        firstDiscoveryDone = true;
    }
}

/**
 * @brief Reconecta slaves offline (igual WiFi.reconnect())
 * Tenta descobrir e reconectar automaticamente
 */
void reconnectESPNOWSlaves() {
    Serial.println("\n🔄 === RECONEXÃO AUTOMÁTICA ESP-NOW ===");
    
    int reconnectedCount = 0;
    
    for (auto& slave : knownSlaves) {
        if (!slave.online) {
            Serial.println("🔌 Tentando reconectar: " + slave.name);
            
            // Tentar ping primeiro
            if (masterBridge) {
                masterBridge->sendPing(slave.mac);
                delay(500); // Aguardar resposta
                
                // Verificar se voltou online
                if (slave.online) {
                    Serial.println("   ✅ Reconectado!");
                    reconnectedCount++;
                } else {
                    Serial.println("   ⚠️ Sem resposta");
                }
            }
        }
    }
    
    // Se não conseguiu reconectar, fazer discovery completo
    if (reconnectedCount == 0) {
        Serial.println("🔍 Ping falhou - fazendo discovery completo...");
        discoverSlaves();
    } else {
        Serial.println("✅ " + String(reconnectedCount) + " slave(s) reconectado(s)!");
    }
    
    Serial.println("==========================================\n");
}

void discoverSlaves() {
    if (!masterBridge) return;
    
    Serial.println("🔍 Procurando slaves...");
    masterBridge->sendDiscoveryBroadcast();
    
    // ===== CORREÇÃO #5: TIMEOUT DE DESCOBERTA AUMENTADO =====
    // Aguardar respostas por 30 segundos (aumentado de 10s)
    const unsigned long DISCOVERY_TIMEOUT = 30000;  // 30 segundos
    unsigned long startTime = millis();
    int dotsCount = 0;
    
    Serial.print("⏳ Aguardando respostas");
    
    while (millis() - startTime < DISCOVERY_TIMEOUT) {
        masterBridge->update();
        delay(100);
        
        // Mostrar progresso visual
        if ((millis() - startTime) % 1000 == 0) {
            Serial.print(".");
            dotsCount++;
            if (dotsCount >= 30) {
                Serial.println();
                dotsCount = 0;
            }
        }
    }
    
    if (dotsCount > 0) Serial.println();
    
    Serial.println("📋 Slaves encontrados: " + String(knownSlaves.size()));
    printSlavesList();
}

void monitorSlaves() {
    if (!masterBridge) return;
    
    static unsigned long lastPing = 0;
    
    // ===== CORREÇÃO #4: TIMEOUTS OTIMIZADOS =====
    // Ping periódico para testar conectividade (reduzido de 30s para 15s)
    const unsigned long PING_INTERVAL = 15000;  // 15 segundos
    const unsigned long OFFLINE_TIMEOUT = 45000;  // 45 segundos (3 pings perdidos)
    const unsigned long CHECK_INTERVAL = 30000;  // 30 segundos
    
    if (millis() - lastPing > PING_INTERVAL) {
        for (const auto& slave : knownSlaves) {
            if (slave.online) {
                masterBridge->sendPing(slave.mac);
            }
        }
        lastPing = millis();
    }
    
    // Verificar slaves offline (verificação mais frequente)
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > CHECK_INTERVAL) {
        for (auto& slave : knownSlaves) {
            unsigned long timeSinceLastSeen = millis() - slave.lastSeen;
            
            if (timeSinceLastSeen > OFFLINE_TIMEOUT) {
                if (slave.online) {
                    slave.online = false;
                    Serial.println("⚠️ Slave offline: " + slave.name);
                    Serial.printf("   Última comunicação: %lu segundos atrás\n", timeSinceLastSeen / 1000);
                    Serial.println("💡 Use 'discover' para tentar reconectar");
                }
            }
        }
        lastCheck = millis();
    }
}

void handleMasterSerialCommands() {
    while (Serial.available()) {
        char c = Serial.read();
        
        // Se for Enter, processar comando
        if (c == '\n' || c == '\r') {
            if (commandBuffer.length() > 0) {
                String command = commandBuffer;
                commandBuffer = ""; // Limpar buffer
                command.trim();
                
                Serial.println(); // Nova linha
                
                if (command == "help") {
                    printMasterHelp();
                }
                else if (command == "discover") {
                    discoverSlaves();
                }
    else if (command == "list") {
        printSlavesList();
    }
    else if (command == "status") {
        printMasterStatus();
    }
    else if (command == "watchdog_status") {
        watchdog.printStatus();
    }
    else if (command == "watchdog_reset") {
        watchdog.reset();
    }
                else if (command.startsWith("ping ")) {
                    // Comando: ping <slave>
                    String slaveName = command.substring(5);
                    slaveName.trim();
                    uint8_t* slaveMac = findSlaveMac(slaveName);
                    if (slaveMac && masterBridge) {
                        Serial.println("🏓 Enviando ping para " + slaveName + "...");
                        masterBridge->sendPing(slaveMac);
                    } else {
                        Serial.println("❌ Slave não encontrado: " + slaveName);
                    }
                }
                else if (command == "ping") {
                    // Ping em todos os slaves
                    Serial.println("🏓 Enviando ping para todos os slaves...");
                    for (const auto& slave : knownSlaves) {
                        if (slave.online && masterBridge) {
                            Serial.println("   → " + slave.name);
                            masterBridge->sendPing(slave.mac);
                            delay(50);
                        }
                    }
                }
                else if (command.startsWith("relay ")) {
                    // Verificar se é comando especial relay off_all ou relay on_all
                    if (command == "relay off_all") {
                        Serial.println("🔄 Desligando todos os relés em todos os slaves...");
                        for (int relayNum = 0; relayNum < 8; relayNum++) {
                            controlAllRelays(relayNum, "off", 0);
                            delay(100);
                        }
                        Serial.println("✅ Comando relay off_all enviado para todos os slaves");
                    }
                    else if (command == "relay on_all") {
                        Serial.println("🔌 Ligando todos os relés permanentemente em todos os slaves...");
                        for (int relayNum = 0; relayNum < 8; relayNum++) {
                            controlAllRelays(relayNum, "on_forever", 0);
                            delay(100);
                        }
                        Serial.println("✅ Comando relay on_all enviado para todos os slaves");
                    }
                    else {
                        handleRelayCommand(command);
                    }
                }
                else if (command == "on_all") {
                    // Ligar todos os relés permanentemente em todos os slaves
                    Serial.println("🔌 Ligando todos os relés permanentemente em todos os slaves...");
                    for (int relayNum = 0; relayNum < 8; relayNum++) {
                        controlAllRelays(relayNum, "on_forever", 0);
                        delay(100);
                    }
                    Serial.println("✅ Comando on_all enviado para todos os slaves");
                }
                else if (command == "off_all") {
                    // Desligar todos os relés em todos os slaves
                    Serial.println("🔄 Desligando todos os relés em todos os slaves...");
                    for (int relayNum = 0; relayNum < 8; relayNum++) {
                        controlAllRelays(relayNum, "off", 0);
                        delay(100); // Pequeno delay entre comandos
                    }
                    Serial.println("✅ Comando off_all enviado para todos os slaves");
                }
                else if (command == "handshake") {
                    // Handshake bidirecional com todos os slaves conhecidos
                    Serial.println("🤝 Iniciando handshake bidirecional com todos os slaves...");
                    for (const auto& slave : knownSlaves) {
                        if (slave.online && masterBridge) {
                            Serial.println("📤 Enviando handshake para: " + slave.name);
                            masterBridge->initiateHandshake(slave.mac);
                            delay(200);
                        }
                    }
                    Serial.println("✅ Handshakes enviados para todos os slaves online");
                }
                else if (command.startsWith("handshake ")) {
                    // Handshake com slave específico
                    String slaveName = command.substring(10);
                    slaveName.trim();
                    uint8_t* slaveMac = findSlaveMac(slaveName);
                    if (slaveMac && masterBridge) {
                        Serial.println("🤝 Iniciando handshake com " + slaveName + "...");
                        masterBridge->initiateHandshake(slaveMac);
                    } else {
                        Serial.println("❌ Slave não encontrado: " + slaveName);
                    }
                }
                else if (command == "connectivity_check") {
                    // Verificação de conectividade com todos os slaves
                    Serial.println("🔍 Verificando conectividade de todos os slaves...");
                    for (const auto& slave : knownSlaves) {
                        if (slave.online && masterBridge) {
                            Serial.println("📤 Solicitando verificação de: " + slave.name);
                            masterBridge->requestConnectivityCheck(slave.mac);
                            delay(200);
                        }
                    }
                    Serial.println("✅ Solicitações de verificação enviadas");
                }
                else if (command.startsWith("connectivity_check ")) {
                    // Verificação com slave específico
                    String slaveName = command.substring(19);
                    slaveName.trim();
                    uint8_t* slaveMac = findSlaveMac(slaveName);
                    if (slaveMac && masterBridge) {
                        Serial.println("🔍 Verificando conectividade de " + slaveName + "...");
                        masterBridge->requestConnectivityCheck(slaveMac);
                    } else {
                        Serial.println("❌ Slave não encontrado: " + slaveName);
                    }
                }
                else if (command == "auto_validation") {
                    // Sistema automático de validação bidirecional
                    Serial.println("🔄 Iniciando sistema automático de validação bidirecional...");
                    Serial.println("📋 Sequência: Handshake → Verificação de Conectividade → Relatório");
                    
                    for (const auto& slave : knownSlaves) {
                        if (slave.online && masterBridge) {
                            Serial.println("\n🎯 Processando: " + slave.name);
                            
                            // 1. Handshake
                            Serial.println("   🤝 Enviando handshake...");
                            masterBridge->initiateHandshake(slave.mac);
                            delay(500);
                            
                            // 2. Verificação de conectividade
                            Serial.println("   🔍 Solicitando verificação...");
                            masterBridge->requestConnectivityCheck(slave.mac);
                            delay(500);
                            
                            // 3. Ping tradicional
                            Serial.println("   🏓 Enviando ping...");
                            masterBridge->sendPing(slave.mac);
                            delay(500);
                        }
                    }
                    Serial.println("\n✅ Sistema automático de validação concluído!");
                    Serial.println("📊 Aguarde os relatórios de conectividade...");
                }
                // ===== COMANDOS DE WiFi CREDENTIALS REMOVIDOS =====
                // MOTIVO: Slaves usam ESP-NOW puro, não precisam de WiFi
                // Discovery automático via ESP-NOW é suficiente
                
                else if (command == "task_status") {
                    // Status da task dedicada ESP-NOW
                    if (espNowTask) {
                        espNowTask->printStatus();
                    } else {
                        Serial.println("❌ ESP-NOW Task não inicializada");
                    }
                }
                else if (command == "task_discover") {
                    // Discovery usando task dedicada
                    if (espNowTask) {
                        Serial.println("🔍 Enviando discovery via task dedicada...");
                        espNowTask->sendDiscovery();
                    } else {
                        Serial.println("❌ ESP-NOW Task não inicializada");
                    }
                }
                // task_wifi_broadcast REMOVIDO - não é mais necessário
                else if (command == "bridge_stats") {
                    if (relayBridge) {
                        relayBridge->printStats();
                    } else {
                        Serial.println("❌ RelayBridge não inicializado");
                    }
                }
                else if (command == "bridge_enable") {
                    if (relayBridge) {
                        relayBridge->setAutoProcessing(true);
                        Serial.println("✅ RelayBridge habilitado - Polling automático ativo");
                    } else {
                        Serial.println("❌ RelayBridge não inicializado");
                    }
                }
                else if (command == "bridge_disable") {
                    if (relayBridge) {
                        relayBridge->setAutoProcessing(false);
                        Serial.println("⚠️ RelayBridge deshabilitado - Polling pausado");
                    } else {
                        Serial.println("❌ RelayBridge não inicializado");
                    }
                }
                else {
                    Serial.println("❓ Comando desconhecido: " + command);
                    Serial.println("💡 Digite 'help' para ajuda");
                }
            }
        } else {
            // Acumular caractere no buffer
            commandBuffer += c;
            Serial.print(c); // Echo
        }
    }
}

void handleRelayCommand(const String& command) {
    // Formato: relay <slave> <número> <ação> [duração]
    // Exemplo: relay ESP-NOW-SLAVE 0 on 30
    
    int firstSpace = command.indexOf(' ', 6);
    int secondSpace = command.indexOf(' ', firstSpace + 1);
    int thirdSpace = command.indexOf(' ', secondSpace + 1);
    
    if (firstSpace > 0 && secondSpace > 0) {
        String slaveName = command.substring(6, firstSpace);
        int relayNumber = command.substring(firstSpace + 1, secondSpace).toInt();
        String action;
        int duration = 0;
        
        if (thirdSpace > 0) {
            // Tem duração: relay ESP-NOW-SLAVE 0 on 30
            action = command.substring(secondSpace + 1, thirdSpace);
            duration = command.substring(thirdSpace + 1).toInt();
        } else {
            // Sem duração: relay ESP-NOW-SLAVE 0 on
            action = command.substring(secondSpace + 1);
            action.trim();
        }
        
        controlRelay(slaveName, relayNumber, action, duration);
    } else {
        Serial.println("❌ Formato: relay <slave> <número> <ação> [duração]");
        Serial.println("💡 Exemplo: relay ESP-NOW-SLAVE 0 on 30");
    }
}

void printMasterHelp() {
    Serial.println("\n🎮 === COMANDOS MASTER ESP-NOW ===");
    Serial.println("🔍 DESCOBERTA E GERENCIAMENTO:");
    Serial.println("   discover           - Procurar slaves");
    Serial.println("   list               - Listar slaves conhecidos");
    Serial.println("   status             - Status do sistema");
    Serial.println("   ping               - Testar conectividade com todos os slaves");
    Serial.println("   ping <slave>       - Testar conectividade com slave específico");
    Serial.println();
    Serial.println("🛡️ WATCHDOG:");
    Serial.println("   watchdog_status    - Status do SafetyWatchdog");
    Serial.println("   watchdog_reset     - Resetar watchdog manualmente");
    Serial.println();
    Serial.println("🤝 VALIDAÇÃO BIDIRECIONAL:");
    Serial.println("   handshake          - Handshake bidirecional com todos os slaves");
    Serial.println("   handshake <slave>  - Handshake com slave específico");
    Serial.println("   connectivity_check - Verificar conectividade de todos os slaves");
    Serial.println("   connectivity_check <slave> - Verificar conectividade específica");
    Serial.println("   auto_validation    - Sistema automático completo de validação");
    Serial.println();
    Serial.println("🔌 CONTROLE DE RELÉS:");
    Serial.println("   relay <slave> <n> <ação> [duração]");
    Serial.println("   Exemplo: relay ESP-NOW-SLAVE 0 on 30");
    Serial.println();
    Serial.println("📢 CONTROLE EM LOTE:");
    Serial.println("   relay off_all          - Desligar todos os relés em todos os slaves");
    Serial.println("   relay on_all           - Ligar todos os relés permanentemente em todos os slaves");
    Serial.println("   off_all                - Desligar todos os relés em todos os slaves");
    Serial.println("   on_all                 - Ligar todos os relés permanentemente em todos os slaves");
    Serial.println();
    Serial.println("🌉 RELAY BRIDGE (Supabase ↔ ESP-NOW):");
    Serial.println("   bridge_stats           - Estatísticas do RelayBridge");
    Serial.println("   bridge_enable          - Habilitar polling automático");
    Serial.println("   bridge_disable         - Desabilitar polling automático");
    Serial.println();
    Serial.println("📶 WIFI BROADCAST (para TODOS os Slaves):");
    Serial.println("   send_wifi <ssid> <password>  - Enviar WiFi em broadcast");
    Serial.println("   send_wifi_auto               - Enviar WiFi atual em broadcast");
    Serial.println("   send_wifi_saved              - Enviar credenciais salvas automaticamente");
    Serial.println("   test_wifi_broadcast          - Testar envio de credenciais");
    Serial.println();
    Serial.println("🎯 AÇÕES DISPONÍVEIS:");
    Serial.println("   on [duração]    - Ligar relé");
    Serial.println("   on_forever     - Ligar relé permanentemente");
    Serial.println("   off            - Desligar relé");
    Serial.println("   toggle         - Alternar relé");
    Serial.println("   status         - Consultar status");
    Serial.println();
    Serial.println("📝 EXEMPLOS:");
    Serial.println("   discover                       - Procura slaves na rede");
    Serial.println("   handshake ESP-NOW-SLAVE        - Handshake com slave específico");
    Serial.println("   auto_validation                - Validação automática completa");
    Serial.println("   connectivity_check             - Verificar todos os slaves");
    Serial.println("   ping ESP-NOW-SLAVE             - Testa conexão com slave");
    Serial.println("   relay ESP-NOW-SLAVE 0 on 60    - Liga relé 0 por 1 minuto");
    Serial.println("   relay ESP-NOW-SLAVE 0 on       - Liga relé 0 permanentemente");
    Serial.println("   relay ESP-NOW-SLAVE 1 off      - Desliga relé 1");
    Serial.println("   relay off_all                  - Desliga todos os relés em todos os slaves");
    Serial.println("   relay on_all                   - Liga todos os relés em todos os slaves");
    Serial.println("   off_all                        - Desliga todos os relés");
    Serial.println("================================\n");
}

void printMasterStatus() {
    Serial.println("\n📊 === STATUS DO SISTEMA MASTER ===");
    Serial.println("🎯 Master Controller");
    if (masterBridge) {
        Serial.println("   MAC: " + masterBridge->getLocalMacString());
        Serial.println("   Canal: 1");
        Serial.println("   Dispositivos online: " + String(masterBridge->getOnlineDeviceCount()));
    } else {
        Serial.println("   ❌ Master não inicializado");
    }
    Serial.println();
    
    int onlineSlaves = 0;
    for (const auto& slave : knownSlaves) {
        if (slave.online) onlineSlaves++;
    }
    
    Serial.printf("👥 Slaves: %d total (%d online, %d offline)\n", 
                  knownSlaves.size(), onlineSlaves, knownSlaves.size() - onlineSlaves);
    Serial.println();
    
    if (masterBridge) {
        Serial.println("📊 Status ESP-NOW:");
        masterBridge->printStatus();
    }
    
    // Diagnóstico ESP-NOW
    Serial.println("\n🔍 DIAGNÓSTICO ESP-NOW:");
    Serial.println("   ESP-NOW inicializado: " + String(esp_now_is_peer_exist ? "✅ Sim" : "❌ Não"));
    
    // Verificar peer de broadcast
    uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    bool broadcastRegistered = esp_now_is_peer_exist(broadcastMac);
    Serial.println("   Peer broadcast registrado: " + String(broadcastRegistered ? "✅ Sim" : "❌ Não"));
    
    // Canal WiFi atual
    wifi_second_chan_t secondChan;
    uint8_t currentChannel;
    esp_wifi_get_channel(&currentChannel, &secondChan);
    Serial.println("   Canal WiFi atual: " + String(currentChannel));
    
    // Status WiFi
    Serial.println("   WiFi conectado: " + String(WiFi.isConnected() ? "✅ Sim" : "❌ Não"));
    if (WiFi.isConnected()) {
        Serial.println("   SSID: " + WiFi.SSID());
        Serial.println("   IP: " + WiFi.localIP().toString());
    }
    
    Serial.println();
    Serial.printf("⏱️ Uptime: %lu segundos\n", millis() / 1000);
    Serial.printf("💾 Heap livre: %d bytes\n", ESP.getFreeHeap());
    Serial.println("===========================");
}

/**
 * @brief Registra peer de broadcast para ESP-NOW
 */
void registerBroadcastPeer() {
    uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    // Verificar se já está registrado
    if (esp_now_is_peer_exist(broadcastMac)) {
        Serial.println("✅ Peer de broadcast já registrado");
        return;
    }
    
    // Obter canal atual do WiFi
    wifi_second_chan_t secondChan;
    uint8_t currentChannel;
    esp_wifi_get_channel(&currentChannel, &secondChan);
    
    // Registrar peer de broadcast
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastMac, 6);
    peerInfo.channel = currentChannel;  // Usar canal atual do WiFi
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    
    Serial.println("📡 Registrando peer de broadcast no canal " + String(currentChannel) + "...");
    
    esp_err_t result = esp_now_add_peer(&peerInfo);
    if (result == ESP_OK) {
        Serial.println("✅ Peer de broadcast registrado com sucesso");
        Serial.println("   Canal: " + String(currentChannel));
        Serial.println("   MAC: FF:FF:FF:FF:FF:FF");
    } else {
        Serial.println("⚠️ Erro ao registrar peer de broadcast: " + String(result));
        Serial.println("🔍 Códigos de erro:");
        Serial.println("   ESP_ERR_ESPNOW_NOT_INIT = " + String(ESP_ERR_ESPNOW_NOT_INIT));
        Serial.println("   ESP_ERR_ESPNOW_ARG = " + String(ESP_ERR_ESPNOW_ARG));
        Serial.println("   ESP_ERR_ESPNOW_FULL = " + String(ESP_ERR_ESPNOW_FULL));
        Serial.println("   ESP_ERR_ESPNOW_NO_MEM = " + String(ESP_ERR_ESPNOW_NO_MEM));
        Serial.println("   ESP_ERR_ESPNOW_EXIST = " + String(ESP_ERR_ESPNOW_EXIST));
    }
}

/**
 * @brief Envia credenciais WiFi em broadcast para todos os slaves
 * @param ssid SSID da rede WiFi
 * @param password Senha da rede WiFi
 * @return true se enviado com sucesso
 */
/**
 * @brief Envia credenciais WiFi salvas automaticamente para todos os slaves
 * @return true se enviado com sucesso
 */
// ===== MÉTODO DESABILITADO - SLAVES NÃO PRECISAM DE CREDENCIAIS WiFi =====
// Slaves conectam via ESP-NOW puro, sem WiFi
/*
bool sendSavedWiFiCredentialsBroadcast() {
    Serial.println("\n🤖 === ENVIO AUTOMÁTICO DE CREDENCIAIS WiFi ===");
    
    // Verificar se Master Bridge está inicializado
    if (!masterBridge) {
        Serial.println("❌ Master Bridge não inicializado!");
        return false;
    }
    
    // Obter credenciais WiFi atuais
    String currentSSID = WiFi.SSID();
    String currentPassword = "";
    
    // Tentar obter senha do Preferences
    Preferences prefs;
    if (prefs.begin("wifi_creds", true)) {
        currentPassword = prefs.getString("password", "");
        prefs.end();
        
        if (currentPassword.length() > 0) {
            Serial.println("📂 Credenciais encontradas no Preferences:");
            Serial.println("   SSID: " + currentSSID);
            Serial.print("   Senha: ");
            for (size_t i = 0; i < currentPassword.length(); i++) Serial.print("*");
            Serial.println();
        } else {
            Serial.println("⚠️ Senha não encontrada no Preferences");
            Serial.println("💡 Use 'send_wifi_auto' para configurar manualmente");
            return false;
        }
    } else {
        Serial.println("❌ Erro ao acessar Preferences");
        return false;
    }
    
    // Enviar credenciais usando função existente
    bool success = sendWiFiCredentialsBroadcast(currentSSID, currentPassword);
    
    if (success) {
        Serial.println("✅ Credenciais automáticas enviadas com sucesso!");
        Serial.println("📡 Todos os slaves no alcance receberão");
        Serial.println("⏳ Aguarde os slaves conectarem (10-20 segundos)...");
        Serial.println("===============================================\n");
        return true;
    } else {
        Serial.println("❌ Falha ao enviar credenciais automáticas");
        return false;
    }
}
*/

// ===== MÉTODO DESABILITADO - SLAVES NÃO PRECISAM DE CREDENCIAIS WiFi =====
// Slaves conectam via ESP-NOW puro, sem WiFi
/*
bool sendWiFiCredentialsBroadcast(const String& ssid, const String& password) {
    Serial.println("\n📢 === ENVIANDO CREDENCIAIS WiFi EM BROADCAST ===");
    
    // Validar SSID
    if (ssid.length() == 0 || ssid.length() > 32) {
        Serial.println("❌ SSID inválido (deve ter 1-32 caracteres)");
        return false;
    }
    
    // Validar senha
    if (password.length() > 63) {
        Serial.println("❌ Senha inválida (máximo 63 caracteres)");
        return false;
    }
    
    // Verificar se Master Bridge está inicializado
    if (!masterBridge) {
        Serial.println("❌ Master Bridge não inicializado!");
        return false;
    }
    
    // Usar ESPNowController para enviar credenciais
    ESPNowController* controller = masterBridge->getESPNowController();
    if (!controller) {
        Serial.println("❌ ESPNowController não disponível!");
        return false;
    }
    
    // Registrar peer de broadcast (se necessário)
    registerBroadcastPeer();
    
    // Debug
    Serial.println("📤 Enviando credenciais:");
    Serial.println("   SSID: " + ssid);
    Serial.print("   Senha: ");
    for (size_t i = 0; i < password.length(); i++) Serial.print("*");
    Serial.println();
    Serial.println("   Alcance: TODOS os slaves");
    Serial.println("📡 Enviando via ESP-NOW...");
    
    // Enviar credenciais (o método cria a estrutura e mensagem internamente)
    bool success = controller->sendWiFiCredentialsBroadcast(ssid, password);
    
    if (success) {
        Serial.println("✅ Credenciais enviadas em broadcast com sucesso!");
        Serial.println("📡 Todos os slaves no alcance receberão");
        Serial.println("⏳ Aguarde os slaves conectarem (10-20 segundos)...");
        Serial.println("💡 Use 'discover' após 20 segundos para verificar");
        Serial.println("================================================\n");
        return true;
    } else {
        Serial.println("❌ Falha ao enviar credenciais via ESP-NOW");
        return false;
    }
}
*/

#endif // MASTER_MODE

// ===== FUNCIONES ESPECÍFICAS PARA MODO SLAVE ESP-NOW =====
#ifdef SLAVE_MODE

void handleSlaveSerialCommands() {
    while (Serial.available()) {
        char c = Serial.read();
        
        // Se for Enter, processar comando
        if (c == '\n' || c == '\r') {
            if (commandBuffer.length() > 0) {
                String command = commandBuffer;
                commandBuffer = ""; // Limpar buffer
                command.trim();
                
                Serial.println(); // Nova linha
        
                if (command == "help") {
                    printSlaveHelp();
                }
                else if (command == "status") {
                    if (relayBox) {
                        relayBox->printStatus();
                    }
                }
                else if (command.startsWith("relay ")) {
                    // Verificar se é comando especial relay off_all ou relay on_all
                    if (command == "relay off_all") {
                        if (relayBox) {
                            relayBox->turnOffAllRelays();
                            Serial.println("🔄 Todos os relés desligados");
                        }
                    }
                    else if (command == "relay on_all") {
                        if (relayBox) {
                            Serial.println("🔌 Ligando todos os relés permanentemente...");
                            for (int i = 0; i < 8; i++) {
                                relayBox->processCommand(i, "on_forever", 0);
                            }
                            Serial.println("✅ Todos os relés ligados permanentemente");
                        }
                    }
                    else {
                        // Comando: relay <número> <ação> [duração]
                        int firstSpace = command.indexOf(' ', 6);
                        
                        if (firstSpace > 0) {
                            int relayNumber = command.substring(6, firstSpace).toInt();
                            int secondSpace = command.indexOf(' ', firstSpace + 1);
                            String action;
                            int duration = 0;
                            
                            if (secondSpace > 0) {
                                // Tem duração: relay 1 on 30
                                action = command.substring(firstSpace + 1, secondSpace);
                                duration = command.substring(secondSpace + 1).toInt();
                            } else {
                                // Sem duração: relay 1 on ou relay 1 on_forever
                                action = command.substring(firstSpace + 1);
                                action.trim();
                            }
                            
                            if (relayNumber >= 0 && relayNumber < 8 && relayBox) {
                                bool success = relayBox->processCommand(relayNumber, action, duration);
                                if (success) {
                                    Serial.println("✅ Comando executado: Relé " + String(relayNumber) + " -> " + action);
                                } else {
                                    Serial.println("❌ Falha ao executar comando");
                                }
                            } else {
                                Serial.println("❌ Número de relé inválido (0-7)");
                            }
                        } else {
                            Serial.println("❌ Formato: relay <número> <ação> [duração] ou relay off_all / relay on_all");
                        }
                    }
                }
                else if (command == "off_all") {
                    if (relayBox) {
                        relayBox->turnOffAllRelays();
                        Serial.println("🔄 Todos os relés desligados");
                    }
                }
                else if (command == "on_all") {
                    // Ligar todos os relés permanentemente
                    if (relayBox) {
                        Serial.println("🔌 Ligando todos os relés permanentemente...");
                        for (int i = 0; i < 8; i++) {
                            relayBox->processCommand(i, "on_forever", 0);
                        }
                        Serial.println("✅ Todos os relés ligados permanentemente");
                    }
                }
                else {
                    Serial.println("❓ Comando desconhecido: " + command);
                    Serial.println("💡 Digite 'help' para ver comandos disponíveis");
                }
            }
        } else {
            // Acumular caractere no buffer
            commandBuffer += c;
            Serial.print(c); // Echo
        }
    }
}

void printSlaveHelp() { 
    Serial.println("\n📋 === COMANDOS SLAVE ESP-NOW ===");
    Serial.println("🏗️ SISTEMA:");
    Serial.println("   help           - Esta ajuda");
    Serial.println("   status         - Status de todos os relés");
    Serial.println();
    Serial.println("🔌 CONTROLE DE RELÉS (0-7):");
    Serial.println("   relay <n> on [tempo]    - Ligar relé");
    Serial.println("   relay <n> on_forever    - Ligar relé permanentemente");
    Serial.println("   relay <n> off           - Desligar relé");
    Serial.println("   relay <n> toggle        - Alternar relé");
    Serial.println("   relay off_all           - Desligar todos os relés");
    Serial.println("   relay on_all            - Ligar todos os relés permanentemente");
    Serial.println("   off_all                 - Desligar todos");
    Serial.println("   on_all                  - Ligar todos os relés permanentemente");
    Serial.println();
    Serial.println("📝 EXEMPLOS:");
    Serial.println("   relay 0 on 30          - Liga relé 0 por 30s");
    Serial.println("   relay 0 on             - Liga relé 0 permanentemente");
    Serial.println("   relay 0 on_forever     - Liga relé 0 permanentemente");
    Serial.println("   relay 1 off            - Desliga relé 1");
    Serial.println("   relay 2 toggle         - Alterna relé 2");
    Serial.println("   relay off_all          - Desliga todos os relés");
    Serial.println("   relay on_all           - Liga todos os relés permanentemente");
    Serial.println("   off_all                - Desliga todos");
    Serial.println("   on_all                 - Liga todos os relés permanentemente");
    Serial.println();
    Serial.println("🤖 MODO SLAVE:");
    Serial.println("   - Recebe comandos via ESP-NOW do Master");
    Serial.println("   - Suporta 8 relés via PCF8574");
    Serial.println("   - Interface serial para teste local");
    Serial.println("===============================\n");
}

#endif // SLAVE_MODE

// Handler de comandos seriais globais SIMPLIFICADO
void handleGlobalSerialCommands() {
    if (!Serial.available()) return;
    
    String command = Serial.readString();
    command.trim();
    command.toLowerCase();
    
    if (command == "help") {
        Serial.println("\n📋 === COMANDOS DISPONÍVEIS ===");
        Serial.println("🏗️ CONTROLE DE ESTADOS:");
        Serial.println("   wifi      - WiFi Config Mode");
        Serial.println("   hydro     - Hydro Active Mode");
        Serial.println("   admin     - Admin Panel Mode");
        Serial.println("   state     - Ver estado atual");
        Serial.println("\n🔧 SISTEMA:");
        Serial.println("   status    - Status do sistema");
        Serial.println("   reset     - Reiniciar ESP32");
        
#ifdef MASTER_MODE
        Serial.println("\n🎯 MODO MASTER ESP-NOW:");
        Serial.println("   discover           - Procurar slaves");
        Serial.println("   list               - Listar slaves conhecidos");
        Serial.println("   ping               - Testar conectividade com todos os slaves");
        Serial.println("   ping <slave>       - Testar conectividade com slave específico");
        Serial.println("   relay <slave> <n> <ação> [duração] - Controlar relé específico");
        Serial.println("   relay on_all       - Ligar todos os relés permanentemente");
        Serial.println("   relay off_all      - Desligar todos os relés");
        Serial.println("   on_all             - Ligar todos os relés permanentemente");
        Serial.println("   off_all            - Desligar todos os relés");
        Serial.println("\n📶 WIFI BROADCAST (para TODOS os Slaves):");
        Serial.println("   send_wifi <ssid> <password>  - Enviar WiFi em broadcast");
        Serial.println("   send_wifi_auto               - Enviar WiFi atual em broadcast");
        Serial.println("   test_wifi_broadcast          - Testar envio de credenciais");
        Serial.println("   debug_creds                  - Debug credenciais salvas");
        Serial.println("\n🚀 TASK DEDICADA ESP-NOW (Nova arquitetura):");
        Serial.println("   task_status                  - Status da task dedicada");
        Serial.println("   task_discover                - Discovery via task dedicada");
        Serial.println("   task_wifi_broadcast          - Enviar WiFi via task dedicada");
#endif

#ifdef SLAVE_MODE
        Serial.println("\n🤖 MODO SLAVE ESP-NOW:");
        Serial.println("   relay <n> on [tempo]    - Ligar relé");
        Serial.println("   relay <n> on_forever    - Ligar relé permanentemente");
        Serial.println("   relay <n> off           - Desligar relé");
        Serial.println("   relay <n> toggle        - Alternar relé");
        Serial.println("   relay on_all            - Ligar todos os relés permanentemente");
        Serial.println("   relay off_all           - Desligar todos os relés");
        Serial.println("   on_all                  - Ligar todos os relés permanentemente");
        Serial.println("   off_all                 - Desligar todos");
#endif
        
        Serial.println("\n📡 ESP-NOW (LEGACY):");
        Serial.println("   espnow_status    - Status completo da rede ESP-NOW");
        Serial.println("   force_discovery  - Descoberta e reconexão automática");
        Serial.println("   remote <MAC> <relay> <action> [duration] - Comando remoto");
        Serial.println("   broadcast        - Broadcast de sensores");
        Serial.println("\n🤖 AUTOMAÇÃO:");
        Serial.println("   auto_discovery   - Controle de automação");
        Serial.println("   auto_reconnect   - Reconexão automática");
        Serial.println("   discovery_stats  - Estatísticas de descoberta");
        Serial.println("   connection_health - Saúde da conexão");
        Serial.println("   help             - Esta ajuda");
        Serial.println("===============================\n");
    }
    else if (command == "wifi") {
        stateManager.switchToWiFiConfig();
    }
    else if (command == "hydro") {
        stateManager.switchToHydroActive();
    }
    else if (command == "admin") {
        stateManager.switchToAdminPanel();
    }
    else if (command == "state") {
        Serial.println("🏗️ Estado: " + stateManager.getStateString() + 
                      " | Uptime: " + String(stateManager.getStateUptime()/1000) + "s");
    }
    else if (command == "status") {
        Serial.println("\n📊 === STATUS COMPLETO ===");
        Serial.println("🏗️ Estado: " + stateManager.getStateString());
        Serial.println("⏰ Uptime Estado: " + String(stateManager.getStateUptime()/1000) + "s");
        Serial.println("⏰ Uptime Total: " + String((millis()-systemStartTime)/1000) + "s");
        Serial.println("🌐 WiFi: " + (WiFi.isConnected() ? "✅ " + WiFi.localIP().toString() : "❌ Desconectado"));
        Serial.println("💾 Heap: " + String(ESP.getFreeHeap()) + " / " + String(ESP.getHeapSize()) + " bytes");
        Serial.println("⬇️ Mínimo: " + String(minHeapSeen) + " bytes");
        Serial.println("============================\n");
    }
    else if (command == "reset") {
        Serial.println("🔄 REINICIANDO ESP32...");
        delay(1000);
        ESP.restart();
    }
    // ===== COMANDOS ESP-NOW MASTER =====
#ifdef MASTER_MODE
    else if (command == "discover") {
        discoverSlaves();
    }
    else if (command == "list") {
        printSlavesList();
    }
    else if (command == "ping") {
        // Ping em todos os slaves
        Serial.println("🏓 Enviando ping para todos os slaves...");
        for (const auto& slave : knownSlaves) {
            if (slave.online && masterBridge) {
                Serial.println("   → " + slave.name);
                masterBridge->sendPing(slave.mac);
                delay(50);
            }
        }
    }
    else if (command.startsWith("ping ")) {
        // Comando: ping <slave>
        String slaveName = command.substring(5);
        slaveName.trim();
        uint8_t* slaveMac = findSlaveMac(slaveName);
        if (slaveMac && masterBridge) {
            Serial.println("🏓 Enviando ping para " + slaveName + "...");
            masterBridge->sendPing(slaveMac);
        } else {
            Serial.println("❌ Slave não encontrado: " + slaveName);
        }
    }
    else if (command.startsWith("relay ")) {
        handleRelayCommand(command);
    }
    else if (command == "relay on_all") {
        // Comando especial: ligar todos os relés permanentemente
        Serial.println("🔌 Ligando todos os relés permanentemente em todos os slaves...");
        for (int relayNum = 0; relayNum < 8; relayNum++) {
            controlAllRelays(relayNum, "on_forever", 0);
            delay(100);
        }
        Serial.println("✅ Comando relay on_all enviado para todos os slaves");
    }
    else if (command == "relay off_all") {
        // Comando especial: desligar todos os relés
        Serial.println("🔄 Desligando todos os relés em todos os slaves...");
        for (int relayNum = 0; relayNum < 8; relayNum++) {
            controlAllRelays(relayNum, "off", 0);
            delay(100);
        }
        Serial.println("✅ Comando relay off_all enviado para todos os slaves");
    }
    else if (command == "on_all") {
        // Ligar todos os relés permanentemente em todos os slaves
        Serial.println("🔌 Ligando todos os relés permanentemente em todos os slaves...");
        for (int relayNum = 0; relayNum < 8; relayNum++) {
            controlAllRelays(relayNum, "on_forever", 0);
            delay(100);
        }
        Serial.println("✅ Comando on_all enviado para todos os slaves");
    }
    else if (command == "off_all") {
        // Desligar todos os relés em todos os slaves
        Serial.println("🔄 Desligando todos os relés em todos os slaves...");
        for (int relayNum = 0; relayNum < 8; relayNum++) {
            controlAllRelays(relayNum, "off", 0);
            delay(100);
        }
        Serial.println("✅ Comando off_all enviado para todos os slaves");
    }
    // ===== COMANDO DESABILITADO - SLAVES NÃO PRECISAM WiFi =====
    /*
    else if (command.startsWith("send_wifi ")) {
        // Formato: send_wifi <ssid> <password>
        int space = command.indexOf(' ', 10);
        
        if (space > 0) {
            String ssid = command.substring(10, space);
            String password = command.substring(space + 1);
            
            sendWiFiCredentialsBroadcast(ssid, password);
        } else {
            Serial.println("❌ Formato: send_wifi <ssid> <password>");
            Serial.println("💡 Exemplo: send_wifi MinhaRede senha123");
        }
    }
    */
    // ===== COMANDO DESABILITADO - SLAVES NÃO PRECISAM WiFi =====
    /*
    else if (command == "send_wifi_auto") {
        String ssid = WiFi.SSID();
        
        if (ssid.length() == 0) {
            Serial.println("❌ WiFi não conectado");
            Serial.println("💡 Use 'send_wifi <ssid> <password>' para enviar manualmente");
            return;
        }
        
        Serial.println("📢 Enviando credenciais WiFi atual em BROADCAST...");
        Serial.println("   SSID: " + ssid);
        Serial.println("⚠️ Digite a senha do WiFi:");
        Serial.print("   Senha: ");
        
        // Aguardar senha via serial (timeout 30 segundos)
        unsigned long startTime = millis();
        while (!Serial.available() && (millis() - startTime < 30000)) {
            delay(100);
        }
        
        if (!Serial.available()) {
            Serial.println("\n❌ Timeout - senha não fornecida");
            return;
        }
        
        String password = Serial.readStringUntil('\n');
        password.trim();
        
        Serial.print("   Senha: ");
        for (size_t i = 0; i < password.length(); i++) Serial.print("*");
        Serial.println();
        
        sendWiFiCredentialsBroadcast(ssid, password);
    }
    */
    // ===== COMANDO DESABILITADO - SLAVES NÃO PRECISAM WiFi =====
    /*
    else if (command == "test_wifi_broadcast") {
        Serial.println("\n🧪 === TESTE DE BROADCAST WiFi ===");
        Serial.println("📡 Testando envio de credenciais WiFi...");
        
        // Verificar se ESP-NOW está funcionando
        if (!esp_now_is_peer_exist) {
            Serial.println("❌ ESP-NOW não está inicializado!");
            return;
        }
        
        // Verificar peer de broadcast
        uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        if (!esp_now_is_peer_exist(broadcastMac)) {
            Serial.println("⚠️ Peer de broadcast não registrado - registrando...");
            registerBroadcastPeer();
        }
        
        // Testar com credenciais de exemplo
        String testSSID = "TESTE_WIFI";
        String testPassword = "senha123";
        
        Serial.println("📤 Enviando credenciais de teste:");
        Serial.println("   SSID: " + testSSID);
        Serial.println("   Senha: " + testPassword);
        
        bool success = sendWiFiCredentialsBroadcast(testSSID, testPassword);
        
        if (success) {
            Serial.println("✅ Teste de broadcast bem-sucedido!");
            Serial.println("💡 Se você tem slaves próximos, eles devem receber as credenciais");
        } else {
            Serial.println("❌ Teste de broadcast falhou!");
            Serial.println("💡 Verifique o diagnóstico acima");
        }
        Serial.println("=====================================\n");
    }
    */
    else if (command == "debug_creds") {
        Serial.println("\n🔍 === DEBUG CREDENCIAIS WiFi ===");
        Serial.println("📋 Namespace: hydro_system (Supabase + Web UI)");
        Serial.println();
        
        Preferences preferences;
        preferences.begin("hydro_system", true);
        String ssid = preferences.getString("ssid", "");
        String password = preferences.getString("password", "");
        String userEmail = preferences.getString("user_email", "");
        String deviceName = preferences.getString("device_name", "");
        String location = preferences.getString("location", "");
        preferences.end();
        
        Serial.println("🔌 CREDENCIAIS WiFi (usadas no ESP-NOW):");
        Serial.println("   📶 SSID: '" + ssid + "' (length: " + String(ssid.length()) + ")");
        Serial.println("   🔐 Password: '" + password + "' (length: " + String(password.length()) + ")");
        Serial.println();
        Serial.println("📊 DADOS SUPABASE (NÃO enviados via ESP-NOW):");
        Serial.println("   📧 Email: '" + userEmail + "' " + (userEmail.length() == 0 ? "(não configurado)" : ""));
        Serial.println("   🏷️  Device: '" + deviceName + "'");
        Serial.println("   📍 Location: '" + location + "' " + (location.length() == 0 ? "(não configurado)" : ""));
        Serial.println();
        Serial.println("💡 NOTA: ESP-NOW envia APENAS ssid + password + channel");
        Serial.println("   Email e location são usados apenas para Supabase/Web UI");
        Serial.println("================================");
    }
#endif

#ifdef SLAVE_MODE
    else if (command == "status" || command.startsWith("relay") || 
             command == "on_all" || command == "off_all") {
        handleSlaveSerialCommands();
    }
#endif
    else {
        // Passar para o state manager
        stateManager.handleSerialCommand(command);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    systemStartTime = millis();
    
    // PROTEÇÃO GLOBAL - Watchdog com timeout maior
    esp_task_wdt_init(60, true); // ✅ Aumentado para 60 segundos
    esp_task_wdt_add(NULL);
    
    // Inicializar WiFi para obtener MAC address
    WiFi.mode(WIFI_STA);
    
    Serial.println("\n🚀 === ESP32 HIDROPÔNICO v3.0 ===");
    Serial.println("🏗️ Arquitetura: Estados Exclusivos");
    Serial.println("💾 Heap inicial: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("🆔 Device ID: " + getDeviceID());
    Serial.println("📶 MAC Address: " + WiFi.macAddress());
    Serial.println("==================================\n");
    
    Serial.println("🏗️ Inicializando HydroStateManager...");
    stateManager.begin();
    
    // ===== INICIALIZAÇÃO ESP-NOW (OPÇÃO 3: WiFi + ESP-NOW) =====
#ifdef MASTER_MODE
    Serial.println("\n🎯 Iniciando ESP-NOW Master Controller (Opção 3)");
    Serial.println("====================================================");
    Serial.println("📡 Modo: WiFi + ESP-NOW simultâneos");
    Serial.println("💡 O SLAVE detectará o canal do MASTER automaticamente");
    
    // Inicializar SaveManager
    if (!configManager.begin()) {
        Serial.println("❌ Erro ao inicializar SaveManager");
    } else {
        Serial.println("✅ SaveManager inicializado");
    }
    
    // Inicializar RelayCommandBox local (opcional - para relés locais do MASTER)
    masterRelayBox = new RelayCommandBox(0x20, "MASTER-RELAYS");
    if (!masterRelayBox->begin()) {
        Serial.println("⚠️ RelayCommandBox local não encontrado - continuando sem relés locais");
    } else {
        Serial.println("✅ RelayCommandBox local inicializado");
    }
    
    // 🚀 INICIALIZAR TASK DEDICADA ESP-NOW (Nova arquitetura)
    Serial.println("\n🚀 === INICIALIZANDO TASK DEDICADA ESP-NOW ===");
    Serial.println("================================================");
    
    espNowTask = new ESPNowTask();
    if (espNowTask->begin()) {
        Serial.println("✅ ESP-NOW Task Dedicada ATIVA!");
        Serial.println("   ✓ Core: 1 (dedicado)");
        Serial.println("   ✓ Canal: 6 (fixo)");
        Serial.println("   ✓ Queue: 10 mensagens");
        Serial.println("   ✓ Heartbeat: 15s");
        
        // Configurar callbacks
        espNowTask->setSlaveDiscoveryCallback([](const SlaveInfo& slave) {
            Serial.println("🔍 Novo slave descoberto: " + String(slave.name));
            Serial.println("   MAC: " + ESPNowTask::macToString(slave.mac));
            Serial.println("   Relés: " + String(slave.relayCount));
        });
        
        espNowTask->setSlaveStatusCallback([](const uint8_t* mac, bool online) {
            String status = online ? "✅ Online" : "❌ Offline";
            Serial.println("📡 Slave " + ESPNowTask::macToString(mac) + ": " + status);
        });
        
        // Enviar discovery inicial
        espNowTask->sendDiscovery();
        Serial.println("📢 Discovery inicial enviado");
        
    } else {
        Serial.println("❌ ERRO: Falha ao inicializar ESP-NOW Task");
        delete espNowTask;
        espNowTask = nullptr;
    }
    Serial.println("================================================\n");
    
    // 🌉 INICIALIZAR RELAY BRIDGE (Capa de traducción Supabase ↔ ESP-NOW)
    Serial.println("\n🌉 === INICIALIZANDO RELAY BRIDGE ===");
    Serial.println("========================================");
    
    // TODO: Inicializar SupabaseClient primero
    // SupabaseClient* supabaseClient = stateManager.getSupabaseClient();
    
    // Por ahora, inicializar sin Supabase (solo ESP-NOW)
    if (espNowTask) {
        relayBridge = new RelayBridge(nullptr, espNowTask);
        if (relayBridge->begin()) {
            Serial.println("✅ RelayBridge inicializado");
            Serial.println("   ✓ ESP-NOW: Activo");
            Serial.println("   ⚠️ Supabase: No configurado");
            Serial.println("   💡 Configure Supabase para polling automático");
            // relayBridge->setAutoProcessing(false);  // Deshabilitado hasta configurar Supabase
        } else {
            Serial.println("⚠️ RelayBridge en modo standby");
            Serial.println("   💡 Configure Supabase para habilitar");
        }
    }
    Serial.println("========================================\n");
    
    // Aguardar WiFi conectar (máximo 30 segundos)
    Serial.println("⏳ Aguardando WiFi conectar para detectar canal...");
    unsigned long wifiStart = millis();
    int dots = 0;
    while (!WiFi.isConnected() && (millis() - wifiStart < 30000)) {
        delay(500);
        Serial.print(".");
        dots++;
        if (dots >= 60) {
            Serial.println();
            dots = 0;
        }
    }
    if (dots > 0) Serial.println();
    
    if (WiFi.isConnected()) {
        // Obter canal do WiFi conectado
        wifi_second_chan_t secondChan;
        uint8_t wifiChannel;
        esp_wifi_get_channel(&wifiChannel, &secondChan);
        
        Serial.println("✅ WiFi conectado!");
        Serial.println("📶 Canal WiFi detectado: " + String(wifiChannel));
        Serial.println("🌐 IP: " + WiFi.localIP().toString());
        Serial.println("📡 SSID: " + WiFi.SSID());
        
        // Inicializar ESP-NOW Bridge no mesmo canal do WiFi
        Serial.println("🔧 Inicializando ESP-NOW no canal " + String(wifiChannel) + "...");
        masterBridge = new ESPNowBridge(masterRelayBox, wifiChannel);
        if (!masterBridge->begin()) {
            Serial.println("❌ Erro ao inicializar ESP-NOW Bridge");
        } else {
            Serial.println("✅ ESP-NOW Bridge inicializado");
            
            // Configurar callbacks
            setupMasterCallbacks();
            
            // Inicializar SafetyWatchdog
            Serial.println("🛡️ Inicializando SafetyWatchdog...");
            watchdog.begin();
            
            // Configurar callback para PONG de slaves via ESPNowController
            ESPNowController* controller = masterBridge->getESPNowController();
            if (controller) {
                controller->setPingCallback([](const uint8_t* senderMac) {
                    // Master recebe PONG dos SLAVES - apenas atualizar status do slave
                    for (auto& slave : knownSlaves) {
                        if (memcmp(slave.mac, senderMac, 6) == 0) {
                            slave.lastSeen = millis();
                            slave.online = true;
                            Serial.println("🏓 Pong recebido de: " + slave.name + " (online)");
                            break;
                        }
                    }
                });
            }
            Serial.println("✅ SafetyWatchdog configurado");
            
            Serial.println("\n🎯 Master Controller pronto!");
            Serial.println("📡 MAC Master: " + masterBridge->getLocalMacString());
            Serial.println("📶 Canal: " + String(wifiChannel) + " (sincronizado com WiFi)");
            Serial.println("✅ WiFi + ESP-NOW funcionando juntos!");
            Serial.println("💡 SLAVE deve estar configurado para detectar canal automaticamente");
            
            // ===== SISTEMA AUTOMÁTICO DE DISCOVERY (SEM CREDENCIAIS WiFi) =====
            // NOTA: Slaves conectam via ESP-NOW puro, sem precisar de WiFi
            //       Discovery acontece via broadcast ESP-NOW periódico
            autoDiscoverAndConnect();
            
            // ===== CONEXÃO AUTOMÁTICA COM SLAVES (NOVA IMPLEMENTAÇÃO) =====
            if (espNowTask) {
                Serial.println("\n🚀 === CONEXÃO AUTOMÁTICA COM SLAVES ===");
                espNowTask->autoConnectToSlaves();
                Serial.println("=====================================\n");
            }
            
            // 🧠 INICIALIZAR SISTEMA INTELIGENTE DE COMUNICAÇÃO
            Serial.println("\n🤖 ==========================================");
            Serial.println("🤖 ATIVANDO SISTEMA INTELIGENTE");
            Serial.println("🤖 ==========================================");
            
            if (controller) {  // Reusar controller já declarado acima
                autoComm = new AutoCommunicationManager(controller, nullptr, true);  // nullptr = sem WiFi credentials
                if (autoComm->begin()) {
                    Serial.println("✅ Sistema Inteligente ATIVO!");
                    Serial.println("   ✓ Auto-Discovery: 30s");
                    Serial.println("   ✓ Heartbeat: 15s");
                    Serial.println("   ✓ Health Check: 10s");
                    Serial.println("   ✓ Auto-Recovery: 4 níveis");
                } else {
                    Serial.println("⚠️ Sistema Inteligente não inicializado");
                }
            }
            Serial.println("==========================================\n");
        }
    } else {
        Serial.println("\n⚠️ WiFi não conectado após 30 segundos");
        Serial.println("💡 ESP-NOW não inicializado - configure WiFi primeiro");
        Serial.println("📝 Use o comando 'wifi' para configurar");
    }
    
#endif

#ifdef SLAVE_MODE
    Serial.println("🚀 Iniciando ESP-NOW Slave");
    Serial.println("=========================");
    
    // Inicializar SaveManager
    if (!configManager.begin()) {
        Serial.println("❌ Erro ao inicializar SaveManager");
    } else {
        Serial.println("✅ SaveManager inicializado");
    }
    
    // Inicializar RelayCommandBox (modo simulação se hardware não disponível)
    relayBox = new RelayCommandBox(0x20, "ESP-NOW-SLAVE");
    if (!relayBox->begin()) {
        Serial.println("⚠️ Aviso: PCF8574 não encontrado - Modo simulação ativado");
        Serial.println("💡 Para funcionamento completo, conecte PCF8574 no endereço 0x20");
        // Continuar sem hardware para testes
    } else {
        Serial.println("✅ RelayCommandBox inicializado");
    }
    
    // Inicializar ESPNowBridge
    espNowBridge = new ESPNowBridge(relayBox, 1);
    if (!espNowBridge->begin()) {
        Serial.println("❌ Erro: Falha ao inicializar ESPNowBridge");
        return;
    }
    Serial.println("✅ ESPNowBridge inicializado");
    
    Serial.println("🎯 Sistema pronto para receber comandos do Master");
    Serial.println("📡 MAC Local: " + WiFi.macAddress());
    Serial.println("🔌 Relés disponíveis: 0-7");
    
#endif
    
    Serial.println("✅ Sistema iniciado - Estado: " + stateManager.getStateString());
    Serial.println("💡 Digite 'help' para comandos disponíveis\n");
}

void loop() {
    // PROTEÇÃO GLOBAL (sempre ativa)
    esp_task_wdt_reset();
    emergencyProtection();
    globalMemoryProtection();
    
    // GERENCIADOR DE ESTADOS (orquestrador principal)
    stateManager.loop();
    delay(100); // ✅ CORRIGIDO: Delay reduzido para evitar watchdog timeout
    
    // ===== ATUALIZAÇÕES ESP-NOW =====
#ifdef MASTER_MODE
    // 🌉 RELAY BRIDGE - Processar comandos de Supabase
    if (relayBridge) {
        relayBridge->update();  // Polling automático de Supabase → ESP-NOW
    }
    
    // 🧠 SISTEMA INTELIGENTE (prioritário)
    if (autoComm) {
        autoComm->update();  // ✨ CÉREBRO da comunicação
    }
    
    if (masterBridge) {
        // Alimentar watchdog de hardware (crítico)
        watchdog.feed();
        
        // Atualizar bridge
        masterBridge->update();
        
        // Monitorar slaves (já tem ping automático de 30s)
        monitorSlaves();
        
        // ===== SISTEMA DE MANUTENÇÃO AUTOMÁTICA DE CONEXÃO =====
        // Funciona igual WiFi.reconnect() - mantém conexão ativa
        maintainESPNOWConnection();
        
        // Verificar slaves offline periodicamente
        static unsigned long lastSlaveHealthCheck = 0;
        if (millis() - lastSlaveHealthCheck > 60000) { // A cada 1 minuto
            for (auto& slave : knownSlaves) {
                unsigned long timeSinceLastSeen = millis() - slave.lastSeen;
                if (timeSinceLastSeen > 90000) { // 90s sem resposta
                    if (slave.online) {
                        Serial.println("⚠️ Slave offline: " + slave.name + 
                                      " (sem resposta há " + String(timeSinceLastSeen/1000) + "s)");
                        slave.online = false;
                    }
                }
            }
            lastSlaveHealthCheck = millis();
        }
        
        // ===== ENVIO AUTOMÁTICO DE CREDENCIAIS WiFi - DESABILITADO =====
        // MOTIVO: Slaves usam ESP-NOW puro, não precisam de credenciais WiFi
        // Discovery automático via ESP-NOW (heartbeat + ping) é suficiente
    }
#endif

#ifdef SLAVE_MODE
    if (espNowBridge) {
        espNowBridge->update();
    }
    if (relayBox) {
        relayBox->update();
    }
#endif
    
    // COMANDOS SERIAIS
    handleGlobalSerialCommands();
    
    delay(100);
}

// ===== IMPLEMENTAÇÃO DAS FUNÇÕES MASTER =====
// Nota: handleRelayCommand já está implementada acima na linha 711

#ifdef SLAVE_MODE
void handleSlaveSerialCommands() {
    if (commandBuffer == "status") {
        Serial.println("\n📊 === STATUS SLAVE ===");
        Serial.println("🆔 MAC: " + WiFi.macAddress());
        Serial.println("📶 WiFi: " + (WiFi.isConnected() ? "✅ " + WiFi.localIP().toString() : "❌ Desconectado"));
        Serial.println("📡 ESP-NOW: " + (espNowBridge ? "✅ Ativo" : "❌ Inativo"));
        Serial.println("🔌 Relés: " + (relayBox ? "✅ Ativo" : "❌ Inativo"));
        Serial.println("========================\n");
    }
    else if (commandBuffer.startsWith("relay ")) {
        handleSlaveRelayCommand(commandBuffer);
    }
    else if (commandBuffer == "on_all") {
        Serial.println("🔌 Ligando todos os relés permanentemente...");
        if (relayBox) {
            for (int i = 0; i < 8; i++) {
                relayBox->processCommand(i, "on_forever", 0);
                delay(50);
            }
            Serial.println("✅ Todos os relés ligados permanentemente");
        } else {
            Serial.println("❌ RelayCommandBox não disponível");
        }
    }
    else if (commandBuffer == "off_all") {
        Serial.println("🔄 Desligando todos os relés...");
        if (relayBox) {
            for (int i = 0; i < 8; i++) {
                relayBox->processCommand(i, "off", 0);
                delay(50);
            }
            Serial.println("✅ Todos os relés desligados");
        } else {
            Serial.println("❌ RelayCommandBox não disponível");
        }
    }
}

void handleSlaveRelayCommand(const String& command) {
    // Formato: relay <número> <ação> [duração]
    // Exemplo: relay 0 on 30
    
    String cmd = command.substring(6); // Remove "relay "
    cmd.trim();
    
    // Dividir o comando em partes
    int firstSpace = cmd.indexOf(' ');
    if (firstSpace == -1) {
        Serial.println("❌ Formato: relay <número> <ação> [duração]");
        Serial.println("💡 Exemplo: relay 0 on 30");
        return;
    }
    
    String relayNumStr = cmd.substring(0, firstSpace);
    String actionAndDuration = cmd.substring(firstSpace + 1);
    actionAndDuration.trim();
    
    // Converter número do relé
    int relayNumber = relayNumStr.toInt();
    if (relayNumber < 0 || relayNumber > 7) {
        Serial.println("❌ Número do relé deve ser entre 0 e 7");
        return;
    }
    
    // Processar ação e duração
    int secondSpace = actionAndDuration.indexOf(' ');
    String action;
    int duration = 0;
    
    if (secondSpace == -1) {
        action = actionAndDuration;
    } else {
        action = actionAndDuration.substring(0, secondSpace);
        String durationStr = actionAndDuration.substring(secondSpace + 1);
        durationStr.trim();
        duration = durationStr.toInt();
    }
    
    // Validar ação
    if (action != "on" && action != "off" && action != "toggle" && 
        action != "on_forever" && action != "status") {
        Serial.println("❌ Ação inválida. Use: on, off, toggle, on_forever, status");
        return;
    }
    
    // Executar comando
    Serial.println("🔌 Executando comando local:");
    Serial.println("   Relé: " + String(relayNumber));
    Serial.println("   Ação: " + action);
    if (duration > 0) {
        Serial.println("   Duração: " + String(duration) + "s");
    }
    
    if (relayBox) {
        relayBox->processCommand(relayNumber, action, duration);
        Serial.println("✅ Comando executado");
    } else {
        Serial.println("❌ RelayCommandBox não disponível");
    }
}
#endif

// ===== COMANDOS SERIAIS GLOBAIS =====
// handleGlobalSerialCommands() ya definida en línea 1521
// handleMasterSerialCommands() ya definida en línea 759

#ifdef MASTER_MODE
// Función handleMasterSerialCommands() duplicada removida - usar la de línea 759

void handleMasterRelayCommand(const String& command) {
    // Formato: relay <slave> <número> <ação> [duração]
    // Exemplo: relay slave1 0 on 30
    
    String cmd = command.substring(6); // Remove "relay "
    cmd.trim();
    
    // Dividir o comando em partes
    int firstSpace = cmd.indexOf(' ');
    if (firstSpace == -1) {
        Serial.println("❌ Formato: relay <slave> <número> <ação> [duração]");
        Serial.println("💡 Exemplo: relay slave1 0 on 30");
        return;
    }
    
    String slaveName = cmd.substring(0, firstSpace);
    String rest = cmd.substring(firstSpace + 1);
    rest.trim();
    
    int secondSpace = rest.indexOf(' ');
    if (secondSpace == -1) {
        Serial.println("❌ Formato: relay <slave> <número> <ação> [duração]");
        Serial.println("💡 Exemplo: relay slave1 0 on 30");
        return;
    }
    
    String relayNumStr = rest.substring(0, secondSpace);
    String actionAndDuration = rest.substring(secondSpace + 1);
    actionAndDuration.trim();
    
    // Converter número do relé
    int relayNumber = relayNumStr.toInt();
    if (relayNumber < 0 || relayNumber > 7) {
        Serial.println("❌ Número do relé deve ser entre 0 e 7");
        return;
    }
    
    // Processar ação e duração
    int thirdSpace = actionAndDuration.indexOf(' ');
    String action;
    int duration = 0;
    
    if (thirdSpace == -1) {
        action = actionAndDuration;
    } else {
        action = actionAndDuration.substring(0, thirdSpace);
        String durationStr = actionAndDuration.substring(thirdSpace + 1);
        durationStr.trim();
        duration = durationStr.toInt();
    }
    
    // Validar ação
    if (action != "on" && action != "off" && action != "toggle") {
        Serial.println("❌ Ação inválida. Use: on, off, toggle");
        return;
    }
    
    // Encontrar MAC do slave
    if (!espNowTask) {
        Serial.println("❌ ESP-NOW Task não inicializada");
        return;
    }
    
    uint8_t* slaveMac = espNowTask->findSlaveMac(slaveName);
    if (!slaveMac) {
        Serial.println("❌ Slave não encontrado: " + slaveName);
        Serial.println("💡 Use 'list' para ver slaves disponíveis");
        return;
    }
    
    // Enviar comando
    Serial.println("📤 Enviando comando para " + slaveName + ":");
    Serial.println("   Relé: " + String(relayNumber));
    Serial.println("   Ação: " + action);
    if (duration > 0) {
        Serial.println("   Duração: " + String(duration) + "s");
    }
    
    if (espNowTask->sendRelayCommand(slaveMac, relayNumber, action.c_str(), duration)) {
        Serial.println("✅ Comando enviado com sucesso");
    } else {
        Serial.println("❌ Erro ao enviar comando");
    }
}

#endif 