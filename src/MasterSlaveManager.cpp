#include "MasterSlaveManager.h"
#include "Config.h"
#include <WiFi.h>
#include "MASTER_CONFIG.h"
#include "ObjectPoolManager.h"  // ✅ Object Pool Pattern
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "ESPNowTypes.h"  // ✅ Para SlaveRelayStatesCache

// Instância estática para callbacks
MasterSlaveManager* MasterSlaveManager::instance = nullptr;

static String resolveSlaveEspAction(const String& action, const String& commandMode) {
    String modeLower = commandMode;
    modeLower.toLowerCase();
    if (modeLower == "timed_on" || modeLower == "timed_off" || modeLower == "cycle" ||
        modeLower == "cycle_stop") {
        return modeLower;
    }
    return action;
}

MasterSlaveManager::MasterSlaveManager(ESPNowController* espNowController) 
    : espNowController(espNowController), initialized(false),
      espnowLockWindowUntil(0),
      totalPingsReceived(0), totalPongsSent(0), totalAcksSent(0), 
      totalAcksReceived(0), totalErrors(0), commandIdCounter(0),
      processingStatusResponse(false), lastEspNowSendAt_(0) {
    instance = this;
    memset(lastEspNowSendMac_, 0, 6);
    
    allRelaysStatusSem = xSemaphoreCreateBinary();
    
    // ✅ PROTEÇÃO MULTI-CORE: Criar mutex para proteger trustedSlaves
    trustedSlavesMutex = xSemaphoreCreateMutex();
    if (trustedSlavesMutex == NULL) {
        Serial.println("❌ Erro ao criar mutex para trustedSlaves!");
    } else {
        Serial.println("✅ Mutex criado para proteção multi-core");
    }

    pendingRelayCommandsMutex = xSemaphoreCreateMutex();
    if (pendingRelayCommandsMutex == NULL) {
        Serial.println("❌ Erro ao criar mutex para pendingRelayCommands!");
    }
}

bool MasterSlaveManager::begin() {
    if (!espNowController || !espNowController->isInitialized()) {
        Serial.println("❌ ESPNowController não inicializado");
        return false;
    }
    
    Serial.println("\n🎯 ==========================================");
    Serial.println("🎯 INICIALIZANDO MASTER-SLAVE MANAGER");
    Serial.println("🎯 ==========================================");
    Serial.println("📡 Modo: MASTER (Receptor de PINGs)");
    Serial.println("🔧 Comunicação: BIDIRECIONAL");
    Serial.println("📋 Lista confiável: ATIVADA");
    Serial.println("✅ Sistema de ACKs: ATIVADO");
    Serial.println("==========================================");
    
    // Configurar callbacks do ESPNowController
    Serial.println("\n🔌 Configurando callbacks ESP-NOW...");
    
    // Callback para PING recebido
    espNowController->setPingCallback(onPingReceivedStatic);
    espNowController->setPongCallback(onPongReceivedStatic);
    
    // Callback para informações de dispositivo
    Serial.println("🔍 [DEBUG] Configurando deviceInfoCallback...");
    Serial.println("   📍 Endereço de onDeviceInfoReceivedStatic: " + String((uint32_t)onDeviceInfoReceivedStatic, HEX));
    espNowController->setDeviceInfoCallback(onDeviceInfoReceivedStatic);
    Serial.println("   ✅ deviceInfoCallback configurado");
    
    // Callback para status de relé
    espNowController->setRelayStatusCallback(onRelayStatusReceivedStatic);
    
    // Callback para erros
    espNowController->setErrorCallback(onErrorReceivedStatic);
    
    Serial.println("✅ Callbacks configurados");
    
    // Registrar callback para PONG (usar callback customizado)
    // O ESPNowController já responde automaticamente ao PING com PONG
    // Mas vamos interceptar para nossa lógica
    
    initialized = true;
    
    Serial.println("\n💾 Carregando peers confiáveis de NVS...");
    if (loadTrustedPeersFromNVS()) {
        Serial.println("✅ Peers confiáveis restaurados");
    } else {
        Serial.println("💡 Nenhum peer em cache (primeira inicialização)");
    }

    // ✅ NOVO: Carregar cache de estados de slaves de NVS
    Serial.println("\n💾 Carregando cache de estados de slaves de NVS...");
    if (loadSlaveRelayStatesFromNVS()) {
        Serial.println("✅ Cache carregado com sucesso");
        // ✅ Validar estados cacheados (solicitar atualização se necessário)
        validateCachedStates();
    } else {
        Serial.println("💡 Nenhum cache encontrado (primeira inicialização)");
    }
    
    Serial.println("\n📊 === INFORMAÇÕES DO MASTER ===");
    Serial.println("🆔 Nome: ESP-NOW-MASTER");
    Serial.println("🆔 Tipo: MasterController");
    Serial.println("🆔 MAC: " + espNowController->getLocalMacString());
    Serial.println("📶 Canal: " + String(espNowController->isInitialized() ? "Ativo" : "Inativo"));
    Serial.println("👥 Slaves confiáveis: 0");
    Serial.println("💾 Heap Livre: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("================================");
    
    Serial.println("\n✅ Master-Slave Manager inicializado com sucesso!");
    Serial.println("🎯 Aguardando PINGs dos Slaves...");
    Serial.println("📡 Comunicação bidirecional ativa!");
    Serial.println("==========================================\n");
    
    return true;
}

void MasterSlaveManager::update() {
    if (!initialized) return;
    
    // Verificar status dos Slaves periodicamente
    static unsigned long lastStatusCheck = 0;
    if (millis() - lastStatusCheck > 5000) {  // A cada 5 segundos
        checkSlaveStatus();
        lastStatusCheck = millis();
    }
    
    // Limpar ACKs expirados
    static unsigned long lastAckCleanup = 0;
    if (millis() - lastAckCleanup > 1000) {  // A cada 1 segundo
        cleanupExpiredAcks();
        lastAckCleanup = millis();
    }
    
    // Reenviar ACKs pendentes se necessário
    static unsigned long lastAckResend = 0;
    if (millis() - lastAckResend > 2000) {  // A cada 2 segundos
        resendPendingAcks();
        lastAckResend = millis();
    }
    
    // 🔄 FASE 1: Processar fila de retry de comandos
    processRetryQueue();
    
    // ✅ DESATIVADO: Processar comandos de slaves do Supabase
    // MOTIVO: A API direta /api/relay/slave funciona sem passar por Supabase
    // BENEFÍCIOS: 
    //   - Reduz uso de memória (evita erros SSL)
    //   - Resposta mais rápida (sem latência de Supabase)
    //   - Menos carga no servidor Supabase
    //   - Funciona mesmo com heap baixo
    // 
    // Se precisar reativar, descomente as linhas abaixo:
    // static unsigned long lastSlaveCommandCheck = 0;
    // if (millis() - lastSlaveCommandCheck > 3000) {
    //     processSlaveRelayCommands();
    //     lastSlaveCommandCheck = millis();
    // }
}

void MasterSlaveManager::end() {
    if (initialized) {
        Serial.println("📡 Master-Slave Manager finalizado");
        initialized = false;
    }
}

bool MasterSlaveManager::addTrustedSlave(const uint8_t* macAddress, const String& deviceName, const String& deviceType) {
    if (!initialized) return false;
    
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de modificar
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em addTrustedSlave()");
        return false;
    }
    
    // Verificar se Slave já existe
    for (auto& slave : trustedSlaves) {
        if (memcmp(slave.macAddress, macAddress, 6) == 0) {
            Serial.println("⚠️ Slave já existe na lista confiável: " + ESPNowController::macToString(macAddress));
            xSemaphoreGive(trustedSlavesMutex);  // Liberar antes de retornar
            return true;
        }
    }
    
    // Adicionar novo Slave
    TrustedSlave newSlave(macAddress);
    if (!deviceName.isEmpty()) {
        newSlave.deviceName = deviceName;
    }
    if (!deviceType.isEmpty()) {
        newSlave.deviceType = deviceType;
    }
    
    trustedSlaves.push_back(newSlave);
    
    Serial.printf("📊 [addTrustedSlave] Tamanho de trustedSlaves após push: %d\n", trustedSlaves.size());
    
    Serial.println("\n🎉 ========================================");
    Serial.println("🎉 SLAVE ADICIONADO À LISTA CONFIÁVEL!");
    Serial.println("🎉 ========================================");
    Serial.println("📥 MAC: " + ESPNowController::macToString(macAddress));
    Serial.println("📝 Nome: " + newSlave.deviceName);
    Serial.println("🏷️ Tipo: " + newSlave.deviceType);
    Serial.println("📊 Status: " + String((int)newSlave.status));
    Serial.println("⏰ Primeiro contato: " + String(newSlave.firstSeen / 1000) + "s");
    Serial.printf("📊 Total de slaves na lista: %d\n", trustedSlaves.size());
    Serial.println("========================================\n");
    
    // ✅ DEVICE_ID GERADO PARA USO NA API
    // O slave não precisa ser registrado no Supabase
    // Basta usar o device_id (ESP32_SLAVE_XX_XX_XX_XX_XX_XX) na tabela relay_commands
    String deviceId = "ESP32_SLAVE_" + ESPNowController::macToString(macAddress);
    deviceId.replace(":", "_");
    
    Serial.println("\n📡 ========================================");
    Serial.println("📡 SLAVE ADICIONADO - PRONTO PARA COMANDOS");
    Serial.println("📡 ========================================");
    Serial.println("🆔 Device ID: " + deviceId);
    Serial.println("💡 Use este device_id na tabela relay_commands");
    Serial.println("💡 O Master enviará comandos via ESP-NOW automaticamente");
    Serial.println("========================================\n");
    
    String callbackName = newSlave.deviceName;
    String callbackType = newSlave.deviceType;
    xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex ANTES de callbacks externos
    saveTrustedPeersToNVS();
    startEspNowLockWindow();
    
    // ⭐ POTENCIA MÁXIMA: Chamar callback SEMPRE que um slave é adicionado
    // Isso garante sincronização entre trustedSlaves e knownSlaves
    if (slaveDiscoveredCallback) {
        Serial.println("📢 Chamando callback de descoberta...");
        slaveDiscoveredCallback(macAddress, callbackName, callbackType);
        Serial.println("✅ Callback de descoberta chamado");
    } else {
        Serial.println("⚠️ AVISO: slaveDiscoveredCallback NÃO está configurado!");
        Serial.println("⚠️ O slave foi adicionado a trustedSlaves mas NÃO será adicionado a knownSlaves");
        Serial.println("⚠️ Isso pode causar problemas com comandos 'relay on_all'");
    }
    
    return true;
}

void MasterSlaveManager::startEspNowLockWindow() {
    espnowLockWindowUntil = millis() + ESPNOW_LOCK_WINDOW_MS;
#if ESPNOW_LOCK_DEBUG
    Serial.printf("[LOCK] window start %lums ch=%u lastRxAgeMs=%lu\n",
                  (unsigned long)ESPNOW_LOCK_WINDOW_MS,
                  (unsigned)WiFi.channel(),
                  getLastRxAgeMs());
#endif
}

bool MasterSlaveManager::isEspNowLockWindowActive() const {
    return espnowLockWindowUntil != 0 && millis() < espnowLockWindowUntil;
}

unsigned long MasterSlaveManager::getLastRxAgeMs() {
    unsigned long now = millis();
    unsigned long newest = 0;
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }
    for (const auto& slave : trustedSlaves) {
        if (slave.lastSeen > newest) {
            newest = slave.lastSeen;
        }
    }
    xSemaphoreGive(trustedSlavesMutex);
    if (newest == 0) {
        return 0;
    }
    return now - newest;
}

bool MasterSlaveManager::removeTrustedSlave(const uint8_t* macAddress) {
    if (!initialized) return false;
    
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de modificar
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em removeTrustedSlave()");
        return false;
    }
    
    for (auto it = trustedSlaves.begin(); it != trustedSlaves.end(); ++it) {
        if (memcmp(it->macAddress, macAddress, 6) == 0) {
            String deviceName = it->deviceName;
            trustedSlaves.erase(it);
            
            Serial.println("🗑️ Slave removido da lista confiável: " + ESPNowController::macToString(macAddress) + " (" + deviceName + ")");
            xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex antes de retornar
            return true;
        }
    }
    
    xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex antes de retornar
    return false;
}

// ✅ Função helper interna (sem mutex - para uso dentro de funciones que já têm mutex)
TrustedSlave* MasterSlaveManager::findTrustedSlaveUnsafe(const uint8_t* macAddress) {
    for (auto& slave : trustedSlaves) {
        if (memcmp(slave.macAddress, macAddress, 6) == 0) {
            return &slave;
        }
    }
    return nullptr;
}

TrustedSlave* MasterSlaveManager::getTrustedSlave(const uint8_t* macAddress) {
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        logSlaveLink("mutex_timeout", macAddress);
        return nullptr;
    }
    
    // Buscar slave
    TrustedSlave* found = findTrustedSlaveUnsafe(macAddress);
    
    // ⚠️ PROBLEMA: Retornar ponteiro con mutex bloqueado causa deadlocks
    // SOLUÇÃO: Siempre liberar mutex. El llamador debe tomar mutex si necesita modificar.
    // NOTA: Esto es seguro porque retornamos un puntero a un objeto en el vector,
    // pero el vector puede cambiar. El llamador debe usar el puntero inmediatamente
    // o tomar el mutex antes de usarlo.
    
    xSemaphoreGive(trustedSlavesMutex);  // ✅ SIEMPRE liberar mutex
    
    // ⚠️ ADVERTENCIA: El puntero retornado puede volverse inválido si el vector cambia
    // El llamador debe tomar el mutex antes de usar el puntero para modificar
    return found;
}

std::vector<TrustedSlave> MasterSlaveManager::getAllTrustedSlaves() {
    // Evitar OOM: cópia de vector falha quando heap fragmentado (espNowTask + SSL)
    static const uint32_t MIN_MAX_ALLOC_FOR_COPY = 8192;
    static const uint32_t MIN_FREE_HEAP_FOR_COPY = 45000;
    if (ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_FOR_COPY ||
        ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_COPY) {
        return std::vector<TrustedSlave>();
    }

    TickType_t startTime = xTaskGetTickCount();
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        std::vector<TrustedSlave> copy;
        copy.reserve(trustedSlaves.size());

        for (const auto& slave : trustedSlaves) {
            copy.push_back(slave);
        }

        xSemaphoreGive(trustedSlavesMutex);
        return copy;
    } else {
        TickType_t waitTime = xTaskGetTickCount() - startTime;
        Serial.printf("❌ [getAllTrustedSlaves] TIMEOUT após %lu ms!\n", waitTime * portTICK_PERIOD_MS);
        return std::vector<TrustedSlave>();
    }
}

