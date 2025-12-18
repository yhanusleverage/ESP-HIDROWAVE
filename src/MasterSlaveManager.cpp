#include "MasterSlaveManager.h"
#include "Config.h"
#include "ObjectPoolManager.h"  // ✅ Object Pool Pattern
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "ESPNowTypes.h"  // ✅ Para SlaveRelayStatesCache

// Instância estática para callbacks
MasterSlaveManager* MasterSlaveManager::instance = nullptr;

MasterSlaveManager::MasterSlaveManager(ESPNowController* espNowController) 
    : espNowController(espNowController), initialized(false),
      totalPingsReceived(0), totalPongsSent(0), totalAcksSent(0), 
      totalAcksReceived(0), totalErrors(0), commandIdCounter(0),  // ⭐ Inicializar contador
      processingStatusResponse(false) {  // ✅ Proteção contra loop infinito
    instance = this;
    
    // ✅ PROTEÇÃO MULTI-CORE: Criar mutex para proteger trustedSlaves
    trustedSlavesMutex = xSemaphoreCreateMutex();
    if (trustedSlavesMutex == NULL) {
        Serial.println("❌ Erro ao criar mutex para trustedSlaves!");
    } else {
        Serial.println("✅ Mutex criado para proteção multi-core");
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
    
    // ⭐ POTENCIA MÁXIMA: Chamar callback SEMPRE que um slave é adicionado
    // Isso garante sincronização entre trustedSlaves e knownSlaves
    if (slaveDiscoveredCallback) {
        Serial.println("📢 Chamando callback de descoberta...");
        slaveDiscoveredCallback(macAddress, newSlave.deviceName, newSlave.deviceType);
        Serial.println("✅ Callback de descoberta chamado");
    } else {
        Serial.println("⚠️ AVISO: slaveDiscoveredCallback NÃO está configurado!");
        Serial.println("⚠️ O slave foi adicionado a trustedSlaves mas NÃO será adicionado a knownSlaves");
        Serial.println("⚠️ Isso pode causar problemas com comandos 'relay on_all'");
    }
    
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
    
    xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex antes de retornar
    return true;
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
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de ler
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em getTrustedSlave()");
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
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de ler
    // ⚠️ LOGGING REDUZIDO: Só loggar em caso de erro para evitar spam no Serial
    // ✅ CORREÇÃO CRÍTICA: Otimizar cópia para evitar falta de memória
    
    TickType_t startTime = xTaskGetTickCount();
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // ✅ OTIMIZAÇÃO: Reservar espaço antes de copiar para evitar realocações
        std::vector<TrustedSlave> copy;
        copy.reserve(trustedSlaves.size());  // ✅ Reservar espaço exato
        
        // ✅ OTIMIZAÇÃO: Copiar manualmente para melhor controle de memória
        for (const auto& slave : trustedSlaves) {
            copy.push_back(slave);  // ✅ Copy constructor otimizado
        }
        
        xSemaphoreGive(trustedSlavesMutex);  // Liberar mutex
        return copy;
    } else {
        // ⚠️ Só loggar se houver timeout (erro)
        TickType_t waitTime = xTaskGetTickCount() - startTime;
        Serial.printf("❌ [getAllTrustedSlaves] TIMEOUT após %lu ms!\n", waitTime * portTICK_PERIOD_MS);
        Serial.println("   ⚠️ Possíveis causas:");
        Serial.println("      - Mutex está bloqueado por outra função");
        Serial.println("      - Deadlock entre cores");
        Serial.println("      - Função está demorando muito para liberar mutex");
        return std::vector<TrustedSlave>();  // Retornar vazio se timeout
    }
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

uint32_t MasterSlaveManager::sendRelayCommandToSlave(const uint8_t* macAddress, int relayNumber, const String& action, int duration, int supabaseCommandId, bool updateStatus) {
    // ✅ MUDANÇA 1: Retornar 0 em vez de false (compatível: 0 = false, >0 = true)
    if (!initialized || !espNowController) return 0;
    
    TrustedSlave* slave = getTrustedSlave(macAddress);
    if (!slave) {
        Serial.println("❌ Slave não encontrado na lista confiável: " + ESPNowController::macToString(macAddress));
        return 0;  // ✅ MUDANÇA 2: 0 em vez de false
    }
    
    // 🚨 NOVO: Verificar se slave está OFFLINE
    if (!slave->isOnline()) {
        Serial.println("\n⏸️ ========================================");
        Serial.println("⏸️ SLAVE OFFLINE - COMANDO P");
        Serial.println("⏸️ ========================================");
        Serial.println("📡 Destino: " + ESPNowController::macToString(macAddress));
        Serial.println("📝 Nome: " + slave->deviceName);
        Serial.println("🔌 Relé: " + String(relayNumber));
        Serial.println("⚡ Ação: " + action);
        Serial.println("💾 Comando será enviado quando slave voltar ONLINE");
        Serial.println("========================================\n");
        
        // 🚨 NOVO: Adicionar à fila de comandos pendentes (não retry, mas pendente)
        uint32_t commandId = generateCommandId();
        addToRetryQueue(macAddress, relayNumber, action, duration, commandId, supabaseCommandId);
        
        // ✅ MUDANÇA 3: Retornar commandId mesmo se offline (para mapeamento futuro)
        return commandId;  // Retorna ID para que possa ser mapeado quando enviado
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
    
    // Tentar enviar o comando
    bool success = espNowController->sendRelayCommand(macAddress, relayNumber, action, duration);
    
    if (success) {
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
        return commandId;  // Retorna ID para mapeamento
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

bool MasterSlaveManager::requestSlaveStatus(const uint8_t* macAddress) {
    if (!initialized || !espNowController) return false;
    
    // ✅ Proteção contra loop infinito: não solicitar status se já está processando uma resposta
    if (processingStatusResponse) {
        Serial.println("⚠️ Ignorando solicitação de status - já processando resposta de status");
        return false;
    }
    
    // Enviar comando de status para todos os relés
    bool success = true;
    TrustedSlave* slave = getTrustedSlave(macAddress);
    if (slave) {
        for (int i = 0; i < slave->numRelays; i++) {
            if (!espNowController->sendRelayCommand(macAddress, i, "status", 0)) {
                success = false;
            }
        }
    }
    
    if (success) {
        Serial.println("📊 Solicitação de status enviada para Slave: " + ESPNowController::macToString(macAddress));
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
        
        // Solicitar status de todos os relés deste slave
        bool slaveSuccess = true;
        for (int i = 0; i < slave.numRelays; i++) {
            if (espNowController->sendRelayCommand(slave.macAddress, i, "status", 0)) {
                requestsSent++;
                totalRelays++;
                delay(5); // Delay reduzido (5ms) para ser mais rápido
            } else {
                slaveSuccess = false;
                requestsFailed++;
            }
        }
        
        if (slaveSuccess) {
            Serial.println("   ✅ " + String(slave.numRelays) + " relés");
        } else {
            Serial.println("   ⚠️ Algumas falhas");
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

void MasterSlaveManager::setSupabaseRelayStateCallback(std::function<void(const String& masterDeviceId, const String& slaveMacAddress, const String& slaveDeviceId, int relayNumber, bool state, bool hasTimer, int remainingTime)> callback) {
    this->supabaseRelayStateCallback = callback;
}

// ===== MÉTODOS PRIVADOS =====

void MasterSlaveManager::processPingReceived(const uint8_t* senderMac, uint32_t pingId) {
    Serial.println("\n🏓 ========================================");
    Serial.println("🏓 PING RECEBIDO DO SLAVE!");
    Serial.println("🏓 ========================================");
    Serial.println("📥 De: " + ESPNowController::macToString(senderMac));
    Serial.println("🆔 Ping ID: " + String(pingId));
    Serial.println("⏰ Timestamp: " + String(millis() / 1000) + "s");
    
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de verificar/adicionar
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em processPingReceived()");
        return;
    }
    
    // Verificar se Slave está na lista confiável (usar versión unsafe porque ya tenemos mutex)
    TrustedSlave* slave = findTrustedSlaveUnsafe(senderMac);
    bool wasNewSlave = false;
    
    if (!slave) {
        xSemaphoreGive(trustedSlavesMutex);  // Liberar antes de addTrustedSlave (que toma mutex)
        
        Serial.println("\n🆕 SLAVE DESCONHECIDO - ADICIONANDO À LISTA CONFIÁVEL");
        String genericName = "Slave-" + ESPNowController::macToString(senderMac).substring(12);
        addTrustedSlave(senderMac, genericName, "RelayBox");
        
        // Tomar mutex de nuevo para buscar
        if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            slave = findTrustedSlaveUnsafe(senderMac);
            wasNewSlave = true;
        } else {
            Serial.println("⚠️ Timeout ao obter mutex após addTrustedSlave()");
            return;
        }
        
        // ⭐ CORREÇÃO: Adicionar à knownSlaves imediatamente quando recebe PING
        // Isso garante que o slave apareça na lista mesmo antes de receber DEVICE_INFO
        if (slaveDiscoveredCallback) {
            Serial.println("📢 Adicionando slave à knownSlaves (nome temporário)...");
            slaveDiscoveredCallback(senderMac, genericName, "RelayBox");
        }
        
        // ⭐ POTENCIA MÁXIMA: Solicitar DEVICE_INFO automáticamente para obtener información completa
        Serial.println("\n📋 === SOLICITANDO DEVICE_INFO AUTOMÁTICAMENTE ===");
        Serial.println("🎯 Usando DEVICE_INFO como fuente principal de información");
        if (espNowController) {
            // Usar handshake para solicitar información completa del slave
            espNowController->initiateHandshake(senderMac);
            Serial.println("✅ Handshake iniciado - Slave debería responder con DEVICE_INFO");
        }
        Serial.println("==================================================\n");
    } else {
        // ⭐ POTENCIA MÁXIMA: Si el slave tiene nombre genérico, solicitar DEVICE_INFO para actualizar
        if (slave->deviceName.startsWith("Slave-") || slave->deviceName == "Unknown") {
            Serial.println("\n📋 === SLAVE CON INFORMACIÓN INCOMPLETA ===");
            Serial.println("🔄 Solicitando DEVICE_INFO para actualizar información...");
            if (espNowController) {
                espNowController->initiateHandshake(senderMac);
                Serial.println("✅ Handshake iniciado para obtener información completa");
            }
            Serial.println("============================================\n");
        }
    }
    
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
                    
                    // Remover e re-adicionar peer com canal correto
                    esp_now_del_peer(senderMac);
                    String slaveName = slave->deviceName.isEmpty() ? "Slave-" + ESPNowController::macToString(senderMac).substring(12) : slave->deviceName;
                    espNowController->addPeerWithChannel(senderMac, currentMasterChannel, slaveName);
                }
            }
        }
        
        // Responder automaticamente com PONG
        Serial.println("\n🏓 Enviando PONG de resposta...");
        bool pongSent = espNowController->sendPing(senderMac);  // ESPNowController já responde PING com PONG
        
        if (pongSent) {
            slave->pongsSent++;
            slave->lastPongId++;
            Serial.println("✅ PONG enviado com sucesso!");
            Serial.println("🔗 Conectividade confirmada");
            
            // Atualizar status para ONLINE se ainda não estiver
            if (slave->status == SlaveStatus::PING_RECEIVED || slave->status == SlaveStatus::DISCOVERED) {
                slave->status = SlaveStatus::ONLINE;
                Serial.println("📊 Status atualizado: ONLINE");
                
                // ✅ CRÍTICO: Atualizar lastSeen quando recebe PING
                slave->updateLastSeen();
                
                // Chamar callback de online (atualiza knownSlaves)
                if (slaveOnlineCallback) {
                    slaveOnlineCallback(senderMac, slave->deviceName);
                }
            }
        } else {
            slave->messagesLost++;
            Serial.println("❌ Falha ao enviar PONG");
        }
        
        Serial.println("📊 Estatísticas do Slave:");
        Serial.println("   PINGs recebidos: " + String(slave->pingsReceived));
        Serial.println("   PINGs enviados: " + String(slave->pingsSent));
        Serial.println("   PONGs recebidos: " + String(slave->pongsReceived));
        Serial.println("   PONGs enviados: " + String(slave->pongsSent));
        Serial.println("   Mensagens recebidas: " + String(slave->messagesReceived));
        Serial.println("   Mensagens perdidas: " + String(slave->messagesLost));
        Serial.println("   Último contato: " + String(slave->getTimeSinceLastSeen() / 1000) + "s atrás");
    }
    
    // Chamar callback se definido
    if (pingReceivedCallback) {
        pingReceivedCallback(senderMac, pingId);
    }
    
    Serial.println("========================================\n");
}

void MasterSlaveManager::processPongReceived(const uint8_t* senderMac, uint32_t pongId) {
    // ✅ REDUZIDO: Log removido (PONGs são frequentes)
    // Serial.println("🏓 PONG recebido de: " + ESPNowController::macToString(senderMac) + " (ID: " + String(pongId) + ")");
    
    TrustedSlave* slave = getTrustedSlave(senderMac);
    if (slave) {
        bool wasOffline = !slave->isOnline();
        slave->updateLastSeen();
        slave->pongsReceived++;  // ✅ CORREÇÃO: Incrementar PONGs recebidos
        slave->messagesReceived++;
        slave->status = SlaveStatus::ONLINE; // ✅ CRÍTICO: Marcar como online quando recebe PONG
        
        // 🚨 NOVO: Se slave estava offline e voltou online, enviar comandos pendentes
        if (wasOffline) {
            Serial.println("🔄 Slave voltou online: " + ESPNowController::macToString(senderMac));
            sendPendingCommandsToSlave(senderMac);
        }
        
        // ✅ CORREÇÃO CRÍTICA: Sincronizar knownSlaves
        if (slaveOnlineCallback) {
            slaveOnlineCallback(senderMac, slave->deviceName);
        }
        
        // Chamar callback se definido
        if (pongReceivedCallback) {
            pongReceivedCallback(senderMac, pongId);
        }
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
    Serial.println("\n📋 ========================================");
    Serial.println("📋 INFORMAÇÕES DO SLAVE RECEBIDAS!");
    Serial.println("📋 ========================================");
    Serial.println("📥 De: " + ESPNowController::macToString(senderMac));
    Serial.println("📝 Nome: " + deviceName);
    Serial.println("🏷️ Tipo: " + deviceType);
    Serial.println("🔌 Relés: " + String(numRelays));
    Serial.println("✅ Operacional: " + String(operational ? "Sim" : "Não"));
    Serial.println("📶 Canal WiFi: " + String(wifiChannel > 0 ? String(wifiChannel) : "Desconhecido"));
    
    // 🔍 DEBUG: Estado WiFi do Master
    wifi_mode_t wifiMode = WiFi.getMode();
    bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    uint8_t masterChannel;
    wifi_second_chan_t secondChan;
    esp_wifi_get_channel(&masterChannel, &secondChan);
    Serial.println("\n🔍 [DEBUG] Estado WiFi do Master:");
    Serial.println("   📶 Modo WiFi: " + String(wifiMode == WIFI_AP_STA ? "AP+STA" : wifiMode == WIFI_STA ? "STA" : wifiMode == WIFI_AP ? "AP" : "OFF"));
    Serial.println("   📡 WiFi conectado: " + String(wifiConnected ? "SIM ✅" : "NÃO ❌"));
    Serial.println("   📶 Canal Master: " + String(masterChannel));
    Serial.println("   📶 Canal Slave: " + String(wifiChannel));
    if (wifiChannel > 0 && masterChannel != wifiChannel) {
        Serial.println("   ⚠️ CONFLITO DE CANAL DETECTADO!");
        Serial.println("   💡 Master no canal " + String(masterChannel) + ", Slave no canal " + String(wifiChannel));
        if (wifiConnected) {
            Serial.println("   ⚠️ WiFi conectado - Master NÃO pode mudar de canal");
            Serial.println("   💡 Slave deve sincronizar para canal " + String(masterChannel));
        }
    }
    
    // ⭐ CORREÇÃO CRÍTICA: Adicionar Slave como peer ESP-NOW se não existir
    // 🚨 CRÍTICO: Usar canal WiFi do Slave se disponível
    uint8_t peerChannel = wifiChannel;
    if (peerChannel == 0) {
        // Se canal não foi fornecido, detectar canal real em que recebemos a mensagem
        uint8_t currentChannel;
        wifi_second_chan_t secondChan;
        esp_wifi_get_channel(&currentChannel, &secondChan);
        peerChannel = currentChannel;
        Serial.println("⚠️ Canal WiFi não fornecido, usando canal detectado: " + String(peerChannel));
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
            
            // Disparar callback de descoberta
            if (slaveDiscoveredCallback) {
                Serial.println("📢 Disparando callback de descoberta...");
                slaveDiscoveredCallback(senderMac, deviceName, deviceType);
            }
        } else {
            Serial.println("❌ Falha ao adicionar Slave à lista confiável");
        }
        Serial.println("===============================\n");
    } else {
        // Slave já existe, apenas atualizar
        // Actualizar directamente (ya tenemos mutex)
        slave->numRelays = numRelays;
        slave->operational = operational;
        slave->updateLastSeen();
        slave->status = SlaveStatus::ONLINE;
        if (!deviceName.isEmpty()) {
            slave->deviceName = deviceName;
        }
        if (!deviceType.isEmpty()) {
            slave->deviceType = deviceType;
        }
        
        // Guardar datos para callbacks (antes de liberar mutex)
        String deviceNameForCallback = slave->deviceName;
        
        xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex ANTES de callbacks
        
        // ✅ CORREÇÃO: NÃO chamar slaveDiscoveredCallback para slaves existentes
        // Isso evita logs repetidos de "NOVO SLAVE DESCOBERTO!"
        // O callback só deve ser chamado quando o slave é realmente novo
    }
    
    // Atualizar informações detalhadas do Slave
    if (slave) {
        bool wasOffline = !slave->isOnline();
        slave->numRelays = numRelays;
        slave->operational = operational;
        slave->messagesReceived++;
        
        // Actualizar todos los datos ANTES de liberar mutex
        slave->status = SlaveStatus::ONLINE;
        slave->updateLastSeen(); // ✅ CRÍTICO: Atualizar lastSeen quando recebe mensagem
        
        // 🚨 CRÍTICO: Guardar canal WiFi do Slave
        if (wifiChannel > 0 && wifiChannel <= 13) {
            if (slave->wifiChannel == 0 || slave->wifiChannel != wifiChannel) {
                Serial.println("💾 Atualizando canal WiFi do Slave: " + String(slave->wifiChannel) + " → " + String(wifiChannel));
                slave->wifiChannel = wifiChannel;
            }
        }
        
        // Guardar datos para callbacks (antes de liberar mutex)
        String deviceNameForCallback = slave->deviceName;
        
        xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex ANTES de callbacks y operaciones externas
        
        // 🚨 NOVO: Se slave estava offline e voltou online, enviar comandos pendentes
        if (wasOffline) {
            Serial.println("\n🔄 ========================================");
            Serial.println("🔄 SLAVE VOLTOU ONLINE - ENVIANDO COMANDOS PENDENTES");
            Serial.println("🔄 ========================================");
            sendPendingCommandsToSlave(senderMac);
            
            // ✅ CORREÇÃO: Solo llamar callback cuando el slave realmente cambia de offline a online
            if (slaveOnlineCallback) {
                slaveOnlineCallback(senderMac, deviceNameForCallback);
            }
        }
        // ✅ NO llamar callback si el slave ya estaba online (evitar spam)
        
        if (isNewSlave) {
            Serial.println("📊 Slave configurado na lista confiável");
        } else {
            Serial.println("📊 Informações atualizadas na lista confiável");
        }
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
        slave->updateLastSeen();
        slave->messagesReceived++;
        slave->status = SlaveStatus::ONLINE; // ✅ CRÍTICO: Marcar como online cuando recibe mensaje
        
        // ⭐ POTENCIA MÁXIMA: Atualizar estado do relé remoto
        if (relayNumber >= 0 && relayNumber < 8) {
            // ✅ DEBUG: Log antes de atualizar
            bool oldState = slave->relayStates[relayNumber].state;
            Serial.printf("🔄 [SLAVE] Atualizando relé %d do slave %s: %s → %s\n",
                         relayNumber,
                         ESPNowController::macToString(senderMac).c_str(),
                         oldState ? "ON" : "OFF",
                         state ? "ON" : "OFF");
            
            slave->relayStates[relayNumber].state = state;
            slave->relayStates[relayNumber].hasTimer = hasTimer;
            slave->relayStates[relayNumber].remainingTime = remainingTime;
            slave->relayStates[relayNumber].lastUpdate = millis();
            if (!name.isEmpty()) {
                slave->relayStates[relayNumber].name = name;  // ✅ Armazenar nome do relé
            }
            
            // ✅ DEBUG: Verificar se foi atualizado corretamente
            if (slave->relayStates[relayNumber].state != state) {
                Serial.printf("❌ [SLAVE] ERRO: Estado não foi atualizado! Esperado: %s, Atual: %s\n",
                             state ? "ON" : "OFF",
                             slave->relayStates[relayNumber].state ? "ON" : "OFF");
            } else {
                Serial.printf("✅ [SLAVE] Estado confirmado: relé %d = %s\n",
                             relayNumber,
                             slave->relayStates[relayNumber].state ? "ON" : "OFF");
            }
            
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
        bool wasOfflineBefore = !slave->isOnline(); // ✅ Verificar si estaba offline antes
        
        xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex ANTES de callbacks
        
        // ✅ CORREÇÃO CRÍTICA: Solo llamar slaveOnlineCallback si:
        // 1. El slave estaba offline y ahora está online (cambio de estado)
        // 2. NO estamos procesando un ALL_RELAYS_STATUS (para evitar spam)
        if (slaveOnlineCallback && wasOfflineBefore && !processingStatusResponse) {
            slaveOnlineCallback(senderMac, deviceName);
        }
    } else {
        xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex se não encontrou
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
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de modificar
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em checkSlaveStatus()");
        return;
    }
    
    unsigned long currentTime = millis();
    const unsigned long offlineTimeout = 30000;  // 30 segundos
    
    for (auto& slave : trustedSlaves) {
        if (slave.isOfflineTimeout(offlineTimeout)) {
            if (slave.status != SlaveStatus::OFFLINE) {
                slave.status = SlaveStatus::OFFLINE;
                Serial.println("🔴 Slave offline: " + ESPNowController::macToString(slave.macAddress) + 
                              " (" + slave.deviceName + ")");
                
                // Chamar callback se definido
                if (slaveOfflineCallback) {
                    slaveOfflineCallback(slave.macAddress, slave.deviceName);
                }
            }
        }
    }
    
    xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex
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
    // ✅ PROTEÇÃO MULTI-CORE: Lock mutex antes de modificar
    if (xSemaphoreTake(trustedSlavesMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        Serial.println("⚠️ Timeout ao obter mutex em cleanupOfflineSlaves()");
        return;
    }
    
    Serial.println("🧹 Limpando Slaves offline...");
    
    int removedCount = 0;
    for (auto it = trustedSlaves.begin(); it != trustedSlaves.end();) {
        if (it->isOfflineTimeout(120000)) {  // 2 minutos offline
            Serial.println("🗑️ Removendo Slave offline: " + ESPNowController::macToString(it->macAddress) + 
                          " (" + it->deviceName + ")");
            it = trustedSlaves.erase(it);
            removedCount++;
        } else {
            ++it;
        }
    }
    
    Serial.println("✅ " + String(removedCount) + " Slaves offline removidos");
    
    xSemaphoreGive(trustedSlavesMutex);  // ✅ Liberar mutex
}

void MasterSlaveManager::rediscoverSlaves() {
    if (!espNowController) {
        Serial.println("❌ ESPNowController não inicializado");
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
    
    // ✅ PATRÓN MASTER-TASK: Enviar múltiplos broadcasts espaciados para garantir descoberta
    Serial.println("📢 Enviando broadcasts de descoberta (padrão MASTER-TASK)...");
    for (int i = 0; i < 3; i++) {
        bool success = espNowController->sendDiscoveryBroadcast();
        if (success) {
            Serial.printf("   ✅ Broadcast %d/3 enviado\n", i + 1);
        } else {
            Serial.printf("   ❌ Falha ao enviar broadcast %d/3\n", i + 1);
        }
        
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
    
    Serial.println("⏳ Aguardando respostas dos Slaves...");
}

// ===== 🔄 FASE 1: IMPLEMENTAÇÃO DO SISTEMA DE RETRY =====

uint32_t MasterSlaveManager::generateCommandId() {
    return ++commandIdCounter;
}

void MasterSlaveManager::addToRetryQueue(const uint8_t* targetMac, int relayNumber, const String& action, int duration, uint32_t commandId, int supabaseCommandId) {
    PendingRelayCommand cmd;
    memcpy(cmd.targetMac, targetMac, 6);
    cmd.relayNumber = relayNumber;
    cmd.action = action;
    cmd.duration = duration;
    cmd.timestamp = millis();
    cmd.nextRetry = millis() + RETRY_INTERVAL;  // Primeiro retry em 2s
    cmd.retryCount = 0;
    cmd.commandId = commandId;
    cmd.waitingForAck = false;
    cmd.supabaseCommandId = supabaseCommandId;  // ✅ Guardar ID de Supabase
    
    pendingRelayCommands.push_back(cmd);
    
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
    if (pendingRelayCommands.empty()) return;
    
    unsigned long now = millis();
    
    for (auto it = pendingRelayCommands.begin(); it != pendingRelayCommands.end();) {
        // Verificar se é hora de tentar novamente
        if (now >= it->nextRetry && it->retryCount < MAX_RELAY_RETRIES) {
            Serial.println("\n🔄 ========================================");
            Serial.println("🔄 REINTENTANDO COMANDO");
            Serial.println("🔄 ========================================");
            Serial.println("🆔 Command ID: " + String(it->commandId));
            Serial.println("📡 Destino: " + ESPNowController::macToString(it->targetMac));
            Serial.println("🔌 Relé: " + String(it->relayNumber));
            Serial.println("⚡ Ação: " + it->action);
            Serial.println("🔢 Tentativa: " + String(it->retryCount + 1) + "/" + String(MAX_RELAY_RETRIES));
            
            // Tentar reenviar o comando
            bool success = espNowController->sendRelayCommand(it->targetMac, it->relayNumber, it->action, it->duration);
            
            if (success) {
                Serial.println("✅ Reintento exitoso!");
                Serial.println("⏳ Aguardando confirmação do Slave...");
                Serial.println("========================================\n");
                
                // Marcar como aguardando ACK
                it->waitingForAck = true;
                it->retryCount++;
                it->nextRetry = now + (RETRY_INTERVAL * it->retryCount);  // Backoff exponencial
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
        else if (now - it->timestamp > 30000) {  // 30 secs timeout
            Serial.println("\n⏰ ========================================");
            Serial.println("⏰ TIMEOUT DE COMANDO (1 minuto)");
            Serial.println("⏰ ========================================");
            Serial.println("🆔 Command ID: " + String(it->commandId));
            Serial.println("📡 Destino: " + ESPNowController::macToString(it->targetMac));
            Serial.println("🔌 Relé: " + String(it->relayNumber));
            Serial.println("⚡ Ação: " + it->action);
            Serial.println("⏱️ Tempo em fila: " + String((now - it->timestamp) / 1000) + "s");
            Serial.println("⚠️ Comando descartado por timeout (slave offline > 1 minuto)");
            
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
        else if (it->waitingForAck && (now - it->timestamp > 30000)) {  // 30 segundos timeout para ACK
            Serial.println("\n⏰ ========================================");
            Serial.println("⏰ TIMEOUT DE ACK");
            Serial.println("⏰ ========================================");
            Serial.println("🆔 Command ID: " + String(it->commandId));
            Serial.println("📡 Destino: " + ESPNowController::macToString(it->targetMac));
            Serial.println("⚠️ Comando descartado por timeout de ACK");
            
            // ✅ Marcar como failed en Supabase si viene de Supabase
            if (it->supabaseCommandId > 0 && supabaseCommandCallback) {
                String errorMsg = "Timeout esperando ACK del slave";
                supabaseCommandCallback(it->supabaseCommandId, false, errorMsg);
                Serial.println("📤 Comando marcado como FAILED en Supabase (ID: " + String(it->supabaseCommandId) + ")");
            }
            
            Serial.println("========================================\n");
            
            it = pendingRelayCommands.erase(it);
        }
        else {
            ++it;
        }
    }
}

void MasterSlaveManager::removeFromRetryQueue(uint32_t commandId, bool currentState) {
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
            
            // ✅ NOVO: Marcar como completed en Supabase com currentState
            if (it->supabaseCommandId > 0 && supabaseCommandCallback) {
                // ✅ Callback precisa ser atualizado para receber currentState
                // Por enquanto, passar currentState via string no errorMessage (temporário)
                String stateStr = currentState ? "true" : "false";
                supabaseCommandCallback(it->supabaseCommandId, true, stateStr);
                Serial.println("📤 Comando marcado como COMPLETED en Supabase (ID: " + String(it->supabaseCommandId) + ", state: " + stateStr + ")");
            }
            
            Serial.println("========================================\n");
            
            pendingRelayCommands.erase(it);
            break;
        }
    }
}

// 🚨 NOVO: Enviar todos os comandos pendentes para um slave quando ele volta online
void MasterSlaveManager::sendPendingCommandsToSlave(const uint8_t* macAddress) {
    if (!initialized || !espNowController) return;
    
    // Contar comandos pendentes para este slave
    int pendingCount = 0;
    for (const auto& cmd : pendingRelayCommands) {
        if (memcmp(cmd.targetMac, macAddress, 6) == 0) {
            pendingCount++;
        }
    }
    
    if (pendingCount == 0) {
        Serial.println("✅ Nenhum comando pendente para este slave");
        Serial.println("========================================\n");
        return;
    }
    
    Serial.println("📋 Comandos pendentes encontrados: " + String(pendingCount));
    Serial.println("📡 Destino: " + ESPNowController::macToString(macAddress));
    Serial.println("🚀 Enviando comandos em lote...");
    Serial.println("========================================\n");
    
    // Enviar todos os comandos pendentes para este slave
    int sentCount = 0;
    int failedCount = 0;
    
    for (auto it = pendingRelayCommands.begin(); it != pendingRelayCommands.end();) {
        if (memcmp(it->targetMac, macAddress, 6) == 0) {
            Serial.println("📤 Enviando comando pendente:");
            Serial.println("   🆔 Command ID: " + String(it->commandId));
            Serial.println("   🔌 Relé: " + String(it->relayNumber));
            Serial.println("   ⚡ Ação: " + it->action);
            if (it->duration > 0) {
                Serial.println("   ⏱️ Duração: " + String(it->duration) + "s");
            }
            
            // Resetar contador de retry para dar nova chance
            it->retryCount = 0;
            it->nextRetry = millis() + 100; // Enviar imediatamente (100ms)
            it->waitingForAck = false;
            
            // Tentar enviar
            bool success = espNowController->sendRelayCommand(it->targetMac, it->relayNumber, it->action, it->duration);
            
            if (success) {
                sentCount++;
                Serial.println("   ✅ Comando enviado com sucesso!");
                it->waitingForAck = true;
                it->timestamp = millis();
                ++it;
            } else {
                failedCount++;
                Serial.println("   ❌ Falha ao enviar comando - permanecerá na fila");
                ++it;
            }
            
            delay(50); // Pequeno delay entre comandos para não sobrecarregar
        } else {
            ++it;
        }
    }
    
    Serial.println("\n📊 Resumo do envio em lote:");
    Serial.println("   ✅ Enviados: " + String(sentCount));
    Serial.println("   ❌ Falhas: " + String(failedCount));
    Serial.println("   📋 Total pendente: " + String(pendingCount));
    Serial.println("========================================\n");
}

// ===== 🔄 FASE 2: PROCESSAMENTO DE ACKs DE RELAY =====

void MasterSlaveManager::processRelayCommandAck(const RelayCommandAck& ack, const uint8_t* senderMac) {
    Serial.println("\n🎊 ========================================");
    Serial.println("🎊 ACK DE COMANDO RECEBIDO!");
    Serial.println("🎊 ========================================");
    Serial.println("📥 De: " + ESPNowController::macToString(senderMac));
    Serial.println("🆔 Command ID: " + String(ack.commandId));
    Serial.println("🔌 Relé: " + String(ack.relayNumber));
    Serial.println("✅ Success: " + String(ack.success ? "Sim" : "Não"));
    Serial.println("💡 Estado atual: " + String(ack.currentState ? "ON" : "OFF"));
    Serial.println("⏰ Timestamp: " + String(ack.timestamp));
    
    // Atualizar estatísticas do Slave
    TrustedSlave* slave = getTrustedSlave(senderMac);
    if (slave) {
        slave->updateLastSeen();
        slave->messagesReceived++;
        
        // ⭐ POTENCIA MÁXIMA: Atualizar estado do relé remoto desde ACK
        if (ack.success && ack.relayNumber < 8) {
            bool newState = (ack.currentState == 1);
            slave->relayStates[ack.relayNumber].state = newState;
            slave->relayStates[ack.relayNumber].lastUpdate = millis();
            Serial.println("✅ Estado do relé " + String(ack.relayNumber) + " atualizado desde ACK: " + (newState ? "ON" : "OFF"));
            
            // ✅ CRÍTICO: Atualizar Supabase (fonte única de verdade)
            if (supabaseRelayStateCallback) {
                String masterDeviceId = getDeviceID();
                String slaveMac = ESPNowController::macToString(senderMac);
                String slaveDeviceId = "ESP32_SLAVE_" + slaveMac;
                slaveDeviceId.replace(":", "_");
                supabaseRelayStateCallback(masterDeviceId, slaveMac, slaveDeviceId, ack.relayNumber, newState, false, 0);
            }
        }
        
        if (ack.success) {
            Serial.println("📊 Slave executou comando com sucesso!");
        } else {
            Serial.println("⚠️ Slave reportou falha na execução");
            slave->errors++;
        }
    }
    
    // 🎯 CRITICAL: Remover comando da fila de retry
    if (ack.success) {
        Serial.println("\n🔄 Removendo comando da fila de retry...");
        // ✅ NOVO: Passar currentState do ACK
        bool currentState = (ack.currentState == 1);
        removeFromRetryQueue(ack.commandId, currentState);
        Serial.println("✅ Comando removido - retry cancelado!");
    } else {
        Serial.println("\n⚠️ Comando falhou no Slave");
        Serial.println("🔄 Sistema de retry continuará tentando...");
    }
    
    // Chamar callback se definido
    if (relayAckCallback) {
        relayAckCallback(senderMac, ack.commandId, ack.success, ack.relayNumber, ack.currentState);
    }
    
    Serial.println("========================================\n");
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

