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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ===== INCLUDES PARA MODO MASTER ESP-NOW =====
// ✅ ESTRUCTURA SIMPLE DE MASTER-TASK (100% funcional)
#include "ESPNowController.h"       // 📡 Controlador ESP-NOW (Core 0)
#include "MasterSlaveManager.h"     // 🎯 Sistema inteligente Master (Core 0)
#include "WebServerTask.h"          // 🌐 WebServer Task (Core 1)
#include "ObjectPoolManager.h"      // ✅ Object Pool Pattern (SSL, HTTP, JSON)
#include <vector>
#include <nvs_flash.h>              // Para initializeNVS()

// ===== SISTEMA DE PROTEÇÃO GLOBAL =====
HydroStateManager stateManager;
unsigned long systemStartTime = 0;
uint32_t minHeapSeen = UINT32_MAX;
unsigned long lastMemoryCheck = 0;

// ===== VARIÁVEIS GLOBAIS PARA WEBSERVERMANAGER =====
bool systemInitialized = true;  // Sistema sempre inicializado quando chega ao modo hydro
bool supabaseConnected = false; // Será atualizado pelo HydroSystemCore
bool webServerRunning = false;  // Será atualizado pelo WebServerManager

// ===== VARIÁVEL GLOBAL PARA REBOOT_COUNT (TÓPICO 3) =====
int globalRebootCount = 0;  // Contador de reinícios (carregado do NVS no setup)

// ===== VARIÁVEIS GLOBAIS PARA MODO MASTER ESP-NOW =====
// Descomente a linha correspondente ao modo desejado:
//#define MASTER_MODE    // ← Definido em platformio.ini
//#define SLAVE_MODE     // Descomente para modo Slave

// ===== PROTÓTIPOS DE FUNÇÃO =====
#ifdef MASTER_MODE
void setupCallbacks();
void onChannelChange(uint8_t newChannel);
void addSlaveToList(const uint8_t* macAddress, const String& deviceName, const String& deviceType, uint8_t numRelays);
uint8_t* findSlaveMac(const String& slaveName);
void printSlavesList();
void controlRelay(const String& slaveName, int relayNumber, const String& action, int duration);
void controlAllRelays(int relayNumber, const String& action, int duration);
void discoverSlaves();
void monitorSlaves();
void handleSerialCommands();  // ✅ Función que delega según el modo
void handleRelayCommand(const String& command);
void printHelp();
void printSystemStatus();
// ✅ Forward declarations para funciones usadas por printHelp() y printSystemStatus()
void printMasterHelp();  // ✅ Forward declaration para printHelp()
void printMasterStatus();  // ✅ Forward declaration para printSystemStatus()
// ✅ Task dedicada para ESP-NOW (Core 0)
void espNowTask(void* parameter);
#endif

#ifdef SLAVE_MODE
void handleSlaveSerialCommands();
void printSlaveHelp();
void handleSlaveRelayCommand(const String& command);
#endif

#ifdef MASTER_MODE
    // ✅ ESTRUCTURA PROFESIONAL: ESP-NOW en Task dedicada (Core 0)
    // 📡 ESP-NOW Task en Core 0 (mismo core que WiFi)
    ESPNowController master("MasterController", 1);  // Instancia para ESP-NOW
    
    // 🎯 SISTEMA PROFESIONAL MASTER-SLAVE (Core 0)
    MasterSlaveManager* masterManager = nullptr;
    
    // 📡 ESP-NOW TASK en Core 0 (Task dedicada para ESP-NOW)
    TaskHandle_t espNowTaskHandle = nullptr;    // Task dedicada Core 0
    
    // 🌐 WEBSERVER TASK en Core 1 (AsyncWebServer + REST API)
    WebServerTask* webServerTask = nullptr;    // Task dedicada Core 1
    
    // ✅ Usar masterManager directamente (sin alias)
    
    // Lista de slaves conocidos (compatibilidad con MASTER-TASK - usar PeerInfo)
    std::vector<PeerInfo> knownSlaves;
    
    // Buffer para comandos seriais
    static String commandBuffer = "";
    
    // ✅ VARIABLES DE TIEMPO PARA MONITOREO
    unsigned long lastSlaveCheck = 0;
    unsigned long lastReconnectionAttempt = 0;
    unsigned long lastSignalCheck = 0;
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

// ===== FUNCIÓN PARA INICIALIZAR NVS (de MASTER-TASK) =====
void initializeNVS() {
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║   💾 INICIALIZANDO NVS (Non-Volatile Storage)      ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    
    // Inicializar NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        Serial.println("⚠️  NVS precisa ser apagado e reinicializado...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    
    if (err != ESP_OK) {
        Serial.println("❌ Erro ao inicializar NVS: " + String(esp_err_to_name(err)));
        Serial.println("╚════════════════════════════════════════════════════╝\n");
        return;
    }
    
    Serial.println("✅ NVS inicializado com sucesso");
    Serial.println("💾 NVS pronto para uso!");
    Serial.println("╚════════════════════════════════════════════════════╝\n");
}

// ===== FUNCIONES PARA GERENCIAR REBOOT_COUNT (TÓPICO 2) =====
/**
 * @brief Lê o contador de reinícios do Preferences (NVS)
 * @return Número de reinícios acumulados (0 se não existir)
 */
int readRebootCount() {
    Preferences prefs;
    if (prefs.begin("hydro_system", true)) {  // true = modo leitura
        int count = prefs.getInt("reboot_count", 0);
        prefs.end();
        return count;
    }
    Serial.println("⚠️ Erro ao ler reboot_count do Preferences");
    return 0;
}

/**
 * @brief Salva o contador de reinícios no Preferences (NVS)
 * @param count Valor do contador a ser salvo
 * @return true se salvou com sucesso, false caso contrário
 */
bool saveRebootCount(int count) {
    Preferences prefs;
    if (prefs.begin("hydro_system", false)) {  // false = modo escrita
        bool success = prefs.putInt("reboot_count", count);
        prefs.end();
        if (success) {
            Serial.printf("✅ reboot_count salvo: %d\n", count);
            return true;
        } else {
            Serial.println("❌ Erro ao salvar reboot_count");
            return false;
        }
    }
    Serial.println("❌ Erro ao abrir Preferences para salvar reboot_count");
    return false;
}

/**
 * @brief Incrementa o contador de reinícios e salva no Preferences
 * @return Novo valor do contador após incremento
 */
int incrementAndSaveRebootCount() {
    Preferences prefs;
    if (prefs.begin("hydro_system", false)) {
        if (prefs.getBool("skip_reboot_inc", false)) {
            prefs.putBool("skip_reboot_inc", false);
            prefs.putInt("reboot_count", 0);
            prefs.end();
            globalRebootCount = 0;
            Serial.println("🔄 Primeiro boot apos portal: reboot_count mantido em 0");
            return 0;
        }
        prefs.end();
    }

    int currentCount = readRebootCount();
    int newCount = currentCount + 1;
    saveRebootCount(newCount);
    Serial.printf("🔄 Reinício detectado! Contador: %d → %d\n", currentCount, newCount);
    return newCount;
}

/**
 * @brief Retorna o contador de reinícios atual (variável global)
 * @return Valor atual do contador de reinícios
 * @note Esta função permite acesso externo ao reboot_count sem expor a variável global diretamente
 */
int getRebootCount() {
    return globalRebootCount;
}

void resetRebootCount() {
    globalRebootCount = 0;
    Preferences prefs;
    if (prefs.begin("hydro_system", false)) {
        prefs.putInt("reboot_count", 0);
        prefs.putBool("skip_reboot_inc", true);
        prefs.end();
        Serial.println("🔄 reboot_count zerado (portal) — proximo boot nao incrementa");
    } else {
        saveRebootCount(0);
    }
}

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