void MasterSlaveManager::refreshEspNowPeersOnCurrentChannel() {
    if (!initialized || !espNowController) {
        return;
    }

    uint8_t channel = WiFi.channel();
    if (channel < 1 || channel > 13) {
        Serial.println("⚠️ Refresh peers: canal STA inválido");
        return;
    }

    Serial.printf("🔄 Re-add ESP-NOW peers no canal STA %u (sem deinit)\n", channel);

    uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    espNowController->removePeer(broadcastMac);
    espNowController->addPeerWithChannel(broadcastMac, channel, "broadcast");

    forEachTrustedSlave([this, channel](const TrustedSlave& slave) {
        espNowController->removePeer(slave.macAddress);
        espNowController->addPeerWithChannel(slave.macAddress, channel, slave.deviceName);
        Serial.printf("   peer %s → ch %u\n",
                      ESPNowController::macToString(slave.macAddress).c_str(), channel);
    });
}

bool MasterSlaveManager::forEachTrustedSlave(const std::function<void(const TrustedSlave&)>& visitor) {
    if (!visitor) {
        return false;
    }
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    for (const auto& slave : trustedSlaves) {
        visitor(slave);
    }
    xSemaphoreGive(trustedSlavesMutex);
    return true;
}

std::vector<TrustedSlave> MasterSlaveManager::getOnlineSlaves() {
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de ler
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        std::vector<TrustedSlave> onlineSlaves;
        for (const auto& slave : trustedSlaves) {
            if (slave.isOnline()) {
                onlineSlaves.push_back(slave);
            }
        }
        xSemaphoreGive(trustedSlavesMutex);  // Liberar mutex
        return onlineSlaves;
    } else {
        Serial.println("⚠️ Timeout ao obter mutex em getOnlineSlaves()");
        return std::vector<TrustedSlave>();  // Retornar vazio se timeout
    }
}

int MasterSlaveManager::getTrustedSlaveCount() {
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de ler
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        int count = trustedSlaves.size();
        xSemaphoreGive(trustedSlavesMutex);  // Liberar mutex
        return count;
    } else {
        Serial.println("⚠️ Timeout ao obter mutex em getTrustedSlaveCount()");
        return 0;  // Retornar 0 se timeout
    }
}

int MasterSlaveManager::getOnlineSlaveCount() {
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de ler
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        int count = 0;
        for (const auto& slave : trustedSlaves) {
            if (slave.isOnline()) {
                count++;
            }
        }
        xSemaphoreGive(trustedSlavesMutex);  // Liberar mutex
        return count;
    } else {
        Serial.println("⚠️ Timeout ao obter mutex em getOnlineSlaveCount()");
        return 0;  // Retornar 0 se timeout
    }
}

bool MasterSlaveManager::sendPingToSlave(const uint8_t* macAddress) {
    if (!initialized || !espNowController) return false;
    
    TrustedSlave* slave = getTrustedSlave(macAddress);
    if (!slave) {
        Serial.println("❌ Slave não encontrado na lista confiável: " + ESPNowController::macToString(macAddress));
        return false;
    }
    
    bool success = espNowController->sendPing(macAddress);
    if (success) {
        slave->pingsSent++;  // ✅ CORREÇÃO: Incrementar PINGs enviados
        slave->lastPongId++;
        Serial.println("🏓 PING enviado para Slave: " + ESPNowController::macToString(macAddress));
    } else {
        slave->messagesLost++;
        Serial.println("❌ Falha ao enviar PING para Slave: " + ESPNowController::macToString(macAddress));
    }
    
    return success;
}

int MasterSlaveManager::sendPingToAllSlaves() {
    if (!initialized) return 0;
    
    int pingsSent = 0;
    for (const auto& slave : trustedSlaves) {
        if (slave.isOnline()) {
            if (sendPingToSlave(slave.macAddress)) {
                pingsSent++;
            }
        }
    }
    
    Serial.println("📡 PINGs enviados para " + String(pingsSent) + " Slaves online");
    return pingsSent;
}

uint32_t MasterSlaveManager::sendRelayCommandToSlave(const uint8_t* macAddress, int relayNumber,
                                                     const String& action, int duration,
                                                     int supabaseCommandId, bool updateStatus,
                                                     int cycleOffDuration, const String& commandMode) {
    // ✅ MUDANÇA 1: Retornar 0 em vez de false (compatível: 0 = false, >0 = true)
    if (!initialized || !espNowController) return 0;
    
    TrustedSlave* slave = getTrustedSlave(macAddress);
    if (!slave) {
        Serial.println("❌ Slave não encontrado na lista confiável: " + ESPNowController::macToString(macAddress));
        return 0;  // ✅ MUDANÇA 2: 0 em vez de false
    }
    
    // Verificar se slave está inalcançável (sem contato radio recente)
    if (!isSlaveReachable(*slave)) {
        logSlaveLink("cmd_blocked_offline", macAddress);
        Serial.println("\n⏸️ ========================================");
        Serial.println("⏸️ SLAVE OFFLINE - COMANDO P");
        Serial.println("⏸️ ========================================");
        Serial.println("📡 Destino: " + ESPNowController::macToString(macAddress));
        Serial.println("📝 Nome: " + slave->deviceName);
        Serial.println("🔌 Relé: " + String(relayNumber));
        Serial.println("⚡ Ação: " + action);
        Serial.println("💾 Comando será enviado quando slave voltar ONLINE");
        Serial.println("========================================\n");
        
        uint32_t commandId = generateCommandId();
        addToRetryQueue(macAddress, relayNumber, action, duration, commandId, supabaseCommandId, false,
                        cycleOffDuration, commandMode);
        
        return commandId;
    }

    if (hasInFlightForMac(macAddress)) {
        uint32_t commandId = generateCommandId();
        addToRetryQueue(macAddress, relayNumber, action, duration, commandId, supabaseCommandId, false,
                        cycleOffDuration, commandMode);
        return commandId;
    }

    if (!canEspNowSendToMac(macAddress)) {
        uint32_t commandId = generateCommandId();
        addToRetryQueue(macAddress, relayNumber, action, duration, commandId, supabaseCommandId, false,
                        cycleOffDuration, commandMode);
        return commandId;
    }
    
    // 🔄 FASE 1: Gerar ID único para este comando
    uint32_t commandId = generateCommandId();
    
    Serial.println("\n📤 ========================================");
    Serial.println("📤 ENVIANDO COMANDO DE RELÉ");
    Serial.println("📤 ========================================");
    Serial.println("🆔 Command ID: " + String(commandId));
    Serial.println("📡 Destino: " + ESPNowController::macToString(macAddress));
    Serial.println("🔌 Relé: " + String(relayNumber));
    Serial.println("⚡ Ação: " + action);
    if (duration > 0) {
        Serial.println("⏱️ Duração: " + String(duration) + "s");
    }
    if (cycleOffDuration > 0) {
        Serial.println("⏱️ Ciclo OFF: " + String(cycleOffDuration) + "s");
    }
    if (commandMode.length() > 0 && commandMode != "instant") {
        Serial.println("🎛️ Modo: " + commandMode);
    }
    
    String espAction = resolveSlaveEspAction(action, commandMode);

    // Tentar enviar o comando
    bool success = espNowController->sendRelayCommand(macAddress, relayNumber, espAction, duration,
                                                      commandId, cycleOffDuration, commandMode);
    
    if (success) {
        markEspNowSendToMac(macAddress);
        Serial.println("✅ Comando enviado com sucesso!");
        Serial.println("========================================\n");
        
        // ✅ ATUALIZAR STATUS APÓS COMANDO (OTIMIZADO)
        // Solo actualizar si updateStatus es true (evitar en operaciones en lote como on_all)
        if (updateStatus) {
            // 1. Primeiro atualiza o slave afetado (mais rápido)
            // 2. Depois atualiza todos os outros (para manter sincronização)
            // 3. Delay pequeno para dar tempo do comando ser processado
            delay(200); // 200ms para o slave processar o comando
            
            Serial.println("🔄 Atualizando status dos relés...");
            
            // Prioridade 1: Atualizar o slave que recebeu o comando
            Serial.println("📡 Atualizando slave afetado primeiro...");
            requestSlaveStatus(macAddress);
            
            delay(100); // Pequeno delay entre atualizações
            
            // Prioridade 2: Atualizar todos os outros slaves (manter sincronização geral)
            requestAllSlavesRelayStatus();
        } else {
            // En operaciones en lote, solo un pequeño delay para permitir procesamiento
            delay(50); // Delay mínimo para permitir processamento
        }
        
        // ✅ MUDANÇA 4: Retornar commandId em vez de success
        if (supabaseCommandId > 0) {
            addToRetryQueue(macAddress, relayNumber, action, duration, commandId, supabaseCommandId, true);
        }
        return commandId;
    } else {
        Serial.println("❌ Falha ao enviar comando!");
        Serial.println("🔄 Adicionando à fila de retry...");
        Serial.println("========================================\n");
        
        slave->messagesLost++;
        
        // 🔄 FASE 1: Adicionar à fila de retry automático
        addToRetryQueue(macAddress, relayNumber, action, duration, commandId, supabaseCommandId);
        
        // ✅ MUDANÇA 5: Retornar commandId mesmo se falhou (para mapeamento futuro)
        return commandId;  // Retorna ID mesmo se falhou (pode ser enviado depois via retry)
    }
}

uint32_t MasterSlaveManager::sendRelayMaskToSlave(const uint8_t* macAddress, uint8_t mask,
                                                   int durationSec, int supabaseCommandId) {
    if (!initialized || !espNowController || !macAddress) {
        return 0;
    }
    const String action = (mask == 0) ? "off_all" : "on_all";
    TrustedSlave* slave = getTrustedSlave(macAddress);
    if (!slave) {
        return 0;
    }
    uint32_t commandId = generateCommandId();
    if (!isSlaveReachable(*slave) || hasInFlightForMac(macAddress) || !canEspNowSendToMac(macAddress)) {
        addToRetryQueue(macAddress, 255, action, durationSec, commandId, supabaseCommandId, false);
        return commandId;
    }
    Serial.printf("[PROC] SET_RELAY_MASK id=%u mask=0x%02X\n", (unsigned)commandId, mask);
    bool success = espNowController->sendSetRelayMask(macAddress, mask, (uint16_t)durationSec, commandId);
    if (success) {
        markEspNowSendToMac(macAddress);
        addToRetryQueue(macAddress, 255, action, durationSec, commandId, supabaseCommandId, true);
        return commandId;
    }
    addToRetryQueue(macAddress, 255, action, durationSec, commandId, supabaseCommandId, false);
    return commandId;
}

int MasterSlaveManager::applyRelayMaskToAllOnlineSlaves(uint8_t mask) {
    int sent = 0;
    auto slaves = getAllTrustedSlaves();
    for (const auto& slave : slaves) {
        if (!slave.isOnline()) {
            continue;
        }
        if (sendRelayMaskToSlave(slave.macAddress, mask) > 0) {
            sent++;
            Serial.printf("[PROC] mask=0x%02X -> %s\n", mask, slave.deviceName.c_str());
        }
    }
    return sent;
}

bool MasterSlaveManager::requestSlaveStatus(const uint8_t* macAddress) {
    if (!initialized || !espNowController) return false;
    
    // ✅ Proteção contra loop infinito: não solicitar status se já está processando uma resposta
    if (processingStatusResponse) {
        Serial.println("⚠️ Ignorando solicitação de status - já processando resposta de status");
        return false;
    }
    
    // Um único comando "status" dispara ALL_RELAYS_STATUS no Slave (8 relés num pacote)
    TrustedSlave* slave = getTrustedSlave(macAddress);
    if (!slave) {
        return false;
    }

    bool success = espNowController->sendRelayCommand(macAddress, 0, "status", 0, 0);

    if (success) {
        LOG_ESPNOW_INFO("📊 ALL_RELAYS_STATUS solicitado: " + ESPNowController::macToString(macAddress));
    }

    return success;
}

void MasterSlaveManager::requestAllSlavesRelayStatus() {
    if (!initialized || !espNowController) return;
    
    // ✅ Proteção contra loop infinito: não solicitar status se já está processando uma resposta
    if (processingStatusResponse) {
        Serial.println("⚠️ Ignorando solicitação de status global - já processando resposta de status");
        return;
    }
    
    // Contar slaves online primeiro (evitar logs desnecessários se não houver nenhum)
    int onlineCount = 0;
    for (const auto& slave : trustedSlaves) {
        if (slave.isOnline()) onlineCount++;
    }
    
    if (onlineCount == 0) {
        Serial.println("⏸️ Nenhum slave online - pulando atualização de status");
        return;
    }
    
    Serial.println("\n🔄 ========================================");
    Serial.println("🔄 ATUALIZANDO STATUS DE TODOS OS SLAVES");
    Serial.println("🔄 ========================================");
    
    int totalSlaves = trustedSlaves.size();
    int onlineSlaves = 0;
    int totalRelays = 0;
    int requestsSent = 0;
    int requestsFailed = 0;
    
    for (auto& slave : trustedSlaves) {
        // Apenas solicitar status de slaves online
        if (!slave.isOnline()) {
            continue; // Não logar offline para não poluir logs
        }
        
        onlineSlaves++;
        Serial.println("📡 " + slave.deviceName + " (" + ESPNowController::macToString(slave.macAddress) + ")");
        
        if (requestSlaveStatus(slave.macAddress)) {
            requestsSent++;
            totalRelays += slave.numRelays;
            Serial.println("   ✅ ALL_RELAYS_STATUS solicitado");
        } else {
            requestsFailed++;
            Serial.println("   ⚠️ Falha ao solicitar status");
        }
        
        delay(20); // Delay entre slaves (20ms)
    }
    
    // Resumo compacto
    Serial.println("\n📊 " + String(onlineSlaves) + "/" + String(totalSlaves) + " slaves | " + 
                   String(totalRelays) + " relés | ✅" + String(requestsSent) + 
                   (requestsFailed > 0 ? " ❌" + String(requestsFailed) : ""));
    Serial.println("========================================\n");
}

bool MasterSlaveManager::requestSlaveInfo(const uint8_t* macAddress) {
    if (!initialized || !espNowController) return false;
    
    // ⭐ POTENCIA MÁXIMA: Usar handshake para solicitar DEVICE_INFO del slave
    // El handshake hace que el slave responda con su información completa
    Serial.println("\n📋 === SOLICITANDO DEVICE_INFO DEL SLAVE ===");
    Serial.println("📡 MAC: " + ESPNowController::macToString(macAddress));
    Serial.println("🎯 Usando handshake para obtener información completa");
    
    bool success = espNowController->initiateHandshake(macAddress);
    
    if (success) {
        Serial.println("✅ Handshake enviado - Slave debería responder con DEVICE_INFO");
        Serial.println("==========================================\n");
    } else {
        Serial.println("❌ Fallo al enviar handshake");
        Serial.println("==========================================\n");
    }
    
    return success;
}

void MasterSlaveManager::setProcessingStatusResponse(bool processing) {
    processingStatusResponse = processing;
}

void MasterSlaveManager::drainAllRelaysStatusWait() {
    if (allRelaysStatusSem != nullptr) {
        xSemaphoreTake(allRelaysStatusSem, 0);
    }
}

bool MasterSlaveManager::waitForAllRelaysStatus(uint32_t timeoutMs) {
    if (allRelaysStatusSem == nullptr) {
        return false;
    }
    return xSemaphoreTake(allRelaysStatusSem, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void MasterSlaveManager::notifyAllRelaysStatusReceived(const uint8_t* senderMac,
                                                       const bool relayStates[8],
                                                       uint8_t numRelays) {
    if (allRelaysStatusSem != nullptr) {
        xSemaphoreGive(allRelaysStatusSem);
    }
    if (senderMac) {
        touchSlaveLink(senderMac, "all_relays_rx");
    }
    if (allRelaysSnapshotCallback && senderMac && relayStates && numRelays > 0) {
        allRelaysSnapshotCallback(senderMac, relayStates, numRelays);
    }
}

bool MasterSlaveManager::sendAck(const uint8_t* macAddress, uint32_t messageId) {
    if (!initialized || !espNowController) return false;
    
    // Criar mensagem ACK
    ESPNowMessage ackMessage = {};
    ackMessage.type = MessageType::ACK;
    espNowController->getLocalMac(ackMessage.senderId);
    memcpy(ackMessage.targetId, macAddress, 6);
    ackMessage.messageId = ++totalAcksSent;
    ackMessage.timestamp = millis();
    
    // Incluir ID da mensagem confirmada nos dados
    ackMessage.dataSize = sizeof(uint32_t);
    memcpy(ackMessage.data, &messageId, sizeof(uint32_t));
    ackMessage.checksum = espNowController->calculateChecksum(ackMessage);
    
    bool success = espNowController->sendMessage(ackMessage, macAddress);
    if (success) {
        Serial.println("✅ ACK enviado para " + ESPNowController::macToString(macAddress) + 
                      " (MsgID: " + String(messageId) + ")");
    }
    
    return success;
}

bool MasterSlaveManager::isWaitingForAck(const uint8_t* macAddress, uint32_t messageId) {
    for (const auto& pending : pendingAcks) {
        if (memcmp(pending.macAddress, macAddress, 6) == 0 && pending.messageId == messageId) {
            return true;
        }
    }
    return false;
}

void MasterSlaveManager::markMessageAcknowledged(const uint8_t* macAddress, uint32_t messageId) {
    for (auto it = pendingAcks.begin(); it != pendingAcks.end(); ++it) {
        if (memcmp(it->macAddress, macAddress, 6) == 0 && it->messageId == messageId) {
            pendingAcks.erase(it);
            Serial.println("✅ Mensagem confirmada: " + ESPNowController::macToString(macAddress) + 
                          " (MsgID: " + String(messageId) + ")");
            break;
        }
    }
}

// ===== CALLBACKS =====

void MasterSlaveManager::setSlaveDiscoveredCallback(std::function<void(const uint8_t* macAddress, const String& deviceName, const String& deviceType)> callback) {
    this->slaveDiscoveredCallback = callback;
}

void MasterSlaveManager::setSlaveOnlineCallback(std::function<void(const uint8_t* macAddress, const String& deviceName)> callback) {
    this->slaveOnlineCallback = callback;
}

void MasterSlaveManager::setSlaveOfflineCallback(std::function<void(const uint8_t* macAddress, const String& deviceName)> callback) {
    this->slaveOfflineCallback = callback;
}

void MasterSlaveManager::setPingReceivedCallback(std::function<void(const uint8_t* macAddress, uint32_t pingId)> callback) {
    this->pingReceivedCallback = callback;
}

void MasterSlaveManager::setPongReceivedCallback(std::function<void(const uint8_t* macAddress, uint32_t pongId)> callback) {
    this->pongReceivedCallback = callback;
}

void MasterSlaveManager::setRelayStatusCallback(std::function<void(const uint8_t* macAddress, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name)> callback) {
    this->relayStatusCallback = callback;
}

void MasterSlaveManager::setDeviceInfoCallback(std::function<void(const uint8_t* macAddress, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel)> callback) {
    this->deviceInfoCallback = callback;
}

void MasterSlaveManager::setAckReceivedCallback(std::function<void(const uint8_t* macAddress, uint32_t messageId)> callback) {
    this->ackReceivedCallback = callback;
}

void MasterSlaveManager::setErrorCallback(std::function<void(const uint8_t* macAddress, const String& error)> callback) {
    this->errorCallback = callback;
}

// 🔄 FASE 2: Callback para ACK de relay
void MasterSlaveManager::setRelayAckCallback(std::function<void(const uint8_t* macAddress, uint32_t commandId, bool success, uint8_t relayNumber, uint8_t currentState)> callback) {
    this->relayAckCallback = callback;
}

void MasterSlaveManager::setSupabaseCommandCallback(std::function<void(int supabaseCommandId, bool success, const String& errorMessage)> callback) {
    this->supabaseCommandCallback = callback;
}

void MasterSlaveManager::setSlaveCommandResolvedCallback(
    std::function<void(int supabaseCommandId, uint32_t espNowCommandId, const uint8_t* mac,
                       int relayNumber, bool currentState)> callback) {
    this->slaveCommandResolvedCallback = callback;
}

void MasterSlaveManager::setSupabaseRelayStateCallback(std::function<void(const String& masterDeviceId, const String& slaveMacAddress, const String& slaveDeviceId, int relayNumber, bool state, bool hasTimer, int remainingTime)> callback) {
    this->supabaseRelayStateCallback = callback;
}

// ===== SLAVE-LINK: vínculo radio + logging correlacionável =====

void MasterSlaveManager::logSlaveLink(const char* event, const uint8_t* mac, long lastSeenDeltaMs) {
    if (!event) return;
    String macBuf = mac ? ESPNowController::macToString(mac) : String("unknown");
    Serial.printf("[SLAVE-LINK] event=%s mac=%s queue=%u heap=%u",
                  event,
                  macBuf.c_str(),
                  (unsigned)getPendingRelayCommandCount(),
                  (unsigned)ESP.getFreeHeap());
    if (lastSeenDeltaMs >= 0) {
        Serial.printf(" last_seen_delta_ms=%ld", lastSeenDeltaMs);
    }
    Serial.println();
}

bool MasterSlaveManager::isSlaveReachable(const TrustedSlave& slave) const {
    if (slave.isOnline()) {
        return true;
    }
    if (slave.lastSeen == 0) {
        return false;
    }
    return (millis() - slave.lastSeen) < SLAVE_REACHABLE_MS;
}

bool MasterSlaveManager::readSlaveRelaySnapshot(const uint8_t* macAddress, bool states[8],
                                              bool timers[8], int remaining[8], uint8_t& numRelays,
                                              bool& linkOnline, uint16_t& linkLastSeenS) {
    if (!macAddress || !states || !timers || !remaining) {
        return false;
    }
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    TrustedSlave* slave = findTrustedSlaveUnsafe(macAddress);
    if (!slave) {
        xSemaphoreGive(trustedSlavesMutex);
        return false;
    }
    numRelays = slave->numRelays > 8 ? 8 : slave->numRelays;
    for (uint8_t i = 0; i < numRelays; i++) {
        states[i] = slave->relayStates[i].state;
        timers[i] = slave->relayStates[i].hasTimer;
        remaining[i] = slave->relayStates[i].remainingTime;
    }
    const unsigned long sinceSeen = slave->getTimeSinceLastSeen();
    linkOnline = slave->isOnline() && slave->lastSeen != 0 && sinceSeen < 90000UL;
    linkLastSeenS = (uint16_t)(sinceSeen / 1000UL);
    xSemaphoreGive(trustedSlavesMutex);
    return true;
}

bool MasterSlaveManager::hasInFlightForMacLocked(const uint8_t* mac) const {
    if (!mac) return false;
    for (const auto& cmd : pendingRelayCommands) {
        if (cmd.waitingForAck && memcmp(cmd.targetMac, mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

bool MasterSlaveManager::hasInFlightForMac(const uint8_t* mac) const {
    if (!mac) return false;
    if (!lockPendingQueue(pdMS_TO_TICKS(100))) {
        return false;
    }
    const bool inFlight = hasInFlightForMacLocked(mac);
    unlockPendingQueue();
    return inFlight;
}

bool MasterSlaveManager::canEspNowSendToMac(const uint8_t* mac) const {
    if (!mac) return false;
    if (memcmp(lastEspNowSendMac_, mac, 6) != 0) {
        return true;
    }
    return (millis() - lastEspNowSendAt_) >= MIN_ESPNOW_SEND_GAP_MS;
}

void MasterSlaveManager::markEspNowSendToMac(const uint8_t* mac) {
    if (!mac) return;
    memcpy(lastEspNowSendMac_, mac, 6);
    lastEspNowSendAt_ = millis();
}

void MasterSlaveManager::notifyEspNowSendFail(const uint8_t* mac, uint8_t consecutiveFails) {
    if (!mac || consecutiveFails < 3) return;
    Serial.printf("[SLAVE-LINK] event=espnow_send_fail mac=%s fails=%u heap=%u\n",
                  ESPNowController::macToString(mac).c_str(),
                  (unsigned)consecutiveFails,
                  (unsigned)ESP.getFreeHeap());
}

void MasterSlaveManager::touchSlaveLink(const uint8_t* mac, const char* reason) {
    if (!mac || !reason) return;

    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        logSlaveLink("mutex_timeout", mac);
        return;
    }

    TrustedSlave* slave = findTrustedSlaveUnsafe(mac);
    if (!slave) {
        xSemaphoreGive(trustedSlavesMutex);
        return;
    }

    bool wasOffline = !slave->isOnline();
    unsigned long prevLastSeen = slave->lastSeen;
    slave->updateLastSeen();
    slave->messagesReceived++;
    slave->status = SlaveStatus::ONLINE;
    String deviceName = slave->deviceName;
    xSemaphoreGive(trustedSlavesMutex);

    long delta = (prevLastSeen > 0) ? (long)(millis() - prevLastSeen) : 0;
    const char* logEvent = wasOffline ? "online_mark" : reason;
    logSlaveLink(logEvent, mac, delta);

    if (wasOffline) {
        sendPendingCommandsToSlave(mac);
        requestSlaveStatus(mac);
        if (slaveOnlineCallback) {
            slaveOnlineCallback(mac, deviceName);
        }
    }
}

// ===== MÉTODOS PRIVADOS =====

void MasterSlaveManager::processPingReceived(const uint8_t* senderMac, uint32_t pingId) {
    Serial.printf("[ESPNOW] PING rx %s id=%u\n",
                  ESPNowController::macToString(senderMac).c_str(), pingId);
    LOG_ESPNOW_DEBUG("\n🏓 PING RECEBIDO DO SLAVE: " + ESPNowController::macToString(senderMac));
    
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de verificar/adicionar
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em processPingReceived()");
        return;
    }
    
    // Verificar se Slave está na lista confiável (usar versión unsafe porque ya tenemos mutex)
    TrustedSlave* slave = findTrustedSlaveUnsafe(senderMac);
    
    if (!slave) {
        xSemaphoreGive(trustedSlavesMutex);  // Liberar antes de addTrustedSlave (que toma mutex)
        
        Serial.println("\n🆕 SLAVE DESCONHECIDO - ADICIONANDO À LISTA CONFIÁVEL");
        String genericName = "Slave-" + ESPNowController::macToString(senderMac).substring(12);
        addTrustedSlave(senderMac, genericName, "RelayBox");
        
        // Tomar mutex de novo para buscar
        if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            Serial.println("⚠️ Timeout ao obter mutex após addTrustedSlave()");
            return;
        }
        slave = findTrustedSlaveUnsafe(senderMac);
        
        // ⭐ POTENCIA MÁXIMA: Solicitar DEVICE_INFO automáticamente para obtener información completa
        if (slave) {
            xSemaphoreGive(trustedSlavesMutex);
            
            Serial.println("\n📋 === SOLICITANDO DEVICE_INFO AUTOMÁTICAMENTE ===");
            Serial.println("🎯 Usando DEVICE_INFO como fuente principal de información");
            if (espNowController) {
                espNowController->initiateHandshake(senderMac);
                Serial.println("✅ Handshake iniciado - Slave debería responder con DEVICE_INFO");
            }
            Serial.println("==================================================\n");
            
            if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
                Serial.println("⚠️ Timeout ao obter mutex após handshake em processPingReceived()");
                return;
            }
            slave = findTrustedSlaveUnsafe(senderMac);
        }
    } else if (slave->deviceName.startsWith("Slave-") || slave->deviceName == "Unknown") {
        xSemaphoreGive(trustedSlavesMutex);
        
        Serial.println("\n📋 === SLAVE CON INFORMACIÓN INCOMPLETA ===");
        Serial.println("🔄 Solicitando DEVICE_INFO para actualizar información...");
        if (espNowController) {
            espNowController->initiateHandshake(senderMac);
            Serial.println("✅ Handshake iniciado para obtener información completa");
        }
        Serial.println("============================================\n");
        
        if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            Serial.println("⚠️ Timeout ao obter mutex após handshake em processPingReceived()");
            return;
        }
        slave = findTrustedSlaveUnsafe(senderMac);
    }
    
    String deviceNameForCallback;
    bool shouldCallOnlineCallback = false;
    
    if (slave) {
        // Atualizar informações do Slave
        slave->updateLastSeen();
        slave->lastPingId = pingId;
        slave->pingsReceived++;
        slave->messagesReceived++;
        
        // Atualizar status
        if (slave->status == SlaveStatus::UNKNOWN || slave->status == SlaveStatus::DISCOVERED) {
            slave->status = SlaveStatus::PING_RECEIVED;
            Serial.println("📊 Status atualizado: PING_RECEIVED");
        }
        
        deviceNameForCallback = slave->deviceName;
        
        xSemaphoreGive(trustedSlavesMutex);
        
        // 🚨 CRÍTICO: Verificar se o peer existe e se o canal está sincronizado antes de enviar PONG
        if (espNowController && espNowController->peerExists(senderMac)) {
            uint8_t currentMasterChannel;
            wifi_second_chan_t secondChan;
            esp_wifi_get_channel(&currentMasterChannel, &secondChan);
            
            esp_now_peer_info_t peerInfo;
            if (esp_now_get_peer(senderMac, &peerInfo) == ESP_OK) {
                if (peerInfo.channel != currentMasterChannel && currentMasterChannel > 0 && currentMasterChannel <= 13) {
                    Serial.println("\n⚠️ Canal do peer desatualizado antes de enviar PONG!");
                    Serial.println("   📶 Canal do peer: " + String(peerInfo.channel));
                    Serial.println("   📶 Canal atual do Master: " + String(currentMasterChannel));
                    Serial.println("   🔄 Atualizando peer...");
                    
                    esp_now_del_peer(senderMac);
                    String slaveName = deviceNameForCallback.isEmpty()
                        ? "Slave-" + ESPNowController::macToString(senderMac).substring(12)
                        : deviceNameForCallback;
                    espNowController->addPeerWithChannel(senderMac, currentMasterChannel, slaveName);
                }
            }
        }
        
        // Responder automaticamente com PONG
        LOG_ESPNOW_DEBUG("[ESPNOW] PONG tx " + ESPNowController::macToString(senderMac));
        bool pongSent = espNowController->sendPing(senderMac);
        
        if (pongSent) {
            if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                slave = findTrustedSlaveUnsafe(senderMac);
                if (slave) {
                    slave->pongsSent++;
                    slave->lastPongId++;
                    if (slave->status == SlaveStatus::PING_RECEIVED || slave->status == SlaveStatus::DISCOVERED) {
                        slave->status = SlaveStatus::ONLINE;
                        slave->updateLastSeen();
                        shouldCallOnlineCallback = true;
                        deviceNameForCallback = slave->deviceName;
                        Serial.println("[ESPNOW] slave ONLINE");
                    }
                }
                xSemaphoreGive(trustedSlavesMutex);
            }
            
            if (shouldCallOnlineCallback && slaveOnlineCallback) {
                slaveOnlineCallback(senderMac, deviceNameForCallback);
            }
        } else {
            if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                slave = findTrustedSlaveUnsafe(senderMac);
                if (slave) {
                    slave->messagesLost++;
                }
                xSemaphoreGive(trustedSlavesMutex);
            }
            Serial.println("❌ Falha ao enviar PONG");
        }
        
#if DEBUG_ESPNOW && MASTER_DEBUG_LEVEL >= DEBUG_LEVEL_DEBUG
        if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            slave = findTrustedSlaveUnsafe(senderMac);
            if (slave) {
                Serial.printf("[ESPNOW] stats ping_rx=%u pong_tx=%u msg_rx=%u\n",
                              slave->pingsReceived, slave->pongsSent, slave->messagesReceived);
            }
            xSemaphoreGive(trustedSlavesMutex);
        }