void setupCallbacks() {
    // ✅ CRÍTICO: Configurar callbacks do MasterSlaveManager (recebe notificações do ESPNowController)
    if (masterManager) {
        // ⭐ POTENCIA MÁXIMA: Callback quando um novo SLAVE é descoberto
        // Este callback é CRÍTICO - garante sincronização entre trustedSlaves e knownSlaves
        masterManager->setSlaveDiscoveredCallback([](const uint8_t* macAddress, const String& deviceName, const String& deviceType) {
            Serial.println("\n🎉 ========================================");
            Serial.println("🎉 NOVO SLAVE DESCOBERTO!");
            Serial.println("🎉 ========================================");
            Serial.println("📱 Nome: " + deviceName);
            Serial.println("🏷️ Tipo: " + deviceType);
            Serial.println("📡 MAC: " + ESPNowController::macToString(macAddress));
            Serial.println("========================================\n");
            
            // ⭐ POTENCIA MÁXIMA: Adicionar à lista de slaves conhecidos (knownSlaves)
            // Esta é a lista usada por controlAllRelays() e outros comandos
            Serial.println("📋 Adicionando slave à knownSlaves...");
            addSlaveToList(macAddress, deviceName, deviceType, 8); // 8 relés por padrão
            
            // Verificar se foi adicionado corretamente
            bool found = false;
            for (const auto& slave : knownSlaves) {
                if (memcmp(slave.macAddress, macAddress, 6) == 0) {
                    found = true;
                    Serial.println("✅ Verificação: Slave encontrado em knownSlaves!");
                    Serial.println("   Nome: " + slave.deviceName);
                    Serial.println("   Online: " + String(slave.online ? "Sim" : "Não"));
                    Serial.println("   Total de knownSlaves: " + String(knownSlaves.size()));
                    break;
                }
            }
            if (!found) {
                Serial.println("❌ ERRO CRÍTICO: Slave NÃO encontrado em knownSlaves após adicionar!");
                Serial.println("❌ Isso causará problemas com comandos 'relay on_all'");
            }
        });
        
        // Callback quando SLAVE fica online
        masterManager->setSlaveOnlineCallback([](const uint8_t* macAddress, const String& deviceName) {
            Serial.println("🟢 Slave ONLINE: " + deviceName + " (" + ESPNowController::macToString(macAddress) + ")");
            
            // ✅ Atualizar status na lista knownSlaves
            for (auto& slave : knownSlaves) {
                if (memcmp(slave.macAddress, macAddress, 6) == 0) {
                    slave.online = true;
                    slave.lastSeen = millis();
                    break;
                }
            }
        });
        
        // Callback quando SLAVE fica offline
        masterManager->setSlaveOfflineCallback([](const uint8_t* macAddress, const String& deviceName) {
            Serial.println("🔴 Slave OFFLINE: " + deviceName + " (" + ESPNowController::macToString(macAddress) + ")");
            
            // ✅ Atualizar status na lista knownSlaves
            for (auto& slave : knownSlaves) {
                if (memcmp(slave.macAddress, macAddress, 6) == 0) {
                    slave.online = false;
                    break;
                }
            }
        });
        
        // Callback para PING recebido do SLAVE
        masterManager->setPingReceivedCallback([](const uint8_t* macAddress, uint32_t pingId) {
            Serial.println("🏓 Ping recebido de: " + ESPNowController::macToString(macAddress));
            
            // ✅ Atualizar lastSeen ao receber PING
            for (auto& slave : knownSlaves) {
                if (memcmp(slave.macAddress, macAddress, 6) == 0) {
                    slave.lastSeen = millis();
                    slave.online = true;
                    break;
                }
            }
        });
        
        Serial.println("✅ Callbacks do MasterSlaveManager configurados");
    }
    
    // ⭐ POTENCIA MÁXIMA: Callback para handshake - usar información y solicitar DEVICE_INFO
    master.setHandshakeCallback([](const uint8_t* senderMac, uint32_t sessionId, const String& deviceName, bool wifiConnected) {
        Serial.println("\n🤝 ========================================");
        Serial.println("🤝 HANDSHAKE RESPONSE RECIBIDO!");
        Serial.println("🤝 ========================================");
        Serial.println("📥 De: " + ESPNowController::macToString(senderMac));
        Serial.println("📝 Nombre: " + deviceName);
        Serial.println("📶 WiFi: " + String(wifiConnected ? "Conectado" : "Desconectado"));
        Serial.println("🆔 Sesión: " + String(sessionId));
        
        // Actualizar slave con información del handshake
        if (masterManager) {
            TrustedSlave* slave = masterManager->getTrustedSlave(senderMac);
            if (slave) {
                // Actualizar nombre si es genérico
                if (slave->deviceName.startsWith("Slave-") || slave->deviceName == "Unknown") {
                    slave->deviceName = deviceName;
                    Serial.println("✅ Nombre actualizado desde handshake: " + deviceName);
                }
                slave->updateLastSeen();
            }
            
            // ⭐ POTENCIA MÁXIMA: El handshake ya tiene información básica
            // El slave debería enviar DEVICE_INFO automáticamente después del handshake
            // Si no lo hace, se solicitará cuando sea necesario
            Serial.println("💡 Esperando DEVICE_INFO del slave (debería enviarse automáticamente)");
            Serial.println("==========================================\n");
        }
    });
    
    // ✅ CORREÇÃO CRÍTICA: NÃO sobrescrever o callback do MasterSlaveManager!
    // O MasterSlaveManager já configura o callback correto (onRelayStatusReceivedStatic)
    // que chama processRelayStatusReceived() para atualizar os estados dos slaves.
    // Se sobrescrevermos aqui, os estados nunca serão atualizados!
    // 
    // O callback correto está em MasterSlaveManager::begin():
    // espNowController->setRelayStatusCallback(onRelayStatusReceivedStatic);
    //
    // REMOVIDO: Callback vazio que estava impedindo a atualização dos estados
    
    // Callback para ping/pong
    master.setPingCallback([](const uint8_t* senderMac) {
        Serial.println("🏓 Pong recebido de: " + ESPNowController::macToString(senderMac));
        
        // ✅ CORREÇÃO CRÍTICA: Atualizar lastSeen e marcar slave como online
        for (auto& slave : knownSlaves) {
            if (memcmp(slave.macAddress, senderMac, 6) == 0) {
                slave.lastSeen = millis();
                slave.online = true;
                break;
            }
        }
    });
    
    // 🔄 FASE 2: Callback para ACK de comandos de relay
    if (masterManager) {
        masterManager->setRelayAckCallback([](const uint8_t* senderMac, uint32_t commandId, bool success, 
            uint8_t relayNumber, uint8_t currentState) {
            
            Serial.println("\n🎊 === ACK DE RELAY RECEBIDO ===");
            Serial.println("📱 De: " + ESPNowController::macToString(senderMac));
            Serial.println("🆔 Command ID: " + String(commandId));
            Serial.println("🔌 Relé: " + String(relayNumber));
            Serial.println("✅ Success: " + String(success ? "Sim" : "Não"));
            Serial.println("💡 Estado: " + String(currentState ? "ON" : "OFF"));
            Serial.println("==================================\n");
        });
    }
}

void addSlaveToList(const uint8_t* macAddress, const String& deviceName, 
                   const String& deviceType, uint8_t numRelays) {
    // Verificar se já existe
    for (auto& slave : knownSlaves) {
        if (memcmp(slave.macAddress, macAddress, 6) == 0) {
            slave.online = true;
            slave.lastSeen = millis();
            slave.deviceName = deviceName;
            slave.deviceType = deviceType;
            return;
        }
    }
    
    // Adicionar novo slave (usando PeerInfo de MASTER-TASK)
    PeerInfo newSlave;
    memcpy(newSlave.macAddress, macAddress, 6);
    newSlave.deviceName = deviceName;
    newSlave.deviceType = deviceType;
    newSlave.online = true;
    newSlave.lastSeen = millis();
    newSlave.rssi = -50; // Valor padrão
    
    knownSlaves.push_back(newSlave);
    Serial.println("✅ Novo slave adicionado: " + deviceName);
}