#endif
    } else {
        xSemaphoreGive(trustedSlavesMutex);
    }
    
    // Chamar callback se definido
    if (pingReceivedCallback) {
        pingReceivedCallback(senderMac, pingId);
    }

    touchSlaveLink(senderMac, "ping_rx");
}

void MasterSlaveManager::processPongReceived(const uint8_t* senderMac, uint32_t pongId) {
    Serial.printf("[ESPNOW] PONG rx %s id=%u\n",
                  ESPNowController::macToString(senderMac).c_str(), pongId);

    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        TrustedSlave* slave = findTrustedSlaveUnsafe(senderMac);
        if (slave) {
            slave->pongsReceived++;
        }
        xSemaphoreGive(trustedSlavesMutex);
    }

    touchSlaveLink(senderMac, "pong_rx");

    if (pongReceivedCallback) {
        pongReceivedCallback(senderMac, pongId);
    }
}

void MasterSlaveManager::processAckReceived(const uint8_t* senderMac, uint32_t messageId) {
    // ✅ REDUZIDO: Log removido (ACKs são frequentes)
    // Serial.println("✅ ACK recebido de: " + ESPNowController::macToString(senderMac) + " (MsgID: " + String(messageId) + ")");
    
    // Marcar mensagem como confirmada
    markMessageAcknowledged(senderMac, messageId);
    totalAcksReceived++;
    
    // Chamar callback se definido
    if (ackReceivedCallback) {
        ackReceivedCallback(senderMac, messageId);
    }
}

void MasterSlaveManager::processDeviceInfoReceived(const uint8_t* senderMac, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel) {
    const bool knownSlave = (getTrustedSlave(senderMac) != nullptr);

    if (knownSlave) {
        LOG_ESPNOW_DEBUG("[ESPNOW] DEVICE_INFO " + ESPNowController::macToString(senderMac) +
                         " " + deviceName + " ch=" + String(wifiChannel));
    } else {
        Serial.println("\n[ESPNOW] DEVICE_INFO novo " + ESPNowController::macToString(senderMac));
        Serial.println("   nome=" + deviceName + " tipo=" + deviceType + " relés=" + String(numRelays));
    }
    
    wifi_mode_t wifiMode = WiFi.getMode();
    (void)wifiMode;
    bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    uint8_t masterChannel;
    wifi_second_chan_t secondChan;
    esp_wifi_get_channel(&masterChannel, &secondChan);
    if (!knownSlave) {
        Serial.println("   canal master=" + String(masterChannel) + " slave=" + String(wifiChannel));
    }
    if (wifiChannel > 0 && masterChannel != wifiChannel && !knownSlave) {
        Serial.println("   ⚠️ CONFLITO DE CANAL DETECTADO!");
        Serial.println("   💡 Master no canal " + String(masterChannel) + ", Slave no canal " + String(wifiChannel));
        if (wifiConnected) {
            Serial.println("   ⚠️ WiFi conectado - Master NÃO pode mudar de canal");
            Serial.println("   💡 Slave deve sincronizar para canal " + String(masterChannel));
        }
    }
    
    // ⭐ CORREÇÃO CRÍTICA: Adicionar Slave como peer ESP-NOW se não existir
    // Master com WiFi STA: peer sempre no canal RF do Master
    uint8_t peerChannel = wifiChannel;
    if (peerChannel == 0) {
        uint8_t currentChannel;
        wifi_second_chan_t secondChan;
        esp_wifi_get_channel(&currentChannel, &secondChan);
        peerChannel = currentChannel;
        Serial.println("⚠️ Canal WiFi não fornecido, usando canal detectado: " + String(peerChannel));
    }
    if (wifiConnected && masterChannel > 0) {
        if (peerChannel != masterChannel) {
            Serial.println("   💡 Peer forçado ao canal Master " + String(masterChannel) + " (Slave reportou " + String(wifiChannel) + ")");
        }
        peerChannel = masterChannel;
    }
    
    if (espNowController && !espNowController->peerExists(senderMac)) {
        Serial.println("\n🔗 === ADICIONANDO SLAVE COMO PEER ESP-NOW ===");
        Serial.println("   MAC: " + ESPNowController::macToString(senderMac));
        Serial.println("   Nome: " + deviceName);
        Serial.println("   📶 Canal WiFi: " + String(peerChannel));
        
        // 🚨 CRÍTICO: Usar addPeerWithChannel para garantir canal correto
        if (espNowController->addPeerWithChannel(senderMac, peerChannel, deviceName)) {
            Serial.println("✅ Slave registrado como peer ESP-NOW no canal " + String(peerChannel) + "!");
            Serial.println("📡 Comunicação bidirecional Master ↔ Slave ativa!");
            Serial.println("🎯 Agora posso responder aos PINGs do Slave");
            Serial.println("==============================================\n");
        } else {
            Serial.println("❌ Falha ao adicionar Slave como peer ESP-NOW");
            Serial.println("⚠️ Comunicação do Slave → Master será limitada");
            Serial.println("==============================================\n");
        }
    } else if (espNowController && espNowController->peerExists(senderMac)) {
        // 🚨 CRÍTICO: Verificar se canal mudou e atualizar peer se necessário
        if (peerChannel > 0 && peerChannel <= 13) {
            Serial.println("✅ Slave já está registrado como peer ESP-NOW");
            Serial.println("📶 Canal WiFi do Slave: " + String(peerChannel));
            
            // 🚨 CRÍTICO: Verificar se o canal do peer precisa ser atualizado
            esp_now_peer_info_t peerInfo;
            if (esp_now_get_peer(senderMac, &peerInfo) == ESP_OK) {
                if (peerInfo.channel != peerChannel) {
                    Serial.println("\n⚠️ ========================================");
                    Serial.println("⚠️ CANAL DO PEER DESATUALIZADO!");
                    Serial.println("⚠️ ========================================");
                    Serial.println("   📶 Canal atual do peer: " + String(peerInfo.channel));
                    Serial.println("   📶 Canal real do Slave: " + String(peerChannel));
                    Serial.println("   🔄 Atualizando peer para sincronizar canais...");
                    
                    // Remover peer existente
                    esp_now_del_peer(senderMac);
                    
                    // Adicionar peer com canal correto
                    if (espNowController->addPeerWithChannel(senderMac, peerChannel, deviceName)) {
                        Serial.println("   ✅ Peer actualizado al canal " + String(peerChannel) + "!");
                        Serial.println("   📡 Comunicação bidirecional sincronizada!");
                    } else {
                        Serial.println("   ❌ Falha ao actualizar peer");
                    }
                    Serial.println("========================================\n");
                } else {
                    Serial.println("   ✅ Canal do peer já está sincronizado (canal " + String(peerChannel) + ")");
                }
            }
        } else {
            Serial.println("✅ Slave já está registrado como peer ESP-NOW\n");
        }
    }
    
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de verificar/adicionar
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em processDeviceInfoReceived()");
        return;
    }
    
    // ⭐ CORREÇÃO CRÍTICA: Verificar se Slave já existe na lista confiável (usar unsafe porque ya tenemos mutex)
    TrustedSlave* slave = findTrustedSlaveUnsafe(senderMac);
    bool isNewSlave = (slave == nullptr);
    
    if (isNewSlave) {
        xSemaphoreGive(trustedSlavesMutex);  // Liberar antes de addTrustedSlave (que toma mutex)
        
        // 🎉 NOVO SLAVE! Adicionar à lista confiável
        Serial.println("\n🎉 === NOVO SLAVE DETECTADO ===");
        Serial.println("📝 Adicionando à lista confiável...");
        
        if (addTrustedSlave(senderMac, deviceName, deviceType)) {
            // Tomar mutex de nuevo para buscar
            if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                slave = findTrustedSlaveUnsafe(senderMac);
                Serial.println("✅ Slave adicionado à lista confiável!");
            } else {
                Serial.println("⚠️ Timeout ao obter mutex após addTrustedSlave()");
                return;
            }
        } else {
            Serial.println("❌ Falha ao adicionar Slave à lista confiável");
            return;
        }
        Serial.println("===============================\n");
    }
    
    // Atualizar informações detalhadas do Slave (mutex mantido para novos e existentes)
    if (slave) {
        bool wasOffline = !slave->isOnline();
        slave->numRelays = numRelays;
        slave->operational = operational;
        slave->messagesReceived++;
        slave->status = SlaveStatus::ONLINE;
        slave->updateLastSeen();
        
        if (!deviceName.isEmpty()) {
            slave->deviceName = deviceName;
        }
        if (!deviceType.isEmpty()) {
            slave->deviceType = deviceType;
        }
        
        // 🚨 CRÍTICO: Guardar canal WiFi do Slave
        if (wifiChannel > 0 && wifiChannel <= 13) {
            if (slave->wifiChannel == 0 || slave->wifiChannel != wifiChannel) {
                Serial.println("💾 Atualizando canal WiFi do Slave: " + String(slave->wifiChannel) + " → " + String(wifiChannel));
                slave->wifiChannel = wifiChannel;
            }
        }
        
        String deviceNameForCallback = slave->deviceName;
        
        xSemaphoreGive(trustedSlavesMutex);  // ✅ Um único give por caminho
        
        // 🚨 NOVO: Se slave estava offline e voltou online, enviar comandos pendentes
        if (wasOffline) {
            Serial.println("\n🔄 ========================================");
            Serial.println("🔄 SLAVE VOLTOU ONLINE - ENVIANDO COMANDOS PENDENTES");
            Serial.println("🔄 ========================================");
            sendPendingCommandsToSlave(senderMac);
            
            if (slaveOnlineCallback) {
                slaveOnlineCallback(senderMac, deviceNameForCallback);
            }
        }
        
        if (isNewSlave) {
            Serial.println("📊 Slave configurado na lista confiável");
        } else {
            Serial.println("📊 Informações atualizadas na lista confiável");
        }
    } else {
        xSemaphoreGive(trustedSlavesMutex);
    }
    
    // Chamar callback de device info se definido (para compatibilidade)
    if (deviceInfoCallback && !isNewSlave) {
        deviceInfoCallback(senderMac, deviceName, deviceType, numRelays, operational, wifiChannel);
    }
    
    Serial.println("========================================\n");
}

void MasterSlaveManager::processRelayStatusReceived(const uint8_t* senderMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name) {
    // ✅ OTIMIZADO: Log removido (chamado 8x por ALL_RELAYS_STATUS - muito spam)
    // Serial.println("📊 Status de relé recebido de " + ESPNowController::macToString(senderMac) + 
    //               ": " + name + " -> " + (state ? "ON" : "OFF"));
    
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de modificar
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em processRelayStatusReceived()");
        return;
    }
    
    // Buscar slave diretamente (sem usar getTrustedSlave para evitar deadlock)
    TrustedSlave* slave = nullptr;
    for (auto& s : trustedSlaves) {
        if (memcmp(s.macAddress, senderMac, 6) == 0) {
            slave = &s;
            break;
        }
    }
    
    if (slave) {
        slave->messagesReceived++;// ⭐ POTENCIA MÁXIMA: Atualizar estado do relé remoto
        if (relayNumber >= 0 && relayNumber < 8) {
            bool oldState = slave->relayStates[relayNumber].state;
            if (!processingStatusResponse && oldState != state) {
                Serial.printf("[ESPNOW] relay delta %s r%d %s→%s\n",
                             ESPNowController::macToString(senderMac).c_str(),
                             relayNumber,
                             oldState ? "ON" : "OFF",
                             state ? "ON" : "OFF");
            }
            
            slave->relayStates[relayNumber].state = state;
            slave->relayStates[relayNumber].hasTimer = hasTimer;
            slave->relayStates[relayNumber].remainingTime = remainingTime;
            slave->relayStates[relayNumber].lastUpdate = millis();
            slave->relayStates[relayNumber].name = "Relé " + String(relayNumber);
            
            // ✅ CRÍTICO: Atualizar Supabase (fonte única de verdade)
            // Solo actualizar si no estamos procesando ALL_RELAYS_STATUS (evitar spam)
            if (supabaseRelayStateCallback && !processingStatusResponse) {
                String masterDeviceId = getDeviceID();
                String slaveMac = ESPNowController::macToString(senderMac);
                String slaveDeviceId = "ESP32_SLAVE_" + slaveMac;
                slaveDeviceId.replace(":", "_");
                supabaseRelayStateCallback(masterDeviceId, slaveMac, slaveDeviceId, relayNumber, state, hasTimer, remainingTime);
            }
            
            // ✅ NOVO: Guardar en NVS (cache local) cuando se recibe estado real
            // Guardar solo si no estamos procesando ALL_RELAYS_STATUS (evitar escrituras excesivas)
            // ✅ Menos exigente: Cache de validación no precisa ser tão frequente
            if (!processingStatusResponse) {
                static unsigned long lastNvsSave = 0;
                // ✅ Guardar máximo uma vez a cada 30 segundos (cache de validação, não crítico)
                // Reduz wear do NVS mantendo cache razoavelmente atualizado
                if (millis() - lastNvsSave > 30000) {  // 30 segundos
                    saveSlaveRelayStatesToNVS();
                    lastNvsSave = millis();
                }
            }
        }
        
        // Guardar deviceName para callback (antes de liberar mutex)
        String deviceName = slave->deviceName;
        
        xSemaphoreGive(trustedSlavesMutex);

        if (!processingStatusResponse) {
            touchSlaveLink(senderMac, "relay_status_rx");
        }
        (void)deviceName;
    } else {
        xSemaphoreGive(trustedSlavesMutex);
    }
    
    // Chamar callback se definido
    if (relayStatusCallback) {
        relayStatusCallback(senderMac, relayNumber, state, hasTimer, remainingTime, name);
    }
}

void MasterSlaveManager::updateTrustedSlave(const uint8_t* macAddress, const String& deviceName, const String& deviceType, SlaveStatus status) {
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de modificar
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em updateTrustedSlave()");
        return;
    }
    
    // Buscar slave diretamente (sem usar getTrustedSlave para evitar deadlock)
    TrustedSlave* slave = nullptr;
    for (auto& s : trustedSlaves) {
        if (memcmp(s.macAddress, macAddress, 6) == 0) {
            slave = &s;
            break;
        }
    }
    
    if (slave) {
        slave->updateLastSeen();
        
        if (!deviceName.isEmpty()) {
            slave->deviceName = deviceName;
        }
        if (!deviceType.isEmpty()) {
            slave->deviceType = deviceType;
        }
        if (status != SlaveStatus::UNKNOWN) {
            slave->status = status;
        }
    }
    
    xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex
}

void MasterSlaveManager::checkSlaveStatus() {
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    const unsigned long offlineTimeout = SLAVE_OFFLINE_TIMEOUT_MS;
    
    for (auto& slave : trustedSlaves) {
        if (slave.isOfflineTimeout(offlineTimeout)) {
            if (slave.status != SlaveStatus::OFFLINE) {
                slave.status = SlaveStatus::OFFLINE;
                logSlaveLink("offline_mark", slave.macAddress,
                             (long)slave.getTimeSinceLastSeen());
                
                if (slaveOfflineCallback) {
                    slaveOfflineCallback(slave.macAddress, slave.deviceName);
                }
            }
        }
    }
    
    xSemaphoreGive(trustedSlavesMutex);
}

void MasterSlaveManager::cleanupExpiredAcks() {
    unsigned long currentTime = millis();
    const unsigned long ackTimeout = 5000;  // 5 segundos
    
    for (auto it = pendingAcks.begin(); it != pendingAcks.end();) {
        if (currentTime - it->timestamp > ackTimeout) {
            Serial.println("⏰ ACK expirado: " + ESPNowController::macToString(it->macAddress) + 
                          " (MsgID: " + String(it->messageId) + ")");
            it = pendingAcks.erase(it);
        } else {
            ++it;
        }
    }
}

void MasterSlaveManager::resendPendingAcks() {
    unsigned long currentTime = millis();
    const unsigned long resendInterval = 2000;  // 2 segundos
    
    for (auto& pending : pendingAcks) {
        if (currentTime - pending.timestamp > resendInterval && pending.retryCount < 3) {
            Serial.println("🔄 Reenviando ACK: " + ESPNowController::macToString(pending.macAddress) + 
                          " (MsgID: " + String(pending.messageId) + ")");
            
            if (sendAck(pending.macAddress, pending.messageId)) {
                pending.timestamp = currentTime;
                pending.retryCount++;
            }
        }
    }
}

// ===== CALLBACKS ESTÁTICOS =====

void MasterSlaveManager::onPingReceivedStatic(const uint8_t* senderMac) {
    if (instance) {
        // ✅ CORREÇÃO CRÍTICA: Log para diagnosticar se callback está sendo chamado
        Serial.println("\n🔔 [CALLBACK] onPingReceivedStatic chamado para: " + ESPNowController::macToString(senderMac));
        // O ESPNowController já responde automaticamente ao PING com PONG
        // Aqui processamos nossa lógica adicional
        instance->processPingReceived(senderMac, millis());  // Usar timestamp como ID
    } else {
        Serial.println("❌ [CALLBACK] MasterSlaveManager::instance é NULL!");
    }
}

void MasterSlaveManager::onPongReceivedStatic(const uint8_t* senderMac) {
    if (instance) {
        instance->processPongReceived(senderMac, millis());
    }
}

void MasterSlaveManager::onDeviceInfoReceivedStatic(const uint8_t* senderMac, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel) {
    // ✅ REDUZIDO: Logs de debug removidos para reduzir verbosidade
    if (instance) {
        instance->processDeviceInfoReceived(senderMac, deviceName, deviceType, numRelays, operational, wifiChannel);
    } else {
        Serial.println("❌ MasterSlaveManager::instance é NULL!");
    }
}

void MasterSlaveManager::onRelayStatusReceivedStatic(const uint8_t* senderMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name) {
    if (instance) {
        instance->processRelayStatusReceived(senderMac, relayNumber, state, hasTimer, remainingTime, name);
    }
}

void MasterSlaveManager::onErrorReceivedStatic(const String& error) {
    if (instance) {
        instance->totalErrors++;
        Serial.println("❌ Erro ESP-NOW: " + error);
        
        if (instance->errorCallback) {
            instance->errorCallback(nullptr, error);
        }
    }
}

// ===== UTILITÁRIOS =====

String MasterSlaveManager::getStatsJSON() {
    DynamicJsonDocument doc(1024);
    
    doc["initialized"] = initialized;
    doc["totalPingsReceived"] = totalPingsReceived;
    doc["totalPongsSent"] = totalPongsSent;
    doc["totalAcksSent"] = totalAcksSent;
    doc["totalAcksReceived"] = totalAcksReceived;
    doc["totalErrors"] = totalErrors;
    doc["trustedSlavesCount"] = trustedSlaves.size();
    doc["onlineSlavesCount"] = getOnlineSlaveCount();
    doc["pendingAcksCount"] = pendingAcks.size();
    
    JsonArray slaves = doc.createNestedArray("slaves");
    for (const auto& slave : trustedSlaves) {
        JsonObject slaveObj = slaves.createNestedObject();
        slaveObj["mac"] = ESPNowController::macToString(slave.macAddress);
        slaveObj["name"] = slave.deviceName;
        slaveObj["type"] = slave.deviceType;
        slaveObj["status"] = (int)slave.status;
        slaveObj["online"] = slave.isOnline();
        slaveObj["lastSeen"] = slave.lastSeen;
        slaveObj["rssi"] = slave.rssi;
        slaveObj["numRelays"] = slave.numRelays;
        slaveObj["operational"] = slave.operational;
        slaveObj["pingsReceived"] = slave.pingsReceived;
        slaveObj["pingsSent"] = slave.pingsSent;
        slaveObj["pongsReceived"] = slave.pongsReceived;
        slaveObj["pongsSent"] = slave.pongsSent;
        slaveObj["messagesReceived"] = slave.messagesReceived;
        slaveObj["messagesLost"] = slave.messagesLost;
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}

void MasterSlaveManager::printStatus() {
    Serial.println("\n🎯 === STATUS MASTER-SLAVE MANAGER ===");
    Serial.println("✅ Inicializado: " + String(initialized ? "Sim" : "Não"));
    Serial.println("📊 PINGs recebidos: " + String(totalPingsReceived));
    Serial.println("📊 PONGs enviados: " + String(totalPongsSent));
    Serial.println("📊 ACKs enviados: " + String(totalAcksSent));
    Serial.println("📊 ACKs recebidos: " + String(totalAcksReceived));
    Serial.println("📊 Erros: " + String(totalErrors));
    Serial.println("👥 Slaves confiáveis: " + String(trustedSlaves.size()));
    Serial.println("🟢 Slaves online: " + String(getOnlineSlaveCount()));
    Serial.println("⏳ ACKs pendentes: " + String(pendingAcks.size()));
    
    if (!trustedSlaves.empty()) {
        Serial.println("\n👥 === SLAVES CONFIÁVEIS ===");
        for (const auto& slave : trustedSlaves) {
            Serial.println("   " + ESPNowController::macToString(slave.macAddress) + " | " + 
                          slave.deviceName + " (" + slave.deviceType + ") | " +
                          (slave.isOnline() ? "🟢 Online" : "🔴 Offline") + 
                          " | Relés: " + String(slave.numRelays) + 
                          " | PINGs: " + String(slave.pingsReceived));
        }
    }
    
    Serial.println("=====================================\n");
}

void MasterSlaveManager::printTrustedSlaves() {
    Serial.println("\n👥 === LISTA DE SLAVES CONFIÁVEIS ===");
    
    // ✅ PROTEÇÃO MULTI-CORE: Usar getAllTrustedSlaves() que já protege com mutex
    std::vector<TrustedSlave> slaves = getAllTrustedSlaves();
    
    if (slaves.empty()) {
        Serial.println("📭 Nenhum Slave confiável encontrado");
    } else {
        for (size_t i = 0; i < slaves.size(); i++) {
            const auto& slave = slaves[i];
            Serial.println("\n📋 Slave #" + String(i + 1) + ":");
            Serial.println("   MAC: " + ESPNowController::macToString(slave.macAddress));
            Serial.println("   Nome: " + slave.deviceName);
            Serial.println("   Tipo: " + slave.deviceType);
            Serial.println("   Status: " + String((int)slave.status));
            Serial.println("   Online: " + String(slave.isOnline() ? "Sim" : "Não"));
            Serial.println("   Relés: " + String(slave.numRelays));
            Serial.println("   Operacional: " + String(slave.operational ? "Sim" : "Não"));
            Serial.println("   Último contato: " + String(slave.getTimeSinceLastSeen() / 1000) + "s atrás");
            Serial.println("   PINGs recebidos: " + String(slave.pingsReceived));
            Serial.println("   PINGs enviados: " + String(slave.pingsSent));
            Serial.println("   PONGs recebidos: " + String(slave.pongsReceived));
            Serial.println("   PONGs enviados: " + String(slave.pongsSent));
            Serial.println("   Mensagens recebidas: " + String(slave.messagesReceived));
            Serial.println("   Mensagens perdidas: " + String(slave.messagesLost));
        }
    }
    
    Serial.println("=====================================\n");
}

void MasterSlaveManager::cleanupOfflineSlaves() {
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em cleanupOfflineSlaves()");
        return;
    }
    
    int offlineCount = 0;
    for (const auto& slave : trustedSlaves) {
        if (slave.isOfflineTimeout(120000)) {
            offlineCount++;
        }
    }
    
    Serial.println("🧹 Slaves offline (>2min): " + String(offlineCount) + " — mantidos em trustedSlaves (use 'cleanup remove' manual se necessário)");
    
    xSemaphoreGive(trustedSlavesMutex);
}

void MasterSlaveManager::rediscoverSlaves() {
    if (!espNowController) {
        Serial.println("❌ ESPNowController não inicializado");
        return;
    }

    if (getTrustedSlaveCount() > 0 && getOnlineSlaveCount() >= getTrustedSlaveCount()) {
        LOG_ESPNOW_DEBUG("Discovery skip: todos slaves online");
        return;
    }
    
    // ✅ PATRÓN MASTER-TASK: Verificar se peer de broadcast está registrado
    uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (!esp_now_is_peer_exist(broadcastMac)) {
        Serial.println("⚠️ Peer de broadcast não encontrado - registrando...");
        
        // Obter canal atual do WiFi
        wifi_second_chan_t secondChan;
        uint8_t currentChannel;
        esp_wifi_get_channel(&currentChannel, &secondChan);
        
        // Adicionar peer de broadcast
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, broadcastMac, 6);
        peerInfo.channel = currentChannel;
        peerInfo.encrypt = false;
        peerInfo.ifidx = WIFI_IF_STA;
        
        esp_err_t result = esp_now_add_peer(&peerInfo);
        if (result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST) {
            Serial.println("✅ Peer de broadcast registrado no canal " + String(currentChannel));
        } else {
            Serial.println("❌ Erro ao registrar peer de broadcast: " + String(result));
            Serial.println("💡 Tentando continuar mesmo assim...");
        }
    }
    
    // ✅ PATRÓN MASTER-TASK: Enviar broadcasts espaciados (1 log INFO/ciclo)
    LOG_ESPNOW_INFO("📢 Discovery: enviando 3 broadcasts ESP-NOW");
    for (int i = 0; i < 3; i++) {
        bool success = espNowController->sendDiscoveryBroadcast();
        LOG_ESPNOW_DEBUG(String("   Broadcast ") + (i + 1) + "/3: " + (success ? "OK" : "FALHA"));
        
        // ✅ Delay entre broadcasts (1.5s como no MASTER-TASK)
        // ✅ CORREÇÃO: Usar vTaskDelay() em vez de delay() para não bloquear outras tasks
        if (i < 2) {  // No delay após o último
            vTaskDelay(pdMS_TO_TICKS(1500));  // Não bloqueia outras tasks
        }
        
        // ✅ PATRÓN MASTER-TASK: Processar respostas imediatas após cada broadcast
        update(); // Processar respostas no MasterSlaveManager
        if (espNowController) {
            espNowController->update(); // Processar respostas no ESPNowController
        }
    }
    