uint8_t* findSlaveMac(const String& slaveName) {
    for (auto& slave : knownSlaves) {
        if (slave.deviceName == slaveName) {
            return slave.macAddress;
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
            Serial.printf("   %s %s\n", statusIcon.c_str(), slave.deviceName.c_str());
            Serial.printf("      Tipo: %s\n", slave.deviceType.c_str());
            Serial.printf("      MAC: %s\n", ESPNowController::macToString(slave.macAddress).c_str());
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
    if (!masterManager) return;
    
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
        success = masterManager->sendRelayCommandToSlave(slaveMac, relayNumber, action.c_str(), duration);
        
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
    if (!masterManager) {
        Serial.println("❌ MasterSlaveManager não inicializado");
        return;
    }
    
    Serial.println("\n📤 ========================================");
    Serial.println("📤 ENVIANDO COMANDO PARA TODOS OS SLAVES");
    Serial.println("📤 ========================================");
    Serial.println("🔌 Relé: " + String(relayNumber));
    Serial.println("⚡ Ação: " + action);
    if (duration > 0) {
        Serial.println("⏱️ Duração: " + String(duration) + "s");
    }
    Serial.println("----------------------------------------");
    
    // ⭐ POTENCIA MÁXIMA: Usar trustedSlaves como fonte principal (mais confiável)
    auto trustedSlaves = masterManager->getAllTrustedSlaves();
    int slavesFound = 0;
    int slavesOnline = 0;
    
    Serial.println("📊 Total de trustedSlaves: " + String(trustedSlaves.size()));
    
    if (trustedSlaves.empty()) {
        Serial.println("⚠️ Nenhum slave encontrado em trustedSlaves!");
        Serial.println("💡 Use 'list' ou 'discover' para encontrar slaves");
        Serial.println("========================================\n");
        return;
    }
    
    for (const auto& slave : trustedSlaves) {
        slavesFound++;
        if (slave.isOnline()) {
            slavesOnline++;
            Serial.println("📤 [" + String(slavesOnline) + "] Enviando para: " + slave.deviceName + 
                          " (" + ESPNowController::macToString(slave.macAddress) + ")");
            bool success = masterManager->sendRelayCommandToSlave(slave.macAddress, relayNumber, action.c_str(), duration);
            if (success) {
                Serial.println("   ✅ Comando enviado com sucesso");
            } else {
                Serial.println("   ❌ Falha ao enviar comando");
            }
            delay(100); // Pequeno delay entre comandos
        } else {
            Serial.println("⏭️  [" + String(slavesFound) + "] " + slave.deviceName + " está OFFLINE - pulando");
        }
    }
    
    Serial.println("----------------------------------------");
    Serial.println("📊 Resumo: " + String(slavesOnline) + " de " + String(slavesFound) + " slaves online");
    Serial.println("========================================\n");
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
        if (masterManager) {
            masterManager->rediscoverSlaves();
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
        // if (masterManager) watchdog.feed(); // watchdog no declarado
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
            if (slave.online && masterManager) {
                Serial.println("   → " + slave.deviceName);
                masterManager->sendPingToSlave(slave.macAddress);
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
            // reconnectESPNOWSlaves(); // Función legacy - usar masterManager->rediscoverSlaves()
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
            Serial.println("🔌 Tentando reconectar: " + slave.deviceName);
            
            // Tentar ping primeiro
            if (masterManager) {
                masterManager->sendPingToSlave(slave.macAddress);
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
    if (!masterManager) return;
    
    Serial.println("🔍 Procurando slaves...");
    masterManager->rediscoverSlaves();
    
    // ===== CORREÇÃO #5: TIMEOUT DE DESCOBERTA AUMENTADO =====
    // Aguardar respostas por 30 segundos (aumentado de 10s)
    const unsigned long DISCOVERY_TIMEOUT = 30000;  // 30 segundos
    unsigned long startTime = millis();
    int dotsCount = 0;
    
    Serial.print("⏳ Aguardando respostas");
    
    while (millis() - startTime < DISCOVERY_TIMEOUT) {
        masterManager->update();
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
    if (!masterManager) return;
    
    static unsigned long lastPing = 0;
    
    // ===== CORREÇÃO CRÍTICA: Usar trustedSlaves como fonte de verdade (igual a MASTER-TASK) =====
    // ✅ MASTER-TASK usa apenas uma lista (trustedSlaves) - não precisa de sincronização
    // ✅ Nossos comandos (on_all, off_all) já usam trustedSlaves
    // ✅ monitorSlaves() deve usar a mesma fonte para consistência
    const unsigned long PING_INTERVAL = 15000;  // 15 segundos
    const unsigned long CHECK_INTERVAL = 30000;  // 30 segundos
    
    // ✅ Usar trustedSlaves (fonte de verdade) - igual a MASTER-TASK
    auto trustedSlaves = masterManager->getAllTrustedSlaves();
    
    if (millis() - lastPing > PING_INTERVAL) {
        for (const auto& slave : trustedSlaves) {
            if (slave.isOnline()) {
                masterManager->sendPingToSlave(slave.macAddress);
            }
        }
        lastPing = millis();
    }
    
    // ✅ MasterSlaveManager já verifica offline automaticamente em update()
    // Não precisamos verificar manualmente aqui - o MasterSlaveManager faz isso
    // Apenas sincronizar knownSlaves com trustedSlaves para comandos que ainda usam knownSlaves
    static unsigned long lastSync = 0;
    if (millis() - lastSync > CHECK_INTERVAL) {
        // Sincronizar knownSlaves com trustedSlaves (para compatibilidade)
        for (const auto& trusted : trustedSlaves) {
            bool found = false;
            for (auto& known : knownSlaves) {
                if (memcmp(known.macAddress, trusted.macAddress, 6) == 0) {
                    found = true;
                    // Atualizar knownSlaves baseado em trustedSlaves (fonte de verdade)
                    known.online = trusted.isOnline();
                    known.lastSeen = trusted.lastSeen;
                    known.deviceName = trusted.deviceName;
                    known.deviceType = trusted.deviceType;
                    break;
                }
            }
            if (!found && trusted.isOnline()) {
                // Adicionar slave que está em trustedSlaves mas não em knownSlaves
                addSlaveToList(trusted.macAddress, trusted.deviceName, trusted.deviceType, trusted.numRelays);
            }
        }
        lastSync = millis();
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
                    printHelp();  // ✅ Usar printHelp() estándar
                }
                else if (command == "discover") {
                    discoverSlaves();
                }
    else if (command == "list") {
        printSlavesList();
    }
    else if (command == "status") {
        printSystemStatus();  // ✅ Usar printSystemStatus() estándar
    }
    else if (command == "watchdog_status") {
        // watchdog.printStatus();  // ❌ REMOVIDO: watchdog no existe
    }
    else if (command == "watchdog_reset") {
        // watchdog.reset();  // ❌ REMOVIDO: watchdog no existe
    }
    else if (command == "master_stats") {
        // Estatísticas do MasterSlaveManager
        if (masterManager) {
            masterManager->printStatus();
        } else {
            Serial.println("❌ MasterSlaveManager não inicializado");
        }
    }
    else if (command == "master_slaves") {
        // Lista confiável de slaves
        if (masterManager) {
            masterManager->printTrustedSlaves();
        } else {
            Serial.println("❌ MasterSlaveManager não inicializado");
        }
    }
    else if (command == "master_cleanup") {
        // Limpar slaves offline
        if (masterManager) {
            Serial.println("🧹 Limpando slaves offline...");
            masterManager->cleanupOfflineSlaves();
            Serial.println("✅ Limpeza concluída");
        } else {
            Serial.println("❌ MasterSlaveManager não inicializado");
        }
    }
    else if (command == "master_rediscover") {
        // Forçar re-discovery
        if (masterManager) {
            Serial.println("🔍 Forçando re-discovery...");
            masterManager->rediscoverSlaves();
            delay(5000); // Aguardar respostas
            masterManager->printTrustedSlaves();
        } else {
            Serial.println("❌ MasterSlaveManager não inicializado");
        }
    }
                else if (command.startsWith("ping ")) {
                    // Comando: ping <slave>
                    String slaveName = command.substring(5);
                    slaveName.trim();
                    uint8_t* slaveMac = findSlaveMac(slaveName);
                    if (slaveMac && masterManager) {
                        Serial.println("🏓 Enviando ping para " + slaveName + "...");
                        masterManager->sendPingToSlave(slaveMac);  // ✅ Método correcto
                    } else {
                        Serial.println("❌ Slave não encontrado: " + slaveName);
                    }
                }
                else if (command == "ping") {
                    // Ping em todos os slaves
                    Serial.println("🏓 Enviando ping para todos os slaves...");
                    for (const auto& slave : knownSlaves) {
                        if (slave.online && masterManager) {
                            Serial.println("   → " + slave.deviceName);
                            masterManager->sendPingToSlave(slave.macAddress);  // ✅ Método correcto
                            delay(50);
                        }
                    }
                }
                else if (command.startsWith("relay ")) {
                    // Verificar se é comando especial relay off_all ou relay on_all
                    if (command == "relay off_all") {
                        // 🔄 Desligar todos os relés em todos os slaves
                        // ✅ MESMA LÓGICA que comando simples de relé, mas percorrendo todos os relés
                        if (!masterManager) {
                            Serial.println("❌ MasterSlaveManager não inicializado");
                            return;
                        }
                        
                        Serial.println("🔄 Desligando todos os relés em todos os slaves...");
                        
                        auto trustedSlaves = masterManager->getAllTrustedSlaves();
                        
                        // ⭐ DIAGNÓSTICO: Verificar se há slaves
                        if (trustedSlaves.empty()) {
                            Serial.println("\n⚠️  NENHUM SLAVE DETECTADO!");
                            Serial.println("📋 Total de slaves confiáveis: 0");
                            Serial.println("💡 Ações sugeridas:");
                            Serial.println("   1. Execute 'discover' para procurar slaves");
                            Serial.println("   2. Execute 'list' ou 'master_slaves' para ver slaves conhecidos");
                            Serial.println("   3. Verifique se o slave está ligado e no mesmo canal WiFi");
                            Serial.println("✅ Comando relay off_all concluído (sem slaves para processar)");
                            return;
                        }
                        
                        Serial.println("📋 Total de slaves confiáveis: " + String(trustedSlaves.size()));
                        int slavesProcessed = 0;
                        int totalCommands = 0;
                        
                        // Iterar sobre cada slave
                        for (const auto& slave : trustedSlaves) {
                            if (slave.isOnline()) {
                                slavesProcessed++;
                                Serial.println("\n📡 [" + String(slavesProcessed) + "] Processando: " + slave.deviceName);
                                Serial.println("   MAC: " + ESPNowController::macToString(slave.macAddress));
                                
                                // Para cada slave, enviar comando para todos os relés (0-7)
                                int commandsSent = 0;
                                for (int relayNum = 0; relayNum < 8 && relayNum < slave.numRelays; relayNum++) {
                                    // ⭐ CRÍTICO: Verificar se slave ainda está online antes de cada comando
                                    auto currentSlaves = masterManager->getAllTrustedSlaves();
                                    bool slaveStillOnline = false;
                                    for (const auto& currentSlave : currentSlaves) {
                                        if (memcmp(currentSlave.macAddress, slave.macAddress, 6) == 0) {
                                            slaveStillOnline = currentSlave.isOnline();
                                            break;
                                        }
                                    }
                                    
                                    if (!slaveStillOnline) {
                                        Serial.println("   ⚠️  Slave ficou OFFLINE durante envio - parando");
                                        break;
                                    }
                                    
                                    bool success = masterManager->sendRelayCommandToSlave(
                                        slave.macAddress, 
                                        relayNum, 
                                        "off", 
                                        0,     // duration
                                        0,     // supabaseCommandId
                                        false  // ✅ NO actualizar status después de cada comando (operación en lote)
                                    );
                                    if (success) {
                                        commandsSent++;
                                        totalCommands++;
                                        delay(50); // Delay para permitir processamento
                                        
                                        // ⭐ CRÍTICO: Processar respostas para atualizar lastSeen
                                        if (masterManager) {
                                            masterManager->update();
                                        }
                                    }
                                }
                                Serial.println("   ✅ " + String(commandsSent) + " comando(s) enviado(s) para este slave");
                            } else {
                                Serial.println("⏭️  " + slave.deviceName + " está OFFLINE - pulando");
                            }
                        }
                        
                        // ✅ Actualizar status UNA SOLA VEZ al final de todos los comandos
                        Serial.println("\n🔄 Actualizando status de todos los slaves (una sola vez)...");
                        delay(500); // Delay para permitir que todos los comandos sean procesados
                        masterManager->requestAllSlavesRelayStatus();
                        
                        Serial.println("\n📊 Resumo:");
                        Serial.println("   Slaves processados: " + String(slavesProcessed) + " de " + String(trustedSlaves.size()));
                        Serial.println("   Total de comandos enviados: " + String(totalCommands));
                        Serial.println("✅ Comando relay off_all concluído");
                    }
                    else if (command == "relay on_all") {
                        // 🔌 Ligar todos os relés permanentemente em todos os slaves
                        // ✅ MESMA LÓGICA que comando simples de relé, mas percorrendo todos os relés
                        if (!masterManager) {
                            Serial.println("❌ MasterSlaveManager não inicializado");
                            return;
                        }
                        
                        Serial.println("🔌 Ligando todos os relés permanentemente em todos os slaves...");
                        
                        auto trustedSlaves = masterManager->getAllTrustedSlaves();
                        
                        // ⭐ DIAGNÓSTICO: Verificar se há slaves
                        if (trustedSlaves.empty()) {
                            Serial.println("\n⚠️  NENHUM SLAVE DETECTADO!");
                            Serial.println("📋 Total de slaves confiáveis: 0");
                            Serial.println("💡 Ações sugeridas:");
                            Serial.println("   1. Execute 'discover' para procurar slaves");
                            Serial.println("   2. Execute 'list' ou 'master_slaves' para ver slaves conhecidos");
                            Serial.println("   3. Verifique se o slave está ligado e no mesmo canal WiFi");
                            Serial.println("✅ Comando relay on_all concluído (sem slaves para processar)");
                            return;
                        }
                        
                        Serial.println("📋 Total de slaves confiáveis: " + String(trustedSlaves.size()));
                        int slavesProcessed = 0;
                        int totalCommands = 0;
                        
                        // Iterar sobre cada slave
                        for (const auto& slave : trustedSlaves) {
                            if (slave.isOnline()) {
                                slavesProcessed++;
                                Serial.println("\n📡 [" + String(slavesProcessed) + "] Processando: " + slave.deviceName);
                                Serial.println("   MAC: " + ESPNowController::macToString(slave.macAddress));
                                
                                // Para cada slave, enviar comando para todos os relés (0-7)
                                int commandsSent = 0;
                                for (int relayNum = 0; relayNum < 8 && relayNum < slave.numRelays; relayNum++) {
                                    // ⭐ CRÍTICO: Verificar se slave ainda está online antes de cada comando
                                    auto currentSlaves = masterManager->getAllTrustedSlaves();
                                    bool slaveStillOnline = false;
                                    for (const auto& currentSlave : currentSlaves) {
                                        if (memcmp(currentSlave.macAddress, slave.macAddress, 6) == 0) {
                                            slaveStillOnline = currentSlave.isOnline();
                                            break;
                                        }
                                    }
                                    
                                    if (!slaveStillOnline) {
                                        Serial.println("   ⚠️  Slave ficou OFFLINE durante envio - parando");
                                        break;
                                    }
                                    
                                    bool success = masterManager->sendRelayCommandToSlave(
                                        slave.macAddress, 
                                        relayNum, 
                                        "on",  // ON permanente (duration=0)
                                        0,     // duration
                                        0,     // supabaseCommandId
                                        false  // ✅ NO actualizar status después de cada comando (operación en lote)
                                    );
                                    if (success) {
                                        commandsSent++;
                                        totalCommands++;
                                        delay(50); // Delay para permitir processamento
                                        
                                        // ⭐ CRÍTICO: Processar respostas para atualizar lastSeen
                                        if (masterManager) {
                                            masterManager->update();
                                        }
                                    }
                                }
                                Serial.println("   ✅ " + String(commandsSent) + " comando(s) enviado(s) para este slave");
                            } else {
                                Serial.println("⏭️  " + slave.deviceName + " está OFFLINE - pulando");
                            }
                        }
                        
                        // ✅ Actualizar status UNA SOLA VEZ al final de todos los comandos
                        Serial.println("\n🔄 Actualizando status de todos los slaves (una sola vez)...");
                        delay(500); // Delay para permitir que todos los comandos sean procesados
                        masterManager->requestAllSlavesRelayStatus();
                        
                        Serial.println("\n📊 Resumo:");
                        Serial.println("   Slaves processados: " + String(slavesProcessed) + " de " + String(trustedSlaves.size()));
                        Serial.println("   Total de comandos enviados: " + String(totalCommands));
                        Serial.println("✅ Comando relay on_all concluído");
                    }
                    else {
                        handleRelayCommand(command);
                    }
                }
                else if (command == "on_all") {
                    // Ligar todos os relés permanentemente em todos os slaves
                    // ✅ MESMA LÓGICA que relay on_all
                    if (!masterManager) {
                        Serial.println("❌ MasterSlaveManager não inicializado");
                        return;
                    }
                    
                    Serial.println("🔌 Ligando todos os relés permanentemente em todos os slaves...");
                    
                    auto trustedSlaves = masterManager->getAllTrustedSlaves();
                    
                    // ⭐ DIAGNÓSTICO: Verificar se há slaves
                    if (trustedSlaves.empty()) {
                        Serial.println("\n⚠️  NENHUM SLAVE DETECTADO!");
                        Serial.println("📋 Total de slaves confiáveis: 0");
                        Serial.println("💡 Ações sugeridas:");
                        Serial.println("   1. Execute 'discover' para procurar slaves");
                        Serial.println("   2. Execute 'list' ou 'master_slaves' para ver slaves conhecidos");
                        Serial.println("   3. Verifique se o slave está ligado e no mesmo canal WiFi");
                        Serial.println("✅ Comando on_all concluído (sem slaves para processar)");
                        return;
                    }
                    
                    Serial.println("📋 Total de slaves confiáveis: " + String(trustedSlaves.size()));
                    int slavesProcessed = 0;
                    int totalCommands = 0;
                    
                    // Iterar sobre cada slave
                    for (const auto& slave : trustedSlaves) {
                        if (slave.isOnline()) {
                            slavesProcessed++;
                            Serial.println("\n📡 [" + String(slavesProcessed) + "] Processando: " + slave.deviceName);
                            Serial.println("   MAC: " + ESPNowController::macToString(slave.macAddress));
                            
                            // Para cada slave, enviar comando para todos os relés (0-7)
                            int commandsSent = 0;
                            for (int relayNum = 0; relayNum < 8 && relayNum < slave.numRelays; relayNum++) {
                                // ⭐ CRÍTICO: Verificar se slave ainda está online antes de cada comando
                                auto currentSlaves = masterManager->getAllTrustedSlaves();
                                bool slaveStillOnline = false;
                                for (const auto& currentSlave : currentSlaves) {
                                    if (memcmp(currentSlave.macAddress, slave.macAddress, 6) == 0) {
                                        slaveStillOnline = currentSlave.isOnline();
                                        break;
                                    }
                                }
                                
                                if (!slaveStillOnline) {
                                    Serial.println("   ⚠️  Slave ficou OFFLINE durante envio - parando");
                                    break;
                                }
                                
                                bool success = masterManager->sendRelayCommandToSlave(
                                    slave.macAddress, 
                                    relayNum, 
                                    "on",  // ON permanente (duration=0)
                                    0,     // duration
                                    0,     // supabaseCommandId
                                    false  // ✅ NO actualizar status después de cada comando (operación en lote)
                                );
                                if (success) {
                                    commandsSent++;
                                    totalCommands++;
                                    delay(50); // Delay para permitir processamento
                                    
                                    // ⭐ CRÍTICO: Processar respostas para atualizar lastSeen
                                    if (masterManager) {
                                        masterManager->update();
                                    }
                                }
                            }
                            Serial.println("   ✅ " + String(commandsSent) + " comando(s) enviado(s) para este slave");
                        } else {
                            Serial.println("⏭️  " + slave.deviceName + " está OFFLINE - pulando");
                        }
                    }
                    
                    // ✅ Actualizar status UNA SOLA VEZ al final de todos los comandos
                    Serial.println("\n🔄 Actualizando status de todos los slaves (una sola vez)...");
                    delay(500); // Delay para permitir que todos los comandos sean procesados
                    masterManager->requestAllSlavesRelayStatus();
                    
                    Serial.println("\n📊 Resumo:");
                    Serial.println("   Slaves processados: " + String(slavesProcessed) + " de " + String(trustedSlaves.size()));
                    Serial.println("   Total de comandos enviados: " + String(totalCommands));
                    Serial.println("✅ Comando on_all concluído");
                }
                else if (command == "off_all") {
                    // Desligar todos os relés em todos os slaves
                    // ✅ MESMA LÓGICA que relay off_all
                    if (!masterManager) {
                        Serial.println("❌ MasterSlaveManager não inicializado");
                        return;
                    }
                    
                    Serial.println("🔄 Desligando todos os relés em todos os slaves...");
                    
                    auto trustedSlaves = masterManager->getAllTrustedSlaves();
                    
                    // ⭐ DIAGNÓSTICO: Verificar se há slaves
                    if (trustedSlaves.empty()) {
                        Serial.println("\n⚠️  NENHUM SLAVE DETECTADO!");
                        Serial.println("📋 Total de slaves confiáveis: 0");
                        Serial.println("💡 Ações sugeridas:");
                        Serial.println("   1. Execute 'discover' para procurar slaves");
                        Serial.println("   2. Execute 'list' ou 'master_slaves' para ver slaves conhecidos");
                        Serial.println("   3. Verifique se o slave está ligado e no mesmo canal WiFi");
                        Serial.println("✅ Comando off_all concluído (sem slaves para processar)");
                        return;
                    }
                    
                    Serial.println("📋 Total de slaves confiáveis: " + String(trustedSlaves.size()));
                    int slavesProcessed = 0;
                    int totalCommands = 0;
                    
                    // Iterar sobre cada slave
                    for (const auto& slave : trustedSlaves) {
                        if (slave.isOnline()) {
                            slavesProcessed++;
                            Serial.println("\n📡 [" + String(slavesProcessed) + "] Processando: " + slave.deviceName);
                            Serial.println("   MAC: " + ESPNowController::macToString(slave.macAddress));
                            
                            // Para cada slave, enviar comando para todos os relés (0-7)
                            int commandsSent = 0;
                            for (int relayNum = 0; relayNum < 8 && relayNum < slave.numRelays; relayNum++) {
                                // ⭐ CRÍTICO: Verificar se slave ainda está online antes de cada comando
                                auto currentSlaves = masterManager->getAllTrustedSlaves();
                                bool slaveStillOnline = false;
                                for (const auto& currentSlave : currentSlaves) {
                                    if (memcmp(currentSlave.macAddress, slave.macAddress, 6) == 0) {
                                        slaveStillOnline = currentSlave.isOnline();
                                        break;
                                    }
                                }
                                
                                if (!slaveStillOnline) {
                                    Serial.println("   ⚠️  Slave ficou OFFLINE durante envio - parando");
                                    break;
                                }
                                
                                bool success = masterManager->sendRelayCommandToSlave(
                                    slave.macAddress, 
                                    relayNum, 
                                    "off", 
                                    0,     // duration
                                    0,     // supabaseCommandId
                                    false  // ✅ NO actualizar status después de cada comando (operación en lote)
                                );
                                if (success) {
                                    commandsSent++;
                                    totalCommands++;
                                    delay(50); // Delay para permitir processamento
                                    
                                    // ⭐ CRÍTICO: Processar respostas para atualizar lastSeen
                                    if (masterManager) {
                                        masterManager->update();
                                    }
                                }
                            }
                            Serial.println("   ✅ " + String(commandsSent) + " comando(s) enviado(s) para este slave");
                        } else {
                            Serial.println("⏭️  " + slave.deviceName + " está OFFLINE - pulando");
                        }
                    }
                    
                    // ✅ Actualizar status UNA SOLA VEZ al final de todos los comandos
                    Serial.println("\n🔄 Actualizando status de todos los slaves (una sola vez)...");
                    delay(500); // Delay para permitir que todos los comandos sean procesados
                    masterManager->requestAllSlavesRelayStatus();
                    
                    Serial.println("\n📊 Resumo:");
                    Serial.println("   Slaves processados: " + String(slavesProcessed) + " de " + String(trustedSlaves.size()));
                    Serial.println("   Total de comandos enviados: " + String(totalCommands));
                    Serial.println("✅ Comando off_all concluído");
                }
                // ❌ REMOVIDO TEMPORALMENTE: Comandos legacy obsoletos
                // handshake, connectivity_check, auto_validation
                // Estos métodos no existen en MasterSlaveManager
                // Se pueden reimplementar usando sendPingToSlave() y otros métodos disponibles
                else if (command == "handshake" || command.startsWith("handshake ") ||
                         command == "connectivity_check" || command.startsWith("connectivity_check ") ||
                         command == "auto_validation") {
                    Serial.println("⚠️ Comando legacy removido temporalmente");
                    Serial.println("💡 Use 'ping' o 'ping_all' para verificar conectividad");
                }
                // ===== COMANDOS DE WiFi CREDENTIALS REMOVIDOS =====
                // MOTIVO: Slaves usam ESP-NOW puro, não precisam de WiFi
                // Discovery automático via ESP-NOW é suficiente
                
                else if (command == "task_status") {
                    // Status da task dedicada ESP-NOW
                    if (masterManager) {
                        masterManager->printStatus(); // Usar MasterSlaveManager
                    } else {
                        Serial.println("❌ MasterSlaveManager não inicializado");
                    }
                }
                else if (command == "task_discover") {
                    // Discovery usando task dedicada
                    if (masterManager) {
                        Serial.println("🔍 Enviando discovery via MasterSlaveManager...");
                        masterManager->rediscoverSlaves();
                    } else {
                        Serial.println("❌ MasterSlaveManager não inicializado");
                    }
                }
                // task_wifi_broadcast REMOVIDO - não é mais necessário
                // ❌ REMOVIDO: RelayBridge no existe en el proyecto actual
                // else if (command == "bridge_stats") {
                //     if (relayBridge) {
                //         relayBridge->printStats();
                //     } else {
                //         Serial.println("❌ RelayBridge não inicializado");
                //     }
                // }
                // else if (command == "bridge_enable") {
                //     if (relayBridge) {
                //         relayBridge->setAutoProcessing(true);
                //         Serial.println("✅ RelayBridge habilitado - Polling automático ativo");
                //     } else {
                //         Serial.println("❌ RelayBridge não inicializado");
                //     }
                // }
                // else if (command == "bridge_disable") {
                //     if (relayBridge) {
                //         relayBridge->setAutoProcessing(false);
                //         Serial.println("⚠️ RelayBridge deshabilitado - Polling pausado");
                //     } else {
                //         Serial.println("❌ RelayBridge não inicializado");
                //     }
                // }
                else if (command.startsWith("EC ") || command == "EC CAL 1413") {
                    stateManager.handleSerialCommand(command);
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
    // ❌ REMOVIDO: RelayBridge no existe en el proyecto actual
    // Serial.println();
    // Serial.println("🌉 RELAY BRIDGE (Supabase ↔ ESP-NOW):");
    // Serial.println("   bridge_stats           - Estatísticas do RelayBridge");
    // Serial.println("   bridge_enable          - Habilitar polling automático");
    // Serial.println("   bridge_disable         - Desabilitar polling automático");
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

// ===== FUNCIÓN printHelp() PARA MODO MASTER =====
// ✅ Implementación de printHelp() que delega a printMasterHelp()
void printHelp() {
    printMasterHelp();
}

// ===== FUNCIÓN printMasterStatus() - DEFINIDA PRIMERO =====
void printMasterStatus() {
    Serial.println("\n📊 === STATUS DO SISTEMA MASTER ===");
    Serial.println("🎯 Master Controller");
    if (masterManager) {
        Serial.println("   Canal: 1");
        Serial.printf("   Dispositivos online: %d / %d\n", 
                     masterManager->getOnlineSlaveCount(),
                     masterManager->getTrustedSlaveCount());
    } else {
        Serial.println("   ❌ MasterSlaveManager não inicializado");
    }
    Serial.println();
    
    int onlineSlaves = 0;
    for (const auto& slave : knownSlaves) {
        if (slave.online) onlineSlaves++;
    }
    
    Serial.printf("👥 Slaves: %d total (%d online, %d offline)\n", 
                  knownSlaves.size(), onlineSlaves, knownSlaves.size() - onlineSlaves);
    Serial.println();
    
    // ⭐ POTENCIA MÁXIMA: Mostrar estado dos relés remotos dos slaves
    if (masterManager) {
        auto trustedSlaves = masterManager->getAllTrustedSlaves();
        if (!trustedSlaves.empty()) {
            Serial.println("🔌 === ESTADO DOS RELÉS REMOTOS (SLAVES) ===");
            for (const auto& slave : trustedSlaves) {
                if (slave.isOnline()) {
                    Serial.println("\n📡 Slave: " + slave.deviceName + " (" + ESPNowController::macToString(slave.macAddress) + ")");
                    bool hasActiveRelays = false;
                    for (int i = 0; i < 8 && i < slave.numRelays; i++) {
                        if (slave.relayStates[i].lastUpdate > 0) { // Solo mostrar si hay información
                            hasActiveRelays = true;
                            String stateIcon = slave.relayStates[i].state ? "🟢 ON" : "🔴 OFF";
                            String timerInfo = "";
                            if (slave.relayStates[i].hasTimer) {
                                timerInfo = " ⏱️ " + String(slave.relayStates[i].remainingTime) + "s";
                            }
                            unsigned long age = (millis() - slave.relayStates[i].lastUpdate) / 1000;
                            Serial.printf("   Relé %d: %s%s (atualizado há %lu s)\n", 
                                        i, stateIcon.c_str(), timerInfo.c_str(), age);
                        }
                    }
                    if (!hasActiveRelays) {
                        Serial.println("   ⚠️ Nenhum estado de relé recebido ainda");
                    }
                }
            }
            Serial.println("==========================================\n");
        }
    }
    
    if (masterManager) {
        Serial.println("📊 Status ESP-NOW:");
        masterManager->printStatus();
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

// ===== FUNCIÓN printSystemStatus() PARA MODO MASTER =====
// ✅ Implementación de printSystemStatus() que delega a printMasterStatus()
void printSystemStatus() {
    printMasterStatus();
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
// ===== MÉTODO DESABILITADO - SLAVES NÃO PRECISAM DE CREDENCIAIS WiFi =====
// Slaves conectam via ESP-NOW puro, sem WiFi
/*
bool sendSavedWiFiCredentialsBroadcast() {
    Serial.println("\n🤖 === ENVIO AUTOMÁTICO DE CREDENCIAIS WiFi ===");
    
    // Verificar se Master Bridge está inicializado
    if (!masterManager) {
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
    if (!masterManager) {
        Serial.println("❌ Master Bridge não inicializado!");
        return false;
    }
    
    // Usar ESPNowController para enviar credenciais
    ESPNowController* controller = masterManager->getESPNowController();
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
            if (slave.online && masterManager) {
                Serial.println("   → " + slave.deviceName);
                masterManager->sendPingToSlave(slave.macAddress);
                delay(50);
            }
        }
    }
    else if (command.startsWith("ping ")) {
        // Comando: ping <slave>
        String slaveName = command.substring(5);
        slaveName.trim();
        uint8_t* slaveMac = findSlaveMac(slaveName);
        if (slaveMac && masterManager) {
            Serial.println("🏓 Enviando ping para " + slaveName + "...");
            masterManager->sendPingToSlave(slaveMac);
        } else {
            Serial.println("❌ Slave não encontrado: " + slaveName);
        }
    }
    // ⭐ POTENCIA MÁXIMA: Verificar comandos especiales ANTES de command.startsWith("relay ")
    // Padrão MASTER-TASK: Iterar sobre slaves primeiro, depois sobre relés
    else if (command == "relay on_all") {
        // Comando especial: ligar todos os relés permanentemente em todos os slaves
        Serial.println("\n🔌 ========================================");
        Serial.println("🔌 LIGANDO TODOS OS RELÉS EM TODOS OS SLAVES");
        Serial.println("🔌 ========================================");
        
        if (!masterManager) {
            Serial.println("❌ MasterSlaveManager não inicializado");
            return;
        }
        
        // ⭐ PADRÃO MASTER-TASK: Obter todos os slaves primeiro
        auto trustedSlaves = masterManager->getAllTrustedSlaves();
        
        if (trustedSlaves.empty()) {
            Serial.println("⚠️ Nenhum slave encontrado!");
            Serial.println("💡 Use 'list' ou 'discover' para encontrar slaves");
            Serial.println("========================================\n");
            return;
        }
        
        int slavesProcessed = 0;
        int totalCommands = 0;
        
        // Iterar sobre cada slave
        for (const auto& slave : trustedSlaves) {
            if (slave.isOnline()) {
                slavesProcessed++;
                Serial.println("\n📡 [" + String(slavesProcessed) + "] Processando: " + slave.deviceName);
                Serial.println("   MAC: " + ESPNowController::macToString(slave.macAddress));
                
                // Para cada slave, enviar comando para todos os relés (0-7)
                // ⭐ POTENCIA MÁXIMA: Delay maior para permitir que slave processe e envie ALL_RELAYS_STATUS
                int commandsSent = 0;
                for (int relayNum = 0; relayNum < 8 && relayNum < slave.numRelays; relayNum++) {
                    bool success = masterManager->sendRelayCommandToSlave(
                        slave.macAddress, 
                        relayNum, 
                        "on",  // ✅ CORREÇÃO: "on" com duration=0 (igual a MASTER-TASK) - Slave não aceita "on_forever"
                        0
                    );
                    if (success) {
                        commandsSent++;
                        totalCommands++;
                        // ⭐ Delay maior para dar tempo ao slave processar comando e enviar ALL_RELAYS_STATUS
                        // Com comandos unitários, o delay natural do usuário já permite isso
                        delay(200); // 200ms entre comandos (aumentado de 50ms)
                    }
                }
                Serial.println("   ✅ " + String(commandsSent) + " comando(s) enviado(s) para este slave");
            } else {
                Serial.println("⏭️  " + slave.deviceName + " está OFFLINE - pulando");
            }
        }
        
        Serial.println("\n📊 Resumo:");
        Serial.println("   Slaves processados: " + String(slavesProcessed) + " de " + String(trustedSlaves.size()));
        Serial.println("   Total de comandos enviados: " + String(totalCommands));
        Serial.println("========================================\n");
        return; // ✅ CORREÇÃO: Evitar que o comando seja passado ao stateManager
    }
    else if (command == "relay off_all") {
        // Comando especial: desligar todos os relés em todos os slaves
        Serial.println("\n🔄 ========================================");
        Serial.println("🔄 DESLIGANDO TODOS OS RELÉS EM TODOS OS SLAVES");
        Serial.println("🔄 ========================================");
        
        if (!masterManager) {
            Serial.println("❌ MasterSlaveManager não inicializado");
            return;
        }
        
        // ⭐ PADRÃO MASTER-TASK: Obter todos os slaves primeiro
        auto trustedSlaves = masterManager->getAllTrustedSlaves();
        
        if (trustedSlaves.empty()) {
            Serial.println("⚠️ Nenhum slave encontrado!");
            Serial.println("💡 Use 'list' ou 'discover' para encontrar slaves");
            Serial.println("========================================\n");
            return;
        }
        
        int slavesProcessed = 0;
        int totalCommands = 0;
        
        // Iterar sobre cada slave
        for (const auto& slave : trustedSlaves) {
            if (slave.isOnline()) {
                slavesProcessed++;
                Serial.println("\n📡 [" + String(slavesProcessed) + "] Processando: " + slave.deviceName);
                Serial.println("   MAC: " + ESPNowController::macToString(slave.macAddress));
                
                // Para cada slave, enviar comando para todos os relés (0-7)
                // ⭐ POTENCIA MÁXIMA: Delay maior para permitir que slave processe e envie ALL_RELAYS_STATUS
                int commandsSent = 0;
                for (int relayNum = 0; relayNum < 8 && relayNum < slave.numRelays; relayNum++) {
                    bool success = masterManager->sendRelayCommandToSlave(
                        slave.macAddress, 
                        relayNum, 
                        "off", 
                        0
                    );
                    if (success) {
                        commandsSent++;
                        totalCommands++;
                        // ⭐ Delay maior para dar tempo ao slave processar comando e enviar ALL_RELAYS_STATUS
                        // Com comandos unitários, o delay natural do usuário já permite isso
                        delay(200); // 200ms entre comandos (aumentado de 50ms)
                    }
                }
                Serial.println("   ✅ " + String(commandsSent) + " comando(s) enviado(s) para este slave");
            } else {
                Serial.println("⏭️  " + slave.deviceName + " está OFFLINE - pulando");
            }
        }
        
        Serial.println("\n📊 Resumo:");
        Serial.println("   Slaves processados: " + String(slavesProcessed) + " de " + String(trustedSlaves.size()));
        Serial.println("   Total de comandos enviados: " + String(totalCommands));
        Serial.println("========================================\n");
        return; // ✅ CORREÇÃO: Evitar que o comando seja passado ao stateManager
    }
    else if (command.startsWith("relay ")) {
        handleRelayCommand(command);
    }
    else if (command == "on_all") {
        // Ligar todos os relés permanentemente em todos os slaves
        // ✅ MESMA LÓGICA que relay on_all
        if (!masterManager) {
            Serial.println("❌ MasterSlaveManager não inicializado");
            return;
        }
        
        Serial.println("🔌 Ligando todos os relés permanentemente em todos os slaves...");
        
        auto trustedSlaves = masterManager->getAllTrustedSlaves();
        int slavesProcessed = 0;
        int totalCommands = 0;
        
        // Iterar sobre cada slave
        for (const auto& slave : trustedSlaves) {
            if (slave.isOnline()) {
                slavesProcessed++;
                Serial.println("\n📡 [" + String(slavesProcessed) + "] Processando: " + slave.deviceName);
                Serial.println("   MAC: " + ESPNowController::macToString(slave.macAddress));
                
                // Para cada slave, enviar comando para todos os relés (0-7)
                int commandsSent = 0;
                for (int relayNum = 0; relayNum < 8 && relayNum < slave.numRelays; relayNum++) {
                    // ⭐ CRÍTICO: Verificar se slave ainda está online antes de cada comando
                    auto currentSlaves = masterManager->getAllTrustedSlaves();
                    bool slaveStillOnline = false;
                    for (const auto& currentSlave : currentSlaves) {
                        if (memcmp(currentSlave.macAddress, slave.macAddress, 6) == 0) {
                            slaveStillOnline = currentSlave.isOnline();
                            break;
                        }
                    }
                    
                    if (!slaveStillOnline) {
                        Serial.println("   ⚠️  Slave ficou OFFLINE durante envio - parando");
                        break;
                    }
                    
                    bool success = masterManager->sendRelayCommandToSlave(
                        slave.macAddress, 
                        relayNum, 
                        "on",  // ON permanente (duration=0)
                        0,     // duration
                        0,     // supabaseCommandId
                        false  // ✅ NO actualizar status después de cada comando (operación en lote)
                    );
                    if (success) {
                        commandsSent++;
                        totalCommands++;
                        delay(50); // Delay para permitir processamento
                        
                        // ⭐ CRÍTICO: Processar respostas para atualizar lastSeen
                        if (masterManager) {
                            masterManager->update();
                        }
                    }
                }
                Serial.println("   ✅ " + String(commandsSent) + " comando(s) enviado(s) para este slave");
            } else {
                Serial.println("⏭️  " + slave.deviceName + " está OFFLINE - pulando");
            }
        }
        
        // ✅ Actualizar status UNA SOLA VEZ al final de todos los comandos
        Serial.println("\n🔄 Actualizando status de todos los slaves (una sola vez)...");
        delay(500); // Delay para permitir que todos los comandos sean procesados
        masterManager->requestAllSlavesRelayStatus();
        
        Serial.println("\n📊 Resumo:");
        Serial.println("   Slaves processados: " + String(slavesProcessed) + " de " + String(trustedSlaves.size()));
        Serial.println("   Total de comandos enviados: " + String(totalCommands));
        Serial.println("✅ Comando on_all concluído");
        return; // ✅ CORREÇÃO: Evitar que o comando seja passado ao stateManager
    }
    else if (command == "off_all") {
        // Desligar todos os relés em todos os slaves
        // ✅ MESMA LÓGICA que relay off_all
        if (!masterManager) {
            Serial.println("❌ MasterSlaveManager não inicializado");
            return;
        }
        
        Serial.println("🔄 Desligando todos os relés em todos os slaves...");
        
        auto trustedSlaves = masterManager->getAllTrustedSlaves();
        int slavesProcessed = 0;
        int totalCommands = 0;
        
        // Iterar sobre cada slave
        for (const auto& slave : trustedSlaves) {
            if (slave.isOnline()) {
                slavesProcessed++;
                Serial.println("\n📡 [" + String(slavesProcessed) + "] Processando: " + slave.deviceName);
                Serial.println("   MAC: " + ESPNowController::macToString(slave.macAddress));
                
                // Para cada slave, enviar comando para todos os relés (0-7)
                int commandsSent = 0;
                for (int relayNum = 0; relayNum < 8 && relayNum < slave.numRelays; relayNum++) {
                    // ⭐ CRÍTICO: Verificar se slave ainda está online antes de cada comando
                    auto currentSlaves = masterManager->getAllTrustedSlaves();
                    bool slaveStillOnline = false;
                    for (const auto& currentSlave : currentSlaves) {
                        if (memcmp(currentSlave.macAddress, slave.macAddress, 6) == 0) {
                            slaveStillOnline = currentSlave.isOnline();
                            break;
                        }
                    }
                    
                    if (!slaveStillOnline) {
                        Serial.println("   ⚠️  Slave ficou OFFLINE durante envio - parando");
                        break;
                    }
                    
                    bool success = masterManager->sendRelayCommandToSlave(
                        slave.macAddress, 
                        relayNum, 
                        "off", 
                        0
                    );
                    if (success) {
                        commandsSent++;
                        totalCommands++;
                        delay(50); // Delay para permitir processamento
                        
                        // ⭐ CRÍTICO: Processar respostas para atualizar lastSeen
                        if (masterManager) {
                            masterManager->update();
                        }
                    }
                }
                Serial.println("   ✅ " + String(commandsSent) + " comando(s) enviado(s) para este slave");
            } else {
                Serial.println("⏭️  " + slave.deviceName + " está OFFLINE - pulando");
            }
        }
        
        Serial.println("\n📊 Resumo:");
        Serial.println("   Slaves processados: " + String(slavesProcessed) + " de " + String(trustedSlaves.size()));
        Serial.println("   Total de comandos enviados: " + String(totalCommands));
        
        if (trustedSlaves.empty()) {
            Serial.println("⚠️  NENHUM SLAVE DETECTADO!");
            Serial.println("📋 Total de slaves confiáveis: 0");
            Serial.println("💡 Ações sugeridas:");
            Serial.println("   1. Execute 'discover' para procurar slaves");
            Serial.println("   2. Execute 'list' ou 'master_slaves' para ver slaves conhecidos");
            Serial.println("   3. Verifique se o slave está ligado e no mesmo canal WiFi");
            Serial.println("✅ Comando off_all concluído (sem slaves para processar)");
        } else if (slavesProcessed == 0) {
            Serial.println("⚠️  Nenhum slave online para processar");
            Serial.println("✅ Comando off_all concluído (sem slaves online)");
        } else {
            Serial.println("✅ Comando off_all concluído");
        }
        
        return; // ✅ CORREÇÃO: Evitar que o comando seja passado ao stateManager
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
        // ✅ CORREÇÃO: Ignorar comandos vazios
        if (command.length() == 0) {
            return;
        }
        // Passar para o state manager apenas se o comando não foi processado acima
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
    
    // Inicializar NVS ANTES de tudo (de MASTER-TASK)
    initializeNVS();
    esp_task_wdt_reset();  // ✅ Resetar watchdog após NVS
    
    // ===== TÓPICO 3: INICIALIZAR REBOOT_COUNT =====
    // Incrementar contador de reinícios e salvar no NVS
    globalRebootCount = incrementAndSaveRebootCount();
    Serial.printf("📊 Contador de reinícios atual: %d\n", globalRebootCount);
    esp_task_wdt_reset();  // ✅ Resetar watchdog após operação NVS
    
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║   🚀 INICIALIZAÇÃO DO SISTEMA EMBARCADO            ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    Serial.println("📊 Informações do Sistema:");
    Serial.printf("   • Device ID:       %s\n", getDeviceID().c_str());
    Serial.printf("   • MAC Address:     %s\n", WiFi.macAddress().c_str());
    Serial.printf("   • Heap Livre:      %u bytes\n", ESP.getFreeHeap());
    Serial.printf("   • Reboot Count:     %d\n", globalRebootCount);
    Serial.println();
    
    // ✅ CORREÇÃO CRÍTICA: Inicializar Object Pool Manager ANTES de stateManager.begin()
    // Isso garante que Supabase tenha acesso ao pool quando HydroSystemCore::begin() for chamado
    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════╗");
    Serial.println("║   🎯 Inicializando Object Pool Manager (CRÍTICO)  ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    
    // ✅ NOVO: Verificar memória antes de inicializar pools
    uint32_t freeHeapBefore = ESP.getFreeHeap();
    Serial.printf("💾 Heap antes de Object Pool: %u bytes\n", freeHeapBefore);
    
    if (freeHeapBefore < 100000) {  // Mínimo 100KB necessário
        Serial.printf("⚠️ [CRÍTICO] Heap muito baixo (%u bytes) para inicializar Object Pool!\n", freeHeapBefore);
        Serial.println("   Tentando continuar mesmo assim...");
    }
    
    esp_task_wdt_reset();  // ✅ Resetar watchdog antes de alocação grande
    
    if (ObjectPoolManager::initialize()) {
        uint32_t freeHeapAfter = ESP.getFreeHeap();
        Serial.println("✅ Object Pool Manager inicializado");
        Serial.printf("   ✓ SSL Client Pool: 2 clientes\n");
        Serial.printf("   ✓ HTTP Client Pool: 3 clientes\n");
        Serial.printf("   ✓ JSON Document Pool: 3 documentos\n");
        Serial.printf("💾 Heap após Object Pool: %u bytes (usado: %u bytes)\n", 
                     freeHeapAfter, freeHeapBefore - freeHeapAfter);
    } else {
        Serial.println("❌ [CRÍTICO] Erro ao inicializar Object Pool Manager!");
        Serial.println("⚠️ Sistema continuará, mas pode ter problemas de memória");
        Serial.println("   Supabase usará modo legacy (menos eficiente)");
    }
    Serial.println();
    esp_task_wdt_reset();  // ✅ Resetar watchdog após inicialização
    
    Serial.println("🏗️ Inicializando HydroStateManager...");
    stateManager.begin();
    
    // ===== INICIALIZAÇÃO ESP-NOW (ESTRUCTURA DE MASTER-TASK) =====
#ifdef MASTER_MODE
    // ⭐ POTENCIA MÁXIMA: Leer device_name de Preferences y actualizar Master
    Preferences prefs;
    if (prefs.begin("hydro_system", true)) {
        String savedDeviceName = prefs.getString("device_name", "");
        if (savedDeviceName.length() > 0) {
            Serial.println("\n📝 === ACTUALIZANDO NOMBRE DEL MASTER ===");
            Serial.println("📝 Nombre guardado: '" + savedDeviceName + "'");
            master.setDeviceName(savedDeviceName);
            Serial.println("✅ Master actualizado con nombre: " + savedDeviceName);
            Serial.println("==========================================\n");
        } else {
            Serial.println("\n💡 Device name no configurado - usando 'MasterController' por defecto");
        }
        prefs.end();
    }
    
    Serial.println("\n");
    Serial.println("╔════════════════════════════════════════════════════╗");
    Serial.println("║   🎯 ESP-NOW MASTER - Controlador Principal       ║");
    Serial.println("║   Con WiFi AP+STA + AsyncWebServer                ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    Serial.println();
    Serial.println("📋 Configuración:");
    Serial.println("   ✓ Modo: MASTER (Controlador Puro)");
    Serial.println("   ✓ Hardware: Solo ESP32");
    Serial.println("   ✓ Función: Controlar Slaves remotos via ESP-NOW");
    Serial.println("   ✓ WiFi: AP+STA con Web Server");
    Serial.println("   ✓ Canal: Dinámico (sincronizado con WiFi)");
    Serial.println();
    
    // 📶 Obtener canal WiFi (o usar predeterminado)
    uint8_t espnowChannel = 1; // Canal predeterminado
    if (WiFi.isConnected()) {
        wifi_second_chan_t secondChan;
        esp_wifi_get_channel(&espnowChannel, &secondChan);
        Serial.println("\n🔄 Canal WiFi detectado: " + String(espnowChannel));
        Serial.println("   ESP-NOW se sincronizará con este canal");
    } else {
        Serial.println("\n⚠️ WiFi Station no conectado - usando canal predeterminado: " + String(espnowChannel));
    }
    
    // 📡 FASE 2: Inicializar ESP-NOW en el canal correcto
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║   📡 FASE 2: Inicializando ESP-NOW                 ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    Serial.println("🔧 Inicializando ESP-NOW en canal " + String(espnowChannel) + "...");
    
    // Forzar canal antes de inicializar ESP-NOW
    esp_wifi_set_channel(espnowChannel, WIFI_SECOND_CHAN_NONE);
    delay(100);
    
    if (!master.begin()) {
        Serial.println("❌ Erro ao inicializar ESP-NOW Master");
        return;
    }
    Serial.println("✅ ESP-NOW Master inicializado en canal " + String(espnowChannel));
    
    // ✅ Inyectar ESPNowController en el StateManager
    stateManager.setESPNowController(&master);
    Serial.println("   ✓ ESPNowController inyectado en StateManager");
    esp_task_wdt_reset();  // ✅ Resetar watchdog após ESP-NOW
    
    // ✅ CORREÇÃO BUG #4: Inicializar MasterSlaveManager ANTES dos callbacks
    Serial.println("\n🎯 Inicializando MasterSlaveManager...");
    
    // ✅ NOVO: Verificar memória antes de criar MasterSlaveManager
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 50000) {
        Serial.printf("⚠️ Heap baixo (%u bytes) antes de criar MasterSlaveManager\n", freeHeap);
    }
    
    masterManager = new MasterSlaveManager(&master);
    if (masterManager->begin()) {
        Serial.println("✅ MasterSlaveManager inicializado");
        Serial.println("   ✓ Sistema de Retry: ATIVO");
        Serial.println("   ✓ Sistema de ACK: ATIVO");
        Serial.println("   ✓ Lista Confiável: ATIVA");
    } else {
        Serial.println("❌ Erro ao inicializar MasterSlaveManager");
        return;
    }
    esp_task_wdt_reset();  // ✅ Resetar watchdog após MasterSlaveManager
    
    // ✅ NOVO: Injetar MasterSlaveManager no StateManager (elimina uso de extern)
    stateManager.setMasterManager(masterManager);
    Serial.println("   ✓ MasterSlaveManager injetado no StateManager");
    
    // Configurar callbacks (DEPOIS de criar masterManager)
    setupCallbacks();
    esp_task_wdt_reset();  // ✅ Resetar watchdog após callbacks
    
    // ✅ CRIAR TASK DEDICADA PARA ESP-NOW (Core 0)
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║   📡 CRIANDO TASK DEDICADA ESP-NOW (Core 0)        ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    xTaskCreatePinnedToCore(
        espNowTask,           // Função da task
        "ESPNowTask",         // Nome da task
        8192,                 // Stack size (8KB)
        nullptr,              // Parâmetros
        2,                    // Prioridade (alta, mas menor que WiFi)
        &espNowTaskHandle,    // Handle da task
        0                     // Core 0 (mesmo core que WiFi)
    );
    
    if (espNowTaskHandle) {
        Serial.println("✅ ESP-NOW Task criada com sucesso!");
        Serial.println("   ✓ Core: 0 (mesmo que WiFi)");
        Serial.println("   ✓ Stack: 8KB");
        Serial.println("   ✓ Prioridade: 2");
        Serial.println("   ✓ Handle: " + String((uint32_t)espNowTaskHandle, HEX));
    } else {
        Serial.println("❌ Erro ao criar ESP-NOW Task");
        Serial.println("⚠️ Continuando sem task dedicada...");
    }
    Serial.println();
    
    // ⭐ POTENCIA MÁXIMA: Sincronizar trustedSlaves com knownSlaves após configurar callbacks
    // Isso garante que slaves já descobertos sejam adicionados a knownSlaves
    if (masterManager) {
        Serial.println("\n🔄 Sincronizando trustedSlaves com knownSlaves...");
        auto trustedSlaves = masterManager->getAllTrustedSlaves();
        int synced = 0;
        for (const auto& slave : trustedSlaves) {
            bool exists = false;
            for (const auto& known : knownSlaves) {
                if (memcmp(known.macAddress, slave.macAddress, 6) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                Serial.println("📋 Sincronizando: " + slave.deviceName);
                addSlaveToList(slave.macAddress, slave.deviceName, slave.deviceType, slave.numRelays);
                synced++;
            }
        }
        if (synced > 0) {
            Serial.println("✅ " + String(synced) + " slave(s) sincronizado(s) com knownSlaves");
        } else {
            Serial.println("✅ knownSlaves já está sincronizado com trustedSlaves");
        }
        Serial.println("📊 Total knownSlaves: " + String(knownSlaves.size()));
        Serial.println("📊 Total trustedSlaves: " + String(trustedSlaves.size()));
    }
    
    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════╗");
    Serial.println("║   ✅ Master Controller Pronto!                     ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    Serial.println("📡 MAC Master: " + master.getLocalMacString());
    Serial.println("🎮 Interfaz: Serial Monitor 115200 baud");
    Serial.println("💡 Comandos: Digite 'help' para ver opciones");
    
    // 🔍 DISCOVERY AUTOMÁTICO INICIAL - Sistema Profissional (igual a MASTER-TASK)
    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════╗");
    Serial.println("║   🔍 DISCOVERY AUTOMÁTICO INICIAL                  ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    Serial.println("📡 Buscando Slaves en la red ESP-NOW...");
    Serial.println("⏳ Aguarde 5 segundos...");
    
    // ✅ PATRÓN MASTER-TASK: Enviar 3 broadcasts espaçados para garantir descoberta
    for (int i = 0; i < 3; i++) {
        master.sendDiscoveryBroadcast();  // ✅ Directo como MASTER-TASK
        Serial.printf("   Broadcast %d/3 enviado\n", i + 1);
        delay(1500); // 1.5s entre broadcasts
        master.update(); // Processar respostas imediatas
        if (masterManager) {
            masterManager->update(); // Processar respostas no MasterSlaveManager
        }
    }
    
    Serial.println();
    Serial.println("✅ Discovery inicial concluído");
    Serial.println("📋 Slaves descobertos: " + String(masterManager->getTrustedSlaveCount()));
    
    if (masterManager->getTrustedSlaveCount() > 0) {
        Serial.println();
        Serial.println("╔════════════════════════════════════════════════════╗");
        Serial.println("║   🎉 SLAVES ENCONTRADOS                            ║");
        Serial.println("╚════════════════════════════════════════════════════╝");
        masterManager->printTrustedSlaves();
    } else {
        Serial.println();
        Serial.println("╔════════════════════════════════════════════════════╗");
        Serial.println("║   ⚠️  SIN SLAVES DETECTADOS                        ║");
        Serial.println("╚════════════════════════════════════════════════════╝");
        Serial.println("💡 Verifica que los Slaves estén:");
        Serial.println("   1. Encendidos y programados con SLAVE_MODE");
        Serial.println("   2. En el mismo canal (ESPNOW_CHANNEL=1)");
        Serial.println("   3. Con hardware conectado (PCF8574 + relés)");
        Serial.println();
        Serial.println("🔧 Puedes hacer discovery manual: 'discover'");
    }
    
    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════╗");
    Serial.println("║   📝 SISTEMA LISTO PARA COMANDOS                   ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    Serial.println("💡 Digite 'help' para ver comandos disponibles");
    Serial.println("🎮 Ejemplos:");
    Serial.println("   • discover                   - Buscar Slaves");
    Serial.println("   • list                       - Ver Slaves conectados");
    Serial.println("   • relay SLAVE-01 0 on 30     - Controlar relé");
    Serial.println("   • off_all                    - Apagar todo");
    Serial.println();
    Serial.print("> ");
    
#if ENABLE_LOCAL_ADMIN_HTTP
    // 🌐 FASE 3: Inicializar WebServerTask en Core 1 (AsyncWebServer + REST API)
    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════╗");
    Serial.println("║   🌐 FASE 3: Inicializando WebServerTask (Core 1) ║");
    Serial.println("╚════════════════════════════════════════════════════╝");

    freeHeap = ESP.getFreeHeap();
    if (freeHeap < 50000) {
        Serial.printf("⚠️ Heap baixo (%u bytes) antes de criar WebServerTask\n", freeHeap);
    }

    webServerTask = new WebServerTask();
    esp_task_wdt_reset();

    if (webServerTask->begin()) {
        Serial.println("✅ WebServerTask inicializado en Core 1");
        Serial.println("   ✓ AsyncWebServer: Puerto 80");
        Serial.println("   ✓ REST API: /api/status, /api/command");
        Serial.println("   ✓ Archivos estáticos: SPIFFS");

        stateManager.setWebServerTask(webServerTask);
        Serial.println("   ✓ WebServerTask inyectado en StateManager");
        Serial.println("   ✓ Core: 1 (dedicado)");

        if (stateManager.getCurrentState() == HYDRO_ACTIVE_MODE ||
            stateManager.getCurrentState() == ADMIN_PANEL_MODE) {
            Serial.println("\n🔄 Recriando HydroSystemCore com WebServerTask válido...");
            if (stateManager.getCurrentState() == HYDRO_ACTIVE_MODE) {
                stateManager.switchToHydroActive();
            } else if (stateManager.getCurrentState() == ADMIN_PANEL_MODE) {
                stateManager.switchToAdminPanel();
            }
            Serial.println("✅ HydroSystemCore recriado com WebServerTask válido");
        }
    } else {
        Serial.println("❌ Error al inicializar WebServerTask");
        Serial.println("⚠️ Continuando sin Web Server...");
    }
    Serial.println();
#else
    Serial.println();
    Serial.println("ℹ️ FASE 3 omitida: admin HTTP :80 desativado (ENABLE_LOCAL_ADMIN_HTTP=0)");
    Serial.println();
#endif
    
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
    
    // ===== COMANDOS SERIAIS (Core 0 - loop principal) =====
#ifdef MASTER_MODE
    // ✅ ESP-NOW agora corre em task dedicada (Core 0)
    // Apenas comandos seriais no loop principal
    handleSerialCommands();             // Comandos do usuário - Core 0
    
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
    
    // ✅ CORREÇÃO: Delay não-bloqueante para permitir processamento de ESP-NOW
    // vTaskDelay não bloqueia callbacks ESP-NOW como delay() faz
    vTaskDelay(pdMS_TO_TICKS(10)); // 10ms - não bloqueia callbacks ESP-NOW
}

// ===== IMPLEMENTAÇÃO DAS FUNÇÕES MASTER =====
// Nota: handleRelayCommand já está implementada acima na linha 711

#ifdef MASTER_MODE
/**
 * @brief Task dedicada para ESP-NOW (Core 0)
 * Esta task processa toda a comunicação ESP-NOW de forma dedicada
 * e não bloqueante, permitindo que callbacks sejam executados corretamente
 */
void espNowTask(void* parameter) {
    Serial.println("🚀 ESP-NOW Task iniciada");
    Serial.printf("   📡 [Core %d] ESP-NOW Task rodando\n", xPortGetCoreID());
    
    // Variáveis para timing
    unsigned long lastDiscovery = 0;
    unsigned long lastStatsUpdate = 0;
    const unsigned long DISCOVERY_INTERVAL = 30000;      // 30 segundos
    const unsigned long STATS_UPDATE_INTERVAL = 120000;  // 2 minutos
    
    while (true) {
        // ✅ Processar ESP-NOW de forma dedicada
        if (masterManager) {
            masterManager->update();        // Sistema de retry, ACKs, status
        }
        
        if (master.isInitialized()) {
            master.update();                // ESPNowController (cleanup peers, discovery)
        }
        
        // Monitorar slaves periodicamente
        monitorSlaves();
        
        // ✅ Discovery automático (a cada 30s)
        unsigned long now = millis();
        if (now - lastDiscovery > DISCOVERY_INTERVAL) {
            if (masterManager) {
                Serial.println("\n🔍 [TASK] Discovery automático iniciado...");
                masterManager->rediscoverSlaves();
            }
            lastDiscovery = now;
        }
        
        // ✅ Estatísticas periódicas (a cada 2 minutos)
        if (now - lastStatsUpdate > STATS_UPDATE_INTERVAL) {
            if (masterManager) {
                Serial.printf("\n📊 [TASK] Slaves online: %d / %d\n", 
                             masterManager->getOnlineSlaveCount(),
                             masterManager->getTrustedSlaveCount());
            }
            lastStatsUpdate = now;
        }
        
        // ✅ Delay não-bloqueante para permitir processamento de callbacks
        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms - permite processamento de callbacks ESP-NOW
    }
}
#endif

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
    
    // ✅ BUSCAR SLAVE EN knownSlaves (estructura actualizada)
    uint8_t* slaveMac = findSlaveMac(slaveName);
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
    
    // ✅ USAR masterManager para enviar comando (en lugar de espNowTask)
    if (masterManager && masterManager->sendRelayCommandToSlave(slaveMac, relayNumber, action.c_str(), duration)) {
        Serial.println("✅ Comando enviado com sucesso");
    } else {
        Serial.println("❌ Erro ao enviar comando");
    }
}

// ===== FUNCIÓN PRINCIPAL DE COMANDOS SERIALES =====
// ✅ Esta función delega a las funciones específicas según el modo
void handleSerialCommands() {
#ifdef MASTER_MODE
    handleMasterSerialCommands();
#elif defined(SLAVE_MODE)
    handleSlaveSerialCommands();
#else
    handleGlobalSerialCommands();
#endif
}

#endif 