#if DEBUG_ESPNOW
    Serial.println("⏳ Aguardando respostas dos Slaves...");
#endif
}

#if DEBUG_ESPNOW
static void formatRelayMask(const bool states[8], char* buf) {
    for (int i = 0; i < 8; i++) {
        buf[i] = states[i] ? 'T' : 'F';
    }
    buf[8] = '\0';
}
#endif

// ===== 🔄 FASE 1: IMPLEMENTAÇÃO DO SISTEMA DE RETRY =====

bool MasterSlaveManager::lockPendingQueue(TickType_t timeout) const {
    if (!pendingRelayCommandsMutex) return true;
    return xSemaphoreTake(pendingRelayCommandsMutex, timeout) == pdTRUE;
}

void MasterSlaveManager::unlockPendingQueue() const {
    if (pendingRelayCommandsMutex) {
        xSemaphoreGive(pendingRelayCommandsMutex);
    }
}

size_t MasterSlaveManager::getPendingRelayCommandCount() const {
    if (!lockPendingQueue(pdMS_TO_TICKS(50))) {
        return 0;
    }
    size_t n = pendingRelayCommands.size();
    unlockPendingQueue();
    return n;
}

uint32_t MasterSlaveManager::generateCommandId() {
    return ++commandIdCounter;
}

void MasterSlaveManager::addToRetryQueue(const uint8_t* targetMac, int relayNumber, const String& action,
                                         int duration, uint32_t commandId, int supabaseCommandId,
                                         bool waitingForAck, int cycleOffDuration,
                                         const String& commandMode) {
    PendingRelayCommand cmd;
    memcpy(cmd.targetMac, targetMac, 6);
    cmd.relayNumber = relayNumber;
    cmd.action = action;
    cmd.duration = duration;
    cmd.cycleOffDuration = cycleOffDuration;
    cmd.commandMode = commandMode;
    cmd.enqueuedAt = millis();
    cmd.ackWaitStartedAt = waitingForAck ? millis() : 0;
    cmd.nextRetry = waitingForAck ? (millis() + 60000UL) : (millis() + RETRY_INTERVAL);
    cmd.retryCount = 0;
    cmd.commandId = commandId;
    cmd.waitingForAck = waitingForAck;
    cmd.supabaseCommandId = supabaseCommandId;

    if (!lockPendingQueue()) {
        Serial.println("❌ addToRetryQueue: mutex timeout");
        return;
    }

    for (auto& existing : pendingRelayCommands) {
        if (memcmp(existing.targetMac, targetMac, 6) != 0 || existing.relayNumber != relayNumber) {
            continue;
        }
        existing.action = action;
        existing.duration = duration;
        existing.cycleOffDuration = cycleOffDuration;
        existing.commandMode = commandMode;
        existing.commandId = commandId;
        existing.supabaseCommandId = supabaseCommandId;
        existing.enqueuedAt = millis();
        if (waitingForAck) {
            existing.waitingForAck = true;
            existing.ackWaitStartedAt = millis();
            existing.nextRetry = millis() + 60000UL;
        } else if (!existing.waitingForAck) {
            existing.nextRetry = millis() + RETRY_INTERVAL;
            existing.retryCount = 0;
        }
        unlockPendingQueue();
        Serial.printf("📋 [QUEUE] coalesce relay=%d esp=%u supabase=%d\n",
                      relayNumber, commandId, supabaseCommandId);
        return;
    }

    while (pendingRelayCommands.size() >= MAX_PENDING_RELAY_COMMANDS) {
        size_t victimIdx = 0;
        unsigned long oldestAt = ULONG_MAX;
        bool foundNonAck = false;
        for (size_t i = 0; i < pendingRelayCommands.size(); i++) {
            const auto& entry = pendingRelayCommands[i];
            if (entry.waitingForAck) {
                continue;
            }
            foundNonAck = true;
            if (entry.enqueuedAt < oldestAt) {
                oldestAt = entry.enqueuedAt;
                victimIdx = i;
            }
        }
        if (!foundNonAck) {
            oldestAt = ULONG_MAX;
            for (size_t i = 0; i < pendingRelayCommands.size(); i++) {
                if (pendingRelayCommands[i].enqueuedAt < oldestAt) {
                    oldestAt = pendingRelayCommands[i].enqueuedAt;
                    victimIdx = i;
                }
            }
        }
        const auto dropped = pendingRelayCommands[victimIdx];
        pendingRelayCommands.erase(pendingRelayCommands.begin() + static_cast<long>(victimIdx));
        Serial.printf("⚠️ [QUEUE] cap=%u drop esp=%u relay=%d supabase=%d\n",
                      static_cast<unsigned>(MAX_PENDING_RELAY_COMMANDS),
                      dropped.commandId, dropped.relayNumber, dropped.supabaseCommandId);
    }

    pendingRelayCommands.push_back(cmd);
    unlockPendingQueue();
    
    if (waitingForAck) {
        Serial.printf("📋 [ACK-WAIT] ESP-NOW id=%u supabase=%d relay=%d %s\n",
                      commandId, supabaseCommandId, relayNumber, action.c_str());
        return;
    }

    Serial.println("\n📋 ========================================");
    Serial.println("📋 COMANDO ADICIONADO À FILA DE RETRY");
    Serial.println("📋 ========================================");
    Serial.println("🆔 Command ID: " + String(commandId));
    Serial.println("📡 Destino: " + ESPNowController::macToString(targetMac));
    Serial.println("🔌 Relé: " + String(relayNumber));
    Serial.println("⚡ Ação: " + action);
    Serial.println("⏱️ Próximo retry em: 2s");
    Serial.println("========================================\n");
}

void MasterSlaveManager::processRetryQueue() {
    if (!lockPendingQueue(pdMS_TO_TICKS(200))) {
        return;
    }
    if (pendingRelayCommands.empty()) {
        unlockPendingQueue();
        return;
    }
    
    unsigned long now = millis();
    
    for (auto it = pendingRelayCommands.begin(); it != pendingRelayCommands.end();) {
        if (now >= it->nextRetry && it->retryCount < MAX_RELAY_RETRIES && !it->waitingForAck) {
            if (hasInFlightForMacLocked(it->targetMac)) {
                ++it;
                continue;
            }
            if (!canEspNowSendToMac(it->targetMac)) {
                it->nextRetry = now + MIN_ESPNOW_SEND_GAP_MS;
                ++it;
                continue;
            }

            Serial.println("\n🔄 ========================================");
            Serial.println("🔄 REINTENTANDO COMANDO");
            Serial.println("🔄 ========================================");
            Serial.println("🆔 Command ID: " + String(it->commandId));
            Serial.println("📡 Destino: " + ESPNowController::macToString(it->targetMac));
            Serial.println("🔌 Relé: " + String(it->relayNumber));
            Serial.println("⚡ Ação: " + it->action);
            Serial.println("🔢 Tentativa: " + String(it->retryCount + 1) + "/" + String(MAX_RELAY_RETRIES));
            
            bool success = false;
            if (it->relayNumber == 255 || it->action == "on_all" || it->action == "off_all") {
                const uint8_t mask = (it->action == "off_all") ? 0x00 : 0xFF;
                success = espNowController->sendSetRelayMask(it->targetMac, mask,
                                                              (uint16_t)it->duration, it->commandId);
            } else {
                String espAction = resolveSlaveEspAction(it->action, it->commandMode);
                success = espNowController->sendRelayCommand(it->targetMac, it->relayNumber, espAction,
                                                              it->duration, it->commandId,
                                                              it->cycleOffDuration, it->commandMode);
            }
            
            if (success) {
                markEspNowSendToMac(it->targetMac);
                Serial.println("✅ Reintento exitoso!");
                Serial.println("⏳ Aguardando confirmação do Slave...");
                Serial.println("========================================\n");
                
                it->waitingForAck = true;
                it->ackWaitStartedAt = now;
                it->retryCount++;
                it->nextRetry = now + (RETRY_INTERVAL * it->retryCount);
                ++it;
            } else {
                Serial.println("❌ Reintento falhou!");
                it->retryCount++;
                
                if (it->retryCount >= MAX_RELAY_RETRIES) {
                    Serial.println("💔 COMANDO DESCARTADO APÓS " + String(MAX_RELAY_RETRIES) + " TENTATIVAS");
                    Serial.println("========================================\n");
                    
                    // ✅ Marcar como failed en Supabase si viene de Supabase
                    if (it->supabaseCommandId > 0 && supabaseCommandCallback) {
                        String errorMsg = "Comando falló después de " + String(MAX_RELAY_RETRIES) + " intentos";
                        supabaseCommandCallback(it->supabaseCommandId, false, errorMsg);
                        Serial.println("📤 Comando marcado como FAILED en Supabase (ID: " + String(it->supabaseCommandId) + ")");
                    }
                    
                    // Registrar erro
                    TrustedSlave* slave = getTrustedSlave(it->targetMac);
                    if (slave) {
                        slave->messagesLost++;
                        slave->errors++;
                    }
                    
                    it = pendingRelayCommands.erase(it);
                } else {
                    // Calcular próximo retry com backoff exponencial
                    it->nextRetry = now + (RETRY_INTERVAL * it->retryCount);
                    Serial.println("⏱️ Próximo retry em: " + String((RETRY_INTERVAL * it->retryCount) / 1000) + "s");
                    Serial.println("========================================\n");
                    ++it;
                }
            }
        }
        // 🚨 CRÍTICO: Verificar timeout de 1 minuto (comandos acumulados por offline)
        else if (!it->waitingForAck && (now - it->enqueuedAt > SLAVE_QUEUE_OFFLINE_TIMEOUT_MS)) {
            long ageSec = (long)(now - it->enqueuedAt) / 1000L;
            Serial.println("\n⏰ ========================================");
            Serial.println("⏰ TIMEOUT DE COMANDO (60 segundos)");
            Serial.println("⏰ ========================================");
            Serial.println("🆔 Command ID: " + String(it->commandId));
            Serial.println("📡 Destino: " + ESPNowController::macToString(it->targetMac));
            Serial.println("🔌 Relé: " + String(it->relayNumber));
            Serial.println("⚡ Ação: " + it->action);
            Serial.println("⏱️ Tempo em fila: " + String(ageSec) + "s");
            Serial.println("⚠️ Comando descartado por timeout (slave offline > 60s)");
            
            // ✅ Marcar como failed en Supabase si viene de Supabase
            if (it->supabaseCommandId > 0 && supabaseCommandCallback) {
                String errorMsg = "Comando expirado - Slave offline por más de 1 minuto";
                supabaseCommandCallback(it->supabaseCommandId, false, errorMsg);
                Serial.println("📤 Comando marcado como FAILED en Supabase (ID: " + String(it->supabaseCommandId) + ")");
            }
            
            Serial.println("========================================\n");
            
            // Registrar erro
            TrustedSlave* slave = getTrustedSlave(it->targetMac);
            if (slave) {
                slave->messagesLost++;
                slave->errors++;
            }
            
            it = pendingRelayCommands.erase(it);
        }
        // Verificar timeout de ACK (se está esperando muito tempo por ACK)
        else if (it->waitingForAck && it->ackWaitStartedAt > 0 &&
                 (now - it->ackWaitStartedAt > ACK_WAIT_TIMEOUT_MS)) {
            Serial.println("\n⏰ ========================================");
            Serial.println("⏰ TIMEOUT DE ACK");
            Serial.println("⏰ ========================================");
            Serial.println("🆔 Command ID: " + String(it->commandId));
            Serial.println("📡 Destino: " + ESPNowController::macToString(it->targetMac));

            TrustedSlave* slave = getTrustedSlave(it->targetMac);
            bool hardwareOk = false;
            if (slave && it->relayNumber >= 0 && it->relayNumber < slave->numRelays) {
                const bool actualOn = slave->relayStates[it->relayNumber].state;
                const bool expectedOn = (it->action == "on" || it->action == "on_forever");
                hardwareOk = (actualOn == expectedOn);
            }

            if (hardwareOk) {
                Serial.println("ℹ️ Hardware confirma estado — omitindo failed (ALL_RELAYS/estado real)");
                if (it->supabaseCommandId > 0 && slaveCommandResolvedCallback) {
                    const bool expectedOn = (it->action == "on" || it->action == "on_forever");
                    slaveCommandResolvedCallback(it->supabaseCommandId, it->commandId, it->targetMac,
                                                 it->relayNumber, expectedOn);
                }
                Serial.println("========================================\n");
                it = pendingRelayCommands.erase(it);
            } else {
                Serial.println("⚠️ Comando descartado por timeout de ACK");

                if (it->supabaseCommandId > 0 && supabaseCommandCallback) {
                    String errorMsg = "Timeout esperando ACK del slave";
                    supabaseCommandCallback(it->supabaseCommandId, false, errorMsg);
                    Serial.println("📤 Comando marcado como FAILED en Supabase (ID: " + String(it->supabaseCommandId) + ")");
                }

                Serial.println("========================================\n");
                it = pendingRelayCommands.erase(it);
            }
        }
        else {
            ++it;
        }
    }
    unlockPendingQueue();
}

void MasterSlaveManager::removeFromRetryQueue(uint32_t commandId, bool currentState, bool notifySupabase) {
    if (!lockPendingQueue(pdMS_TO_TICKS(500))) {
        Serial.println("❌ removeFromRetryQueue: mutex timeout id=" + String(commandId));
        return;
    }
    for (auto it = pendingRelayCommands.begin(); it != pendingRelayCommands.end(); ++it) {
        if (it->commandId == commandId) {
            Serial.println("\n✅ ========================================");
            Serial.println("✅ COMANDO CONFIRMADO");
            Serial.println("✅ ========================================");
            Serial.println("🆔 Command ID: " + String(commandId));
            Serial.println("📡 Destino: " + ESPNowController::macToString(it->targetMac));
            Serial.println("🔌 Relé: " + String(it->relayNumber));
            Serial.println("⚡ Ação: " + it->action);
            Serial.println("💡 Estado final: " + String(currentState ? "ON" : "OFF"));
            Serial.println("🎯 Removido da fila de retry");
            
            if (it->supabaseCommandId > 0 && supabaseCommandCallback && notifySupabase) {
                String stateStr = currentState ? "true" : "false";
                supabaseCommandCallback(it->supabaseCommandId, true, stateStr);
                Serial.println("📤 Comando marcado como COMPLETED en Supabase (ID: " + String(it->supabaseCommandId) + ", state: " + stateStr + ")");
            }
            
            Serial.println("========================================\n");
            
            pendingRelayCommands.erase(it);
            break;
        }
    }
    unlockPendingQueue();
}

// 🚨 NOVO: Enviar todos os comandos pendentes para um slave quando ele volta online
void MasterSlaveManager::sendPendingCommandsToSlave(const uint8_t* macAddress) {
    if (!initialized || !espNowController) return;
    
    if (!lockPendingQueue(pdMS_TO_TICKS(200))) {
        return;
    }

    int pendingCount = 0;
    unsigned long now = millis();
    for (auto& cmd : pendingRelayCommands) {
        if (memcmp(cmd.targetMac, macAddress, 6) == 0 && !cmd.waitingForAck) {
            pendingCount++;
            cmd.retryCount = 0;
            cmd.nextRetry = now + 100;
        }
    }
    unlockPendingQueue();
    
    if (pendingCount == 0) {
        return;
    }

    Serial.printf("[SLAVE-LINK] event=pending_drain mac=%s count=%d\n",
                  ESPNowController::macToString(macAddress).c_str(), pendingCount);
}

// ===== 🔄 FASE 2: PROCESSAMENTO DE ACKs DE RELAY =====

void MasterSlaveManager::processRelayCommandAck(const RelayCommandAck& ack, const uint8_t* senderMac) {
    Serial.printf("[RELAY-ACK] id=%u relay=%u ok=%u state=%s mac=%s\n",
                  (unsigned)ack.commandId, (unsigned)ack.relayNumber,
                  (unsigned)ack.success, ack.currentState ? "ON" : "OFF",
                  ESPNowController::macToString(senderMac).c_str());

    touchSlaveLink(senderMac, "relay_ack_rx");

    // Liberar cola ANTES de callbacks bloqueantes (SSL/MQTT) — evita race com processRetryQueue
    const bool currentState = (ack.currentState == 1);
    if (ack.success) {
        removeFromRetryQueue(ack.commandId, currentState, false);
    }

    // Callback rápido (MQTT command_ack) — no bloquear cola
    if (relayAckCallback) {
        relayAckCallback(senderMac, ack.commandId, ack.success, ack.relayNumber, ack.currentState);
    }
    
    // Atualizar estatísticas do Slave (sem SSL aquí)
    TrustedSlave* slave = getTrustedSlave(senderMac);
    if (slave) {
        slave->updateLastSeen();
        slave->messagesReceived++;
        
        if (ack.success && ack.relayNumber < 8) {
            slave->relayStates[ack.relayNumber].state = currentState;
            slave->relayStates[ack.relayNumber].lastUpdate = millis();
            Serial.println("✅ Estado do relé " + String(ack.relayNumber) + " atualizado desde ACK: " + (currentState ? "ON" : "OFF"));
        }
        
        if (ack.success) {
            Serial.println("📊 Slave executou comando com sucesso!");
        } else {
            Serial.println("⚠️ Slave reportou falha na execução");
            slave->errors++;
        }
    } else if (ack.success) {
        Serial.println("📊 Slave executou comando com sucesso!");
    } else {
        Serial.println("\n⚠️ Comando falhou no Slave");
        Serial.println("🔄 Sistema de retry continuará tentando...");
    }
}


// ===== ✅ PASSO 1: PROCESSAMENTO DE COMANDOS DO SUPABASE PARA SLAVES =====

bool MasterSlaveManager::deviceIdToMacAddress(const String& deviceId, uint8_t* macAddress) {
    // Verificar se é um device_id de slave
    if (!deviceId.startsWith("ESP32_SLAVE_")) {
        Serial.println("❌ device_id não é de um slave: " + deviceId);
        return false;
    }
    
    // Extrair parte do MAC (remover "ESP32_SLAVE_")
    String macStr = deviceId.substring(12); // Remove "ESP32_SLAVE_"
    macStr.replace("_", ":"); // Substitui _ por :
    
    // Converter string MAC para bytes
    // Formato: "14:33:5C:38:BF:60"
    int values[6];
    int count = sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", 
                      &values[0], &values[1], &values[2], 
                      &values[3], &values[4], &values[5]);
    
    if (count != 6) {
        Serial.println("❌ Erro ao converter MAC: " + macStr);
        return false;
    }
    
    // Converter para uint8_t
    for (int i = 0; i < 6; i++) {
        macAddress[i] = (uint8_t)values[i];
    }
    
    Serial.println("✅ device_id convertido: " + deviceId + " → " + macStr);
    return true;
}

void MasterSlaveManager::processSlaveRelayCommands() {
    if (!initialized || WiFi.status() != WL_CONNECTED) {
        return;
    }
    
    Serial.println("\n📡 ========================================");
    Serial.println("📡 PROCESSANDO COMANDOS DE SLAVES DO SUPABASE");
    Serial.println("📡 ========================================");
    
    // ✅ ESTRATÉGIA: Buscar comandos pendentes com target_device_id (comandos para slaves)
    // Frontend cria comandos con device_id = master_device_id y target_device_id = slave_name
    // Buscamos comandos donde target_device_id no es NULL (comandos para slaves)
    String endpoint = "relay_commands?status=eq.pending&target_device_id=not.is.null&order=created_at.asc&limit=50";
    String fullUrl = String(SUPABASE_URL) + "/rest/v1/" + endpoint;
    
    Serial.println("🔍 Buscando comandos pendentes para slaves (target_device_id no NULL)...");
    
    // ✅ ESTRATÉGIA: Usar http.begin() direto (como DeviceRegistration)
    // Mais eficiente em memória que WiFiClientSecure
    if (ESP.getFreeHeap() < 30000) {
        Serial.println("⚠️ Heap baixo (" + String(ESP.getFreeHeap()) + " bytes) - pulando busca de comandos");
        return;
    }
    
    // ✅ PASSO 1: Verificar heap ANTES de criar HTTPClient
    uint32_t freeHeapBefore = ESP.getFreeHeap();
    uint32_t maxAllocHeap = ESP.getMaxAllocHeap(); // ✅ NOVO: Verificar fragmentación
    
    // ✅ MEJORADO: Verificar tanto heap libre como bloque máximo contiguo
    // SSL necesita bloques grandes contiguos, no solo heap libre total
    // ✅ AJUSTADO: Reducido umbral para memoria fragmentada
    uint32_t minHeapForSSL = 40000;  // Mínimo 40KB libre
    uint32_t minContiguousForSSL = 35000; // Mínimo 35KB contiguo
    
    if (freeHeapBefore < minHeapForSSL || maxAllocHeap < minContiguousForSSL) {
        // ✅ OTIMIZADO: Log solo cuando realmente es problema (no cada vez)
        static unsigned long lastWarning = 0;
        if (millis() - lastWarning > 30000) { // Solo cada 30s
            Serial.printf("⚠️ [SUPABASE] Memoria insuficiente: libre=%d, contiguo=%d (mínimos: %d, %d)\n", 
                         freeHeapBefore, maxAllocHeap, minHeapForSSL, minContiguousForSSL);
            lastWarning = millis();
        }
        return;
    }
    
    // ✅ PASSO 2: Delay adicional para liberação de memória SSL (ordem procedural)
    // ✅ CRÍTICO: SSL precisa de tempo para liberar memória de conexões anteriores
    if (freeHeapBefore < 60000 || maxAllocHeap < 45000) {
        delay(200);  // ✅ Mais tempo se heap está baixo o fragmentado
    } else {
        delay(50);   // ✅ Delay normal
    }
    
    // ✅ PASSO 4: Verificar heap novamente após delay
    uint32_t freeHeapAfterDelay = ESP.getFreeHeap();
    if (freeHeapAfterDelay < minHeapForSSL) {
        Serial.printf("⚠️ [PROCESSO SUPABASE] Heap ainda insuficiente após delay: %d bytes\n", 
                      freeHeapAfterDelay);
        return;
    }
    
    // ✅ NOVO: Usar Object Pool se disponível
    ObjectPoolManager* poolMgr = ObjectPoolManager::getInstance();
    bool usePool = (poolMgr && poolMgr->isInitialized());
    
    WiFiClientSecure* sslClient = nullptr;
    HTTPClient* httpClient = nullptr;
    bool usingPool = false;
    
    if (usePool) {
        // ✅ POOL: Adquirir do pool
        sslClient = poolMgr->acquireSSLClient();
        httpClient = poolMgr->acquireHTTPClient(sslClient);
        
        if (!sslClient || !httpClient) {
            Serial.println("⚠️ [PROCESSO SUPABASE] Pool esgotado - tentando modo legacy");
            usePool = false;
        } else {
            usingPool = true;
            sslClient->setInsecure(); // ✅ CRÍTICO: Configurar SSL insecure
        }
    }
    
    // ✅ FALLBACK: Criar HTTPClient e WiFiClientSecure local se pool não disponível
    HTTPClient localHttpClient;
    WiFiClientSecure localSecureClient;
    if (!usePool) {
        httpClient = &localHttpClient;
        // ✅ CRÍTICO: Configurar SSL insecure para fallback
        localSecureClient.setInsecure();
        sslClient = &localSecureClient;
    }
    
    // ✅ PASSO 5: Iniciar conexão SSL COM VERIFICAÇÃO (ordem procedural)
    bool connectionStarted = false;
    if (sslClient) {
        // ✅ Usar SSL client (do pool ou fallback)
        connectionStarted = httpClient->begin(*sslClient, fullUrl);
    } else {
        // ✅ ÚLTIMO RECURSO: begin() direto (cria SSL internamente, menos eficiente)
        connectionStarted = httpClient->begin(fullUrl);
    }
    
    if (!connectionStarted) {
        Serial.println("❌ [PROCESSO SUPABASE] Falha ao iniciar conexão HTTP/SSL");
        Serial.println("   URL: " + fullUrl);
        Serial.printf("   Heap: %d bytes\n", ESP.getFreeHeap());
        Serial.printf("   Max block: %d bytes\n", ESP.getMaxAllocHeap());
        // ✅ Liberar pools se estavam em uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        return;
    }
    
    // ✅ PASSO 6: Configurar timeouts e headers (ordem procedural)
    httpClient->setConnectTimeout(10000);  // 10s conexão (reduzido de 15s)
    httpClient->setTimeout(15000);         // 15s total (reduzido de 20s)
    httpClient->addHeader("Authorization", "Bearer " + String(SUPABASE_ANON_KEY));
    httpClient->addHeader("apikey", String(SUPABASE_ANON_KEY));
    httpClient->addHeader("Accept", "application/json");
    
    // ✅ PASSO 7: Fazer GET request COM VERIFICAÇÃO (ordem procedural)
    Serial.println("📡 [PROCESSO SUPABASE] Enviando requisição GET...");
    int httpCode = httpClient->GET();
    
    // ✅ PASSO 8: Verificar código HTTP ANTES de ler resposta (ordem procedural)
    if (httpCode <= 0) {
        Serial.println("❌ [PROCESSO SUPABASE] Erro HTTP: " + String(httpCode));
        Serial.println("   Erro: " + httpClient->errorToString(httpCode));
        Serial.println("   Heap: " + String(ESP.getFreeHeap()) + " bytes");
        httpClient->end();  // ✅ FECHAR conexão sempre
        // ✅ Dar tempo para SSL liberar memória
        vTaskDelay(pdMS_TO_TICKS(50));
        // ✅ Liberar pools se estavam em uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        return;
    }
    
    if (httpCode != 200) {
        Serial.println("❌ [PROCESSO SUPABASE] HTTP " + String(httpCode));
        if (httpCode > 0) {
            String errorResponse = httpClient->getString();
            Serial.println("   Resposta: " + errorResponse.substring(0, 200));  // Limitar tamanho
        }
        httpClient->end();  // ✅ FECHAR conexão sempre
        // ✅ Dar tempo para SSL liberar memória
        vTaskDelay(pdMS_TO_TICKS(50));
        // ✅ Liberar pools se estavam em uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        return;
    }
    
    // ✅ PASSO 9: Verificar tamanho da resposta ANTES de ler (ordem procedural)
    int contentLength = httpClient->getSize();
    if (contentLength <= 0) {
        Serial.println("⚠️ [PROCESSO SUPABASE] Resposta vazia ou tamanho desconhecido");
        httpClient->end();  // ✅ FECHAR conexão
        // ✅ Dar tempo para SSL liberar memória
        vTaskDelay(pdMS_TO_TICKS(50));
        // ✅ Liberar pools se estavam em uso
        if (usingPool && poolMgr) {
            poolMgr->releaseHTTPClient(httpClient);
            poolMgr->releaseSSLClient(sslClient);
        }
        return;
    }
    
    Serial.printf("📥 [PROCESSO SUPABASE] Resposta recebida: %d bytes\n", contentLength);
    
    // ✅ PASSO 10: Ler resposta (ordem procedural)
    String response = httpClient->getString();
    
    // ✅ PASSO 11: FECHAR conexão IMEDIATAMENTE após ler (ordem procedural)
    httpClient->end();
    // ✅ Dar tempo para SSL liberar memória
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // ✅ Liberar pools se estavam em uso
    if (usingPool && poolMgr) {
        poolMgr->releaseHTTPClient(httpClient);
        poolMgr->releaseSSLClient(sslClient);
    }
    
    // ✅ PASSO 10: Verificar se resposta não está vazia (ordem procedural)
    if (response.length() == 0) {
        Serial.println("❌ [PROCESSO SUPABASE] Resposta vazia após getString()");
        Serial.printf("   Content-Length esperado: %d bytes\n", contentLength);
        return;
    }
    
    // ✅ PASSO 11: Verificar formato JSON ANTES de parsear (ordem procedural)
    if (response.charAt(0) != '[' && response.charAt(0) != '{') {
        Serial.println("❌ [PROCESSO SUPABASE] Resposta não é JSON válido");
        Serial.printf("   Primeiro caractere: '%c' (esperado: '[' ou '{')\n", response.charAt(0));
        Serial.printf("   Primeiros 100 chars: %s\n", response.substring(0, 100).c_str());
        return;
    }
    
    // ✅ PASSO 12: Parsear JSON COM TRATAMENTO DE ERRO (ordem procedural)
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, response);
    
    if (error) {
        Serial.println("❌ [PROCESSO SUPABASE] Erro ao parsear JSON: " + String(error.c_str()));
        Serial.printf("   Tamanho da resposta: %d bytes\n", response.length());
        Serial.printf("   Primeiros 200 chars: %s\n", response.substring(0, 200).c_str());
        
        // ✅ Log de heap após erro
        uint32_t freeHeapAfter = ESP.getFreeHeap();
        Serial.printf("   Heap: %d → %d (usado: %d bytes)\n", 
                      freeHeapBefore, freeHeapAfter, freeHeapBefore - freeHeapAfter);
        return;
    }
    
    // ✅ PASSO 13: Verificar heap após parsear (ordem procedural)
    uint32_t freeHeapAfter = ESP.getFreeHeap();
    uint32_t heapUsed = freeHeapBefore - freeHeapAfter;
    if (heapUsed > 20000) {
        Serial.printf("⚠️ [PROCESSO SUPABASE] Muito heap usado: %d bytes\n", heapUsed);
    }
    
    JsonArray commandsArray = doc.as<JsonArray>();
    int totalCommands = commandsArray.size();
    
    if (totalCommands == 0) {
        Serial.println("✅ Nenhum comando pendente");
        Serial.println("========================================\n");
        return;
    }
    
    Serial.println("📥 Encontrados " + String(totalCommands) + " comando(s) pendente(s) para slaves");
    
    // Todos os comandos já são para slaves (filtrados por target_device_id)
    int slaveCommandCount = 0;
    int processedCount = 0;
    int successCount = 0;
    int failCount = 0;
    
    for (int i = 0; i < totalCommands; i++) {
        JsonObject cmd = commandsArray[i];
        String targetDeviceId = cmd["target_device_id"].as<String>(); // Nombre del slave
        
        if (targetDeviceId.length() == 0) {
            continue; // Pular si no tiene target_device_id
        }
        
        slaveCommandCount++;
        
        int cmdId = cmd["id"];
        int relayNumber = cmd["relay_number"];
        String action = cmd["action"].as<String>();
        int durationSeconds = cmd["duration_seconds"] | 0;
        
        Serial.println("\n📋 Comando de Slave " + String(slaveCommandCount) + " (total pendentes: " + String(totalCommands) + "):");
        Serial.println("   🆔 ID: " + String(cmdId));
        Serial.println("   📡 Target Device (Slave Name): " + targetDeviceId);
        Serial.println("   🔌 Relé: " + String(relayNumber));
        Serial.println("   ⚡ Ação: " + action);
        if (durationSeconds > 0) {
            Serial.println("   ⏱️ Duração: " + String(durationSeconds) + "s");
        }
        
        // ✅ Buscar slave por nombre (target_device_id = slave_name)
        // Buscar en la lista de slaves confiables
        const uint8_t* targetMac = nullptr;
        auto trustedSlaves = getAllTrustedSlaves();
        
        for (const auto& slave : trustedSlaves) {
            if (slave.deviceName == targetDeviceId || 
                slave.deviceName.equalsIgnoreCase(targetDeviceId)) {
                targetMac = slave.macAddress;
                Serial.println("   ✅ Slave encontrado: " + slave.deviceName);
                Serial.println("   💡 MAC: " + ESPNowController::macToString(targetMac));
                break;
            }
        }
        
        if (!targetMac) {
            Serial.println("   ❌ Slave não encontrado na lista confiável: " + targetDeviceId);
            Serial.println("   💡 Use 'list' para ver slaves disponíveis");
            failCount++;
            continue;
        }
        
        // Verificar si slave está online
        TrustedSlave* slave = getTrustedSlave(targetMac);
        if (!slave || !slave->isOnline()) {
            Serial.println("   ⚠️ Slave offline ou não encontrado");
            failCount++;
            continue;
        }
        
        // Enviar comando via ESP-NOW
        bool success = sendRelayCommandToSlave(targetMac, relayNumber, action, durationSeconds, cmdId);
        
        if (success) {
            successCount++;
            Serial.println("   ✅ Comando enviado com sucesso!");
            
            // Atualizar status no Supabase para 'sent'
            // Usar PATCH para atualizar
            String updateUrl = String(SUPABASE_URL) + "/rest/v1/relay_commands?id=eq." + String(cmdId);
            HTTPClient updateClient;
            
            // ✅ Usar begin() direto (mais eficiente em memória)
            updateClient.begin(updateUrl);
            updateClient.setConnectTimeout(15000);
            updateClient.setTimeout(20000);
            updateClient.addHeader("Authorization", "Bearer " + String(SUPABASE_ANON_KEY));
            updateClient.addHeader("apikey", String(SUPABASE_ANON_KEY));
            updateClient.addHeader("Content-Type", "application/json");
            updateClient.addHeader("Prefer", "return=minimal");
            
            DynamicJsonDocument updateDoc(128);
            updateDoc["status"] = "sent";
            String updatePayload;
            serializeJson(updateDoc, updatePayload);
            
            int updateCode = updateClient.PATCH(updatePayload);
            updateClient.end();
            
            if (updateCode == 200 || updateCode == 204) {
                Serial.println("   ✅ Status atualizado no Supabase");
            } else {
                Serial.println("   ⚠️ Falha ao atualizar status (HTTP " + String(updateCode) + ")");
            }
        } else {
            failCount++;
            Serial.println("   ❌ Falha ao enviar comando");
        }
        
        processedCount++;
        delay(100); // Pequeno delay entre comandos
    }
    
    if (slaveCommandCount == 0) {
        Serial.println("✅ Nenhum comando pendente para slaves");
        Serial.println("========================================\n");
        return;
    }
    
    Serial.println("\n📊 ========================================");
    Serial.println("📊 RESUMO DO PROCESSAMENTO");
    Serial.println("📊 ========================================");
    Serial.println("📥 Comandos pendentes totais: " + String(totalCommands));
    Serial.println("📡 Comandos de slaves: " + String(slaveCommandCount));
    Serial.println("✅ Processados com sucesso: " + String(successCount));
    Serial.println("❌ Falhas: " + String(failCount));
    Serial.println("========================================\n");
}

// ===== 🎯 CACHE NVS DE ESTADOS DE SLAVES =====

bool MasterSlaveManager::saveSlaveRelayStatesToNVS() {
    SlaveRelayStatesCache cache = {};
    cache.timestamp = millis();
    cache.version = 1;
    cache.numSlaves = 0;
    
    // ✅ Proteger acesso com mutex
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em saveSlaveRelayStatesToNVS()");
        return false;
    }
    
    // Coletar estados de todos os slaves
    int stateIndex = 0;
    for (const auto& slave : trustedSlaves) {
        if (stateIndex >= 32) break; // Limite de 32 estados
        
        for (int i = 0; i < slave.numRelays && i < 8; i++) {
            if (stateIndex >= 32) break;
            
            CachedSlaveRelayState& cachedState = cache.states[stateIndex];
            memcpy(cachedState.slaveMac, slave.macAddress, 6);
            cachedState.relayNumber = i;
            cachedState.state = slave.relayStates[i].state ? 1 : 0;
            cachedState.hasTimer = slave.relayStates[i].hasTimer ? 1 : 0;
            cachedState.remainingTime = slave.relayStates[i].remainingTime;
            cachedState.timestamp = slave.relayStates[i].lastUpdate;
            cachedState.isStale = (slave.relayStates[i].lastUpdate == 0 || 
                                   (millis() - slave.relayStates[i].lastUpdate) > 60000) ? 1 : 0;
            
            stateIndex++;
        }
        cache.numSlaves++;
    }
    
    xSemaphoreGive(trustedSlavesMutex);
    
    // Calcular checksum
    uint8_t checksum = 0;
    uint8_t* data = (uint8_t*)&cache;
    for (size_t i = 0; i < sizeof(SlaveRelayStatesCache) - 1; i++) {
        checksum ^= data[i];
    }
    cache.checksum = checksum;
    
    // Guardar em NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open("slave_states", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        Serial.println("❌ Erro ao abrir NVS para guardar cache de slaves: " + String(esp_err_to_name(err)));
        return false;
    }
    
    err = nvs_set_blob(handle, "relay_states_cache", &cache, sizeof(SlaveRelayStatesCache));
    if (err != ESP_OK) {
        Serial.println("❌ Erro ao guardar cache em NVS: " + String(esp_err_to_name(err)));
        nvs_close(handle);
        return false;
    }
    
    err = nvs_commit(handle);
    nvs_close(handle);
    
    if (err == ESP_OK) {
        Serial.printf("💾 Cache de estados de slaves guardado em NVS (%d estados, %d slaves)\n", 
                     stateIndex, cache.numSlaves);
        return true;
    } else {
        Serial.println("❌ Erro ao fazer commit em NVS: " + String(esp_err_to_name(err)));
        return false;
    }
}

bool MasterSlaveManager::loadSlaveRelayStatesFromNVS() {
    SlaveRelayStatesCache cache = {};
    
    // Carregar de NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open("slave_states", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false; // Nenhum cache encontrado (primeira inicialização)
    }
    
    size_t required_size = sizeof(SlaveRelayStatesCache);
    err = nvs_get_blob(handle, "relay_states_cache", &cache, &required_size);
    nvs_close(handle);
    
    if (err != ESP_OK) {
        return false; // Nenhum cache encontrado
    }
    
    // Validar checksum
    uint8_t checksum = 0;
    uint8_t* data = (uint8_t*)&cache;
    for (size_t i = 0; i < sizeof(SlaveRelayStatesCache) - 1; i++) {
        checksum ^= data[i];
    }
    
    if (checksum != cache.checksum) {
        Serial.println("❌ Cache inválido (checksum incorreto)");
        return false;
    }
    
    // ✅ Proteger acesso com mutex
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em loadSlaveRelayStatesFromNVS()");
        return false;
    }
    
    // Aplicar estados cacheados aos slaves conhecidos
    int loadedCount = 0;
    for (int i = 0; i < cache.numSlaves && i < 32; i++) {
        const CachedSlaveRelayState& cachedState = cache.states[i];
        
        // Buscar slave por MAC
        TrustedSlave* slave = findTrustedSlaveUnsafe(cachedState.slaveMac);
        if (slave && cachedState.relayNumber < 8) {
            slave->relayStates[cachedState.relayNumber].state = (cachedState.state == 1);
            slave->relayStates[cachedState.relayNumber].hasTimer = (cachedState.hasTimer == 1);
            slave->relayStates[cachedState.relayNumber].remainingTime = cachedState.remainingTime;
            slave->relayStates[cachedState.relayNumber].lastUpdate = cachedState.timestamp;
            
            // ✅ Marcar como "potencialmente desatualizado" se isStale = 1
            if (cachedState.isStale == 1) {
                // Timestamp antigo indica que pode estar desatualizado
                unsigned long age = (cachedState.timestamp == 0) ? ULONG_MAX : (millis() - cachedState.timestamp);
                if (age > 60000) {
                    // Mais de 1 minuto = potencialmente desatualizado
                    slave->relayStates[cachedState.relayNumber].lastUpdate = 0; // Forçar atualização
                }
            }
            
            loadedCount++;
        }
    }
    
    xSemaphoreGive(trustedSlavesMutex);
    
    if (loadedCount > 0) {
        Serial.printf("✅ Cache carregado: %d estados de %d slaves\n", loadedCount, cache.numSlaves);
        unsigned long cacheAge = (cache.timestamp == 0) ? ULONG_MAX : (millis() - cache.timestamp);
        Serial.printf("   Idade do cache: %lu ms (%.1f minutos)\n", cacheAge, cacheAge / 60000.0);
    }
    
    return loadedCount > 0;
}

bool MasterSlaveManager::saveTrustedPeersToNVS() {
    TrustedPeersCache cache = {};
    cache.timestamp = millis();
    cache.version = 1;

    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    cache.numPeers = trustedSlaves.size() > 8 ? 8 : static_cast<uint8_t>(trustedSlaves.size());
    for (uint8_t i = 0; i < cache.numPeers; i++) {
        memcpy(cache.peers[i].mac, trustedSlaves[i].macAddress, 6);
        strncpy(cache.peers[i].deviceName, trustedSlaves[i].deviceName.c_str(), sizeof(cache.peers[i].deviceName) - 1);
        String deviceId = "ESP32_SLAVE_" + ESPNowController::macToString(trustedSlaves[i].macAddress);
        deviceId.replace(":", "_");
        strncpy(cache.peers[i].deviceId, deviceId.c_str(), sizeof(cache.peers[i].deviceId) - 1);
    }
    xSemaphoreGive(trustedSlavesMutex);

    uint8_t checksum = 0;
    uint8_t* data = reinterpret_cast<uint8_t*>(&cache);
    for (size_t i = 0; i < sizeof(TrustedPeersCache) - 1; i++) {
        checksum ^= data[i];
    }
    cache.checksum = checksum;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("hidro_state", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_set_blob(handle, "trusted_peers", &cache, sizeof(TrustedPeersCache));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool MasterSlaveManager::loadTrustedPeersFromNVS() {
    TrustedPeersCache cache = {};
    nvs_handle_t handle;
    esp_err_t err = nvs_open("hidro_state", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t required = sizeof(TrustedPeersCache);
    err = nvs_get_blob(handle, "trusted_peers", &cache, &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t checksum = 0;
    uint8_t* data = reinterpret_cast<uint8_t*>(&cache);
    for (size_t i = 0; i < sizeof(TrustedPeersCache) - 1; i++) {
        checksum ^= data[i];
    }
    if (checksum != cache.checksum || cache.numPeers == 0) {
        return false;
    }

    int restored = 0;
    for (uint8_t i = 0; i < cache.numPeers && i < 8; i++) {
        if (addTrustedSlave(cache.peers[i].mac, String(cache.peers[i].deviceName), "RelayCommandBox")) {
            restored++;
        }
    }
    return restored > 0;
}

void MasterSlaveManager::validateCachedStates() {
    // ✅ Verificar se há estados cacheados que precisam validação
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    
    bool needsValidation = false;
    unsigned long now = millis();
    
    for (auto& slave : trustedSlaves) {
        for (int i = 0; i < slave.numRelays && i < 8; i++) {
            // ✅ Se lastUpdate = 0 ou muito antigo (> 1 minuto), precisa validação
            if (slave.relayStates[i].lastUpdate == 0 || 
                (now - slave.relayStates[i].lastUpdate) > 60000) {
                needsValidation = true;
                break;
            }
        }
        if (needsValidation) break;
    }
    
    xSemaphoreGive(trustedSlavesMutex);
    
    if (needsValidation) {
        Serial.println("🔄 Estados cacheados precisam validação - solicitando atualização...");
        // ✅ Solicitar atualização via ESP-NOW (mas não bloquear)
        requestAllSlavesRelayStatus();
    } else {
        Serial.println("✅ Estados cacheados são recentes (< 1 minuto)");
    }
}

