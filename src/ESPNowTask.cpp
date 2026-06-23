#include "ESPNowTask.h"
#include <ArduinoJson.h>
#include <WiFi.h>

// ===== INSTÂNCIA ESTÁTICA =====
ESPNowTask* ESPNowTask::instance = nullptr;

ESPNowTask::ESPNowTask() 
    : taskHandle(nullptr), messageQueue(nullptr), mutex(nullptr),
      initialized(false), messageCallback(nullptr), discoveryCallback(nullptr), statusCallback(nullptr),
      lastHeartbeat(0), lastCleanup(0), lastPingCycle(0), currentSlaveIndex(0) {
    
    // MAC de broadcast
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(broadcastMac, broadcast, 6);
    
    instance = this;
}

ESPNowTask::~ESPNowTask() {
    end();
}

bool ESPNowTask::begin() {
    Serial.println("\n🚀 === INICIANDO ESP-NOW TASK DEDICADA ===");
    
    // ===== PASSO 1: VERIFICAR SE WiFi ESTÁ CONECTADO =====
    // MOTIVO: ESP-NOW e WiFi usam o mesmo hardware de rádio
    //         Precisam estar no MESMO CANAL para funcionar juntos
    Serial.println("\n⏳ Aguardando conexão WiFi antes de inicializar ESP-NOW...");
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        Serial.print(".");
        delay(500);
        attempts++;
    }
    Serial.println();
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ WiFi não conectado após 15 segundos");
        Serial.println("💡 ESP-NOW precisa do WiFi conectado para obter o canal correto");
        return false;
    }
    
    // ===== PASSO 2: OBTER CANAL DO WiFi =====
    // MOTIVO: ESP-NOW DEVE usar o mesmo canal que o WiFi
    //         Se usar canal diferente, vai desconectar o WiFi
    uint8_t wifiChannel = WiFi.channel();
    Serial.println("✅ WiFi conectado!");
    Serial.println("   SSID: " + WiFi.SSID());
    Serial.println("   Canal WiFi: " + String(wifiChannel));
    Serial.println("   RSSI: " + String(WiFi.RSSI()) + " dBm");
    Serial.println("   IP: " + WiFi.localIP().toString());
    
    // ===== PASSO 3: CRIAR MUTEX =====
    // MOTIVO: Proteger acesso a dados compartilhados entre tasks
    mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        Serial.println("❌ Erro ao criar mutex");
        return false;
    }
    
    // ===== PASSO 4: CRIAR QUEUE DE MENSAGENS =====
    // MOTIVO: Comunicação entre callback ESP-NOW e a task principal
    messageQueue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(TaskESPNowMessage));
    if (!messageQueue) {
        Serial.println("❌ Erro ao criar queue de mensagens");
        return false;
    }
    
    // ===== PASSO 5: INICIALIZAR ESP-NOW =====
    // MOTIVO: Agora sim, com WiFi conectado, podemos inicializar ESP-NOW
    if (!initializeESPNow()) {
        Serial.println("❌ Erro ao inicializar ESP-NOW");
        return false;
    }
    
    // Criar task dedicada
    BaseType_t result = xTaskCreatePinnedToCore(
        taskFunction,           // Função da task
        "ESPNowTask",          // Nome da task
        ESPNOW_TASK_STACK_SIZE, // Stack size
        this,                   // Parâmetro
        ESPNOW_TASK_PRIORITY,   // Prioridade
        &taskHandle,           // Handle da task
        ESPNOW_TASK_CORE       // Core dedicado
    );
    
    if (result != pdPASS) {
        Serial.println("❌ Erro ao criar task ESP-NOW");
        return false;
    }
    
    initialized = true;
    Serial.println("✅ ESP-NOW Task criada com sucesso!");
    Serial.println("   Core: " + String(ESPNOW_TASK_CORE));
    Serial.println("   Canal: " + String(ESPNOW_FIXED_CHANNEL));
    Serial.println("   MAC: " + getLocalMacString());
    Serial.println("==========================================\n");
    
    return true;
}

void ESPNowTask::end() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = nullptr;
    }
    
    if (messageQueue) {
        vQueueDelete(messageQueue);
        messageQueue = nullptr;
    }
    
    if (mutex) {
        vSemaphoreDelete(mutex);
        mutex = nullptr;
    }
    
    if (esp_now_deinit() != ESP_OK) {
        Serial.println("⚠️ Erro ao finalizar ESP-NOW");
    }
    
    initialized = false;
}

bool ESPNowTask::initializeESPNow() {
    // ===== PASSO 5.1: NÃO MUDAR O MODO WiFi =====
    // MOTIVO: WiFi já está conectado, mudar modo vai desconectar
    // ANTES: WiFi.mode(WIFI_STA); ← Isso desconectaria!
    // AGORA: Não mexemos no modo, WiFi já está configurado
    
    // ===== PASSO 5.2: USAR O CANAL QUE O WiFi JÁ ESTÁ USANDO =====
    // MOTIVO: ESP-NOW e WiFi precisam estar no MESMO canal
    // ANTES: esp_wifi_set_channel(ESPNOW_FIXED_CHANNEL, ...) ← Forçava canal fixo
    // AGORA: Usamos o canal que o WiFi já está usando
    uint8_t currentChannel = WiFi.channel();
    Serial.println("📶 ESP-NOW usando canal do WiFi: " + String(currentChannel));
    
    // Inicializar ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ Erro ao inicializar ESP-NOW");
        return false;
    }
    
    // Registrar callbacks
    registerCallbacks();
    
    // Obter MAC local
    esp_wifi_get_mac(WIFI_IF_STA, localMac);
    
    // ===== PASSO 5.3: REGISTRAR PEER BROADCAST NO CANAL DO WiFi =====
    // MOTIVO: O peer precisa estar no mesmo canal que estamos usando
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastMac, 6);
    peerInfo.channel = currentChannel;  // ← Usar canal do WiFi, não canal fixo!
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("❌ Erro ao registrar peer de broadcast");
        return false;
    }
    
    Serial.println("✅ ESP-NOW inicializado com sucesso!");
    Serial.println("   MAC Local: " + getLocalMacString());
    Serial.println("   Canal: " + String(currentChannel) + " (mesmo do WiFi)");
    
    return true;
}

void ESPNowTask::registerCallbacks() {
    esp_now_register_recv_cb(onDataReceived);
    esp_now_register_send_cb(onDataSent);
}

void ESPNowTask::taskFunction(void* parameter) {
    ESPNowTask* task = static_cast<ESPNowTask*>(parameter);
    
    Serial.println("🔄 ESP-NOW Task iniciada no Core " + String(xPortGetCoreID()));
    Serial.println("📡 ARQUITETURA HÍBRIDA ATIVADA:");
    Serial.println("   ├─ Heartbeat Broadcast: " + String(ESPNOW_HEARTBEAT_INTERVAL/1000) + "s");
    Serial.println("   ├─ Ping Rotacionado: " + String(ESPNOW_PING_CYCLE_INTERVAL/1000) + "s");
    Serial.println("   ├─ Cleanup: " + String(ESPNOW_CLEANUP_INTERVAL/1000) + "s");
    Serial.println("   └─ Offline Timeout: " + String(ESPNOW_OFFLINE_TIMEOUT/1000) + "s");
    
    while (true) {
        uint32_t now = millis();
        
        // ===== 1. PROCESSAR QUEUE DE MENSAGENS RECEBIDAS =====
        task->processMessageQueue();
        
        // ===== 2. HEARTBEAT BROADCAST (a cada 30s) =====
        // Mantém presença do Master para todos os Slaves
        // Usa broadcast para economizar banda
        if (now - task->lastHeartbeat > ESPNOW_HEARTBEAT_INTERVAL) {
            task->sendHeartbeat();
            task->lastHeartbeat = now;
        }
        
        // ===== 3. PING ROTACIONADO (a cada 6s) =====
        // Verifica individualmente cada Slave em um ciclo
        // Com 10 slaves: cada um recebe ping a cada 60s (10 × 6s)
        if (now - task->lastPingCycle > ESPNOW_PING_CYCLE_INTERVAL) {
            xSemaphoreTake(task->mutex, portMAX_DELAY);
            
            if (!task->slaves.empty()) {
                // Pegar o próximo slave no ciclo
                SlaveInfo& slave = task->slaves[task->currentSlaveIndex];
                
                // Enviar ping apenas se estiver online ou recentemente visto
                if (slave.online || (now - slave.lastSeen < ESPNOW_OFFLINE_TIMEOUT)) {
                    // Registrar timestamp do ping para calcular RTT
                    slave.pingTimestamp = now;
                    task->sendPing(slave.mac);
                    
                    Serial.println("🏓 Ping → " + String(slave.name) + " (" + 
                                   task->macToString(slave.mac) + ")");
                }
                
                // Avançar para o próximo slave (rotação circular)
                task->currentSlaveIndex = (task->currentSlaveIndex + 1) % task->slaves.size();
            }
            
            xSemaphoreGive(task->mutex);
            task->lastPingCycle = now;
        }
        
        // ===== 4. CLEANUP DE SLAVES OFFLINE (a cada 60s) =====
        // Verifica se algum slave ficou 2min sem comunicação
        if (now - task->lastCleanup > ESPNOW_CLEANUP_INTERVAL) {
            task->cleanupOfflineSlaves();
            task->lastCleanup = now;
        }
        
        // ===== 5. DELAY DA TASK (100ms) =====
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void ESPNowTask::processMessageQueue() {
    TaskESPNowMessage message;
    
    while (xQueueReceive(messageQueue, &message, 0) == pdTRUE) {
        // Processar mensagem recebida
        processReceivedMessage(message);
    }
}

void ESPNowTask::processHeartbeat() {
    // MÉTODO DEPRECATED - Heartbeat agora é gerenciado diretamente na taskFunction
    // Mantido para compatibilidade, mas não faz nada
}

void ESPNowTask::cleanupOfflineSlaves() {
    // NOTA: Este método é chamado pela taskFunction, não precisa verificar intervalo
    uint32_t now = millis();
    
    xSemaphoreTake(mutex, portMAX_DELAY);
    
    int offlineCount = 0;
    for (auto& slave : slaves) {
        // Verificar se passou do timeout (ESPNOW_OFFLINE_TIMEOUT = 120s)
        if (now - slave.lastSeen > ESPNOW_OFFLINE_TIMEOUT) {
            if (slave.online) {
                slave.online = false;
                offlineCount++;
                
                Serial.println("⚠️ Slave OFFLINE: " + String(slave.name) + 
                               " (sem comunicação há " + String((now - slave.lastSeen)/1000) + "s)");
                
                // Notificar callback
                if (statusCallback) {
                    statusCallback(slave.mac, false);
                }
            }
        }
    }
    
    if (offlineCount > 0) {
        Serial.println("📊 Cleanup: " + String(offlineCount) + " slave(s) marcado(s) offline");
    }
    
    xSemaphoreGive(mutex);
}

// ===== MÉTODO DESABILITADO - SLAVES NÃO PRECISAM DE CREDENCIAIS WiFi =====
// Mantido comentado para referência futura, caso seja necessário
// Slaves conectam via ESP-NOW puro, sem WiFi
/*
bool ESPNowTask::sendWiFiCredentials(const uint8_t* targetMac, const char* ssid, const char* password, uint8_t channel) {
    TaskESPNowMessage message = {};
    message.type = TASK_MSG_WIFI_CREDENTIALS;
    memcpy(message.targetMac, targetMac, 6);
    memcpy(message.senderMac, localMac, 6);
    message.timestamp = millis();
    
    WiFiCredentials creds = {};
    strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
    strncpy(creds.password, password, sizeof(creds.password) - 1);
    creds.channel = channel;
    creds.checksum = calculateChecksum((uint8_t*)&creds, sizeof(creds) - 1);
    
    memcpy(message.data, &creds, sizeof(creds));
    message.dataSize = sizeof(creds);
    message.checksum = calculateChecksum((uint8_t*)&message, sizeof(message) - 1);
    
    esp_err_t result = esp_now_send(targetMac, (uint8_t*)&message, sizeof(message));
    
    if (result == ESP_OK) {
        Serial.println("✅ Credenciais WiFi enviadas");
        Serial.println("   SSID: " + String(ssid));
        Serial.println("   Canal: " + String(channel));
        Serial.println("   Destino: " + macToString(targetMac));
        return true;
    } else {
        Serial.println("❌ Erro ao enviar credenciais: " + String(result));
        return false;
    }
}
*///

bool ESPNowTask::sendRelayCommand(const uint8_t* targetMac, uint8_t relayNumber, const char* action, uint32_t duration) {
    TaskESPNowMessage message = {};
    message.type = TASK_MSG_RELAY_COMMAND;
    memcpy(message.targetMac, targetMac, 6);
    memcpy(message.senderMac, localMac, 6);
    message.timestamp = millis();
    
    ESPNowRelayCommand cmd = {};
    cmd.relayNumber = relayNumber;
    strncpy(cmd.action, action, sizeof(cmd.action) - 1);
    cmd.duration = duration;
    cmd.checksum = calculateChecksum((uint8_t*)&cmd, sizeof(cmd) - 1);
    
    memcpy(message.data, &cmd, sizeof(cmd));
    message.dataSize = sizeof(cmd);
    message.checksum = calculateChecksum((uint8_t*)&message, sizeof(message) - 1);
    
    esp_err_t result = esp_now_send(targetMac, (uint8_t*)&message, sizeof(message));
    
    if (result == ESP_OK) {
        Serial.println("✅ Comando enviado: Relé " + String(relayNumber) + " " + String(action));
        Serial.println("   Destino: " + macToString(targetMac));
        Serial.println("   Duração: " + String(duration) + "s");
        return true;
    } else {
        Serial.println("❌ Erro ao enviar comando: " + String(result));
        return false;
    }
}

bool ESPNowTask::sendPing(const uint8_t* targetMac) {
    TaskESPNowMessage message = {};
    message.type = TASK_MSG_PING;
    memcpy(message.targetMac, targetMac, 6);
    memcpy(message.senderMac, localMac, 6);
    message.timestamp = millis();
    message.dataSize = 0;
    message.checksum = calculateChecksum((uint8_t*)&message, sizeof(message) - 1);
    
    esp_err_t result = esp_now_send(targetMac, (uint8_t*)&message, sizeof(message));
    return (result == ESP_OK);
}

bool ESPNowTask::sendDiscovery() {
    TaskESPNowMessage message = {};
    message.type = TASK_MSG_DISCOVERY;
    memcpy(message.targetMac, broadcastMac, 6);
    memcpy(message.senderMac, localMac, 6);
    message.timestamp = millis();
    message.dataSize = 0;
    message.checksum = calculateChecksum((uint8_t*)&message, sizeof(message) - 1);
    
    esp_err_t result = esp_now_send(broadcastMac, (uint8_t*)&message, sizeof(message));
    
    if (result == ESP_OK) {
        Serial.println("✅ Discovery broadcast enviado");
        return true;
    } else {
        Serial.println("❌ Erro ao enviar discovery: " + String(result));
        return false;
    }
}

bool ESPNowTask::sendHeartbeat() {
    TaskESPNowMessage message = {};
    message.type = TASK_MSG_HEARTBEAT;
    memcpy(message.targetMac, broadcastMac, 6);
    memcpy(message.senderMac, localMac, 6);
    message.timestamp = millis();
    message.dataSize = 0;
    message.checksum = calculateChecksum((uint8_t*)&message, sizeof(message) - 1);
    
    esp_err_t result = esp_now_send(broadcastMac, (uint8_t*)&message, sizeof(message));
    return (result == ESP_OK);
}

bool ESPNowTask::sendChannelChangeNotification(uint8_t oldChannel, uint8_t newChannel, uint8_t reason) {
    Serial.println("\n📢 === NOTIFICANDO MUDANÇA DE CANAL ===");
    Serial.printf("   Canal Anterior: %d\n", oldChannel);
    Serial.printf("   Novo Canal: %d\n", newChannel);
    Serial.printf("   Motivo: %s\n", reason == 1 ? "WiFi mudou" : reason == 2 ? "Manual" : "Interferência");
    
    TaskESPNowMessage message = {};
    message.type = TASK_MSG_CHANNEL_CHANGE;
    memcpy(message.targetMac, broadcastMac, 6);  // Broadcast para todos os slaves
    memcpy(message.senderMac, localMac, 6);
    message.timestamp = millis();
    
    // Preparar estrutura de notificação
    ChannelChangeNotification notification = {};
    notification.oldChannel = oldChannel;
    notification.newChannel = newChannel;
    notification.reason = reason;
    notification.changeTime = millis();
    notification.checksum = calculateChecksum((uint8_t*)&notification, sizeof(notification) - 1);
    
    // Copiar para mensagem
    memcpy(message.data, &notification, sizeof(notification));
    message.dataSize = sizeof(notification);
    message.checksum = calculateChecksum((uint8_t*)&message, sizeof(message) - 1);
    
    // IMPORTANTE: Enviar no CANAL ANTIGO primeiro (slaves ainda estão lá)
    uint8_t currentEspNowChannel = WiFi.channel();
    if (currentEspNowChannel != oldChannel) {
        Serial.printf("⚠️ Mudando temporariamente para canal %d para notificar slaves\n", oldChannel);
        esp_wifi_set_channel(oldChannel, WIFI_SECOND_CHAN_NONE);
        delay(50); // Aguardar estabilização
    }
    
    // Enviar notificação 3 vezes para garantir entrega
    int successCount = 0;
    for (int i = 0; i < 3; i++) {
        esp_err_t result = esp_now_send(broadcastMac, (uint8_t*)&message, sizeof(message));
        if (result == ESP_OK) {
            successCount++;
        }
        delay(100); // Delay entre tentativas
    }
    
    // Voltar para o novo canal
    if (currentEspNowChannel != newChannel) {
        Serial.printf("📶 Retornando para canal %d\n", newChannel);
        esp_wifi_set_channel(newChannel, WIFI_SECOND_CHAN_NONE);
    }
    
    if (successCount > 0) {
        Serial.printf("✅ Notificação enviada com sucesso (%d/3 tentativas)\n", successCount);
        Serial.println("=====================================\n");
        return true;
    } else {
        Serial.println("❌ Falha ao enviar notificação");
        Serial.println("=====================================\n");
        return false;
    }
}

// ===== MÉTODO DESABILITADO - SLAVES NÃO PRECISAM DE WiFi =====
/*
bool ESPNowTask::broadcastWiFiCredentials(const char* ssid, const char* password, uint8_t channel) {
    return sendWiFiCredentials(broadcastMac, ssid, password, channel);
}
*///

bool ESPNowTask::broadcastRelayCommand(uint8_t relayNumber, const char* action, uint32_t duration) {
    TaskESPNowMessage message = {};
    message.type = TASK_MSG_RELAY_COMMAND;
    memcpy(message.targetMac, broadcastMac, 6);
    memcpy(message.senderMac, localMac, 6);
    message.timestamp = millis();
    
    ESPNowRelayCommand cmd = {};
    cmd.relayNumber = relayNumber;
    strncpy(cmd.action, action, sizeof(cmd.action) - 1);
    cmd.duration = duration;
    cmd.checksum = calculateChecksum((uint8_t*)&cmd, sizeof(cmd) - 1);
    
    memcpy(message.data, &cmd, sizeof(cmd));
    message.dataSize = sizeof(cmd);
    message.checksum = calculateChecksum((uint8_t*)&message, sizeof(message) - 1);
    
    esp_err_t result = esp_now_send(broadcastMac, (uint8_t*)&message, sizeof(message));
    
    if (result == ESP_OK) {
        Serial.println("✅ Comando broadcast enviado");
        Serial.println("   Relé: " + String(relayNumber));
        Serial.println("   Ação: " + String(action));
        return true;
    } else {
        Serial.println("❌ Erro ao enviar broadcast: " + String(result));
        return false;
    }
}

void ESPNowTask::addSlave(const uint8_t* mac, const char* name, uint8_t relayCount) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    
    // Verificar se já existe
    for (auto& slave : slaves) {
        if (memcmp(slave.mac, mac, 6) == 0) {
            slave.online = true;
            slave.lastSeen = millis();
            strncpy(slave.name, name, sizeof(slave.name) - 1);
            slave.relayCount = relayCount;
            xSemaphoreGive(mutex);
            return;
        }
    }
    
    // Adicionar novo slave
    SlaveInfo newSlave = {};
    memcpy(newSlave.mac, mac, 6);
    strncpy(newSlave.name, name, sizeof(newSlave.name) - 1);
    newSlave.online = true;
    newSlave.lastSeen = millis();
    newSlave.relayCount = relayCount;
    newSlave.rssi = -50;
    newSlave.pingTimestamp = 0;  // Inicializar timestamp do ping
    newSlave.latency = 0;        // Inicializar latência
    
    slaves.push_back(newSlave);
    
    Serial.println("✅ Novo slave adicionado: " + String(name));
    Serial.println("   MAC: " + macToString(mac));
    Serial.println("   Relés: " + String(relayCount));
    
    if (discoveryCallback) {
        discoveryCallback(newSlave);
    }
    
    xSemaphoreGive(mutex);
}

void ESPNowTask::removeSlave(const uint8_t* mac) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    
    for (auto it = slaves.begin(); it != slaves.end(); ++it) {
        if (memcmp(it->mac, mac, 6) == 0) {
            Serial.println("🗑️ Slave removido: " + String(it->name));
            slaves.erase(it);
            break;
        }
    }
    
    xSemaphoreGive(mutex);
}

SlaveInfo* ESPNowTask::findSlave(const uint8_t* mac) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    
    for (auto& slave : slaves) {
        if (memcmp(slave.mac, mac, 6) == 0) {
            xSemaphoreGive(mutex);
            return &slave;
        }
    }
    
    xSemaphoreGive(mutex);
    return nullptr;
}

std::vector<SlaveInfo> ESPNowTask::getSlaves() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    std::vector<SlaveInfo> result = slaves;
    xSemaphoreGive(mutex);
    return result;
}

int ESPNowTask::getOnlineSlaveCount() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    
    int count = 0;
    for (const auto& slave : slaves) {
        if (slave.online) count++;
    }
    
    xSemaphoreGive(mutex);
    return count;
}

void ESPNowTask::setMessageCallback(ESPNowCallback callback) {
    messageCallback = callback;
}

void ESPNowTask::setSlaveDiscoveryCallback(SlaveDiscoveryCallback callback) {
    discoveryCallback = callback;
}

void ESPNowTask::setSlaveStatusCallback(SlaveStatusCallback callback) {
    statusCallback = callback;
}

bool ESPNowTask::isInitialized() {
    return initialized;
}

String ESPNowTask::getStatusJSON() {
    DynamicJsonDocument doc(1024);
    
    doc["initialized"] = initialized;
    doc["channel"] = ESPNOW_FIXED_CHANNEL;
    doc["mac"] = getLocalMacString();
    doc["slaves_total"] = slaves.size();
    doc["slaves_online"] = getOnlineSlaveCount();
    doc["uptime"] = millis() / 1000;
    
    return doc.as<String>();
}

void ESPNowTask::printStatus() {
    Serial.println("\n📊 === STATUS ESP-NOW TASK ===");
    Serial.println("   Inicializado: " + String(initialized ? "✅ Sim" : "❌ Não"));
    Serial.println("   Canal: " + String(ESPNOW_FIXED_CHANNEL));
    Serial.println("   MAC: " + getLocalMacString());
    Serial.println("   Slaves: " + String(slaves.size()) + " total, " + String(getOnlineSlaveCount()) + " online");
    Serial.println("   Uptime: " + String(millis() / 1000) + "s");
    Serial.println("===============================");
}

String ESPNowTask::macToString(const uint8_t* mac) {
    char macStr[18];
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", 
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}
bool ESPNowTask::stringToMac(const String& macStr, uint8_t* mac) {
    if (macStr.length() != 17) return false;
    
    int values[6];
    if (sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", 
               &values[0], &values[1], &values[2], 
               &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    
    for (int i = 0; i < 6; i++) {
        mac[i] = values[i];
    }
    
    return true;
}

String ESPNowTask::getLocalMacString() {
    return macToString(localMac);
}

uint8_t ESPNowTask::calculateChecksum(const uint8_t* data, uint8_t length) {
    uint8_t checksum = 0;
    for (uint8_t i = 0; i < length; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

bool ESPNowTask::validateMessage(const TaskESPNowMessage& message) {
    uint8_t calculatedChecksum = calculateChecksum((uint8_t*)&message, sizeof(message) - 1);
    return calculatedChecksum == message.checksum;
}

void ESPNowTask::processReceivedMessage(const TaskESPNowMessage& message) {
    if (!validateMessage(message)) {
        Serial.println("❌ Mensagem inválida (checksum incorreto)");
        return;
    }
    
    // Atualizar status do slave
    updateSlaveStatus(message.senderMac, true);
    
    // Processar por tipo
    switch (message.type) {
        case TASK_MSG_WIFI_CREDENTIALS:
            Serial.println("📶 Credenciais WiFi recebidas de: " + macToString(message.senderMac));
            break;
            
        case TASK_MSG_RELAY_COMMAND:
            Serial.println("🔌 Comando de relé recebido de: " + macToString(message.senderMac));
            break;
            
        case TASK_MSG_PING: {
            Serial.println("🏓 Ping recebido de: " + macToString(message.senderMac));
            // Enviar PONG
            TaskESPNowMessage pong = message;
            pong.type = TASK_MSG_PONG;
            memcpy(pong.targetMac, message.senderMac, 6);
            memcpy(pong.senderMac, localMac, 6);
            pong.timestamp = millis();
            pong.checksum = calculateChecksum((uint8_t*)&pong, sizeof(pong) - 1);
            esp_now_send(message.senderMac, (uint8_t*)&pong, sizeof(pong));
            break;
        }
            
        case TASK_MSG_PONG: {
            // Calcular latência (RTT - Round Trip Time)
            uint32_t now = millis();
            SlaveInfo* slave = findSlave(message.senderMac);
            
            if (slave && slave->pingTimestamp > 0) {
                // Calcular RTT
                slave->latency = now - slave->pingTimestamp;
                slave->pingTimestamp = 0; // Reset
                
                // Log com medição de latência
                Serial.println("🏓 Pong ← " + String(slave->name) + 
                               " | RTT: " + String(slave->latency) + "ms" +
                               " | RSSI: " + String(slave->rssi) + "dBm");
            } else {
                Serial.println("🏓 Pong recebido de: " + macToString(message.senderMac));
            }
            break;
        }
            
        case TASK_MSG_DISCOVERY:
            Serial.println("🔍 Discovery recebido de: " + macToString(message.senderMac));
            break;
            
        case TASK_MSG_STATUS_REQUEST:
            // MessageType::BROADCAST (0x06) quando recebido como TaskESPNowMessage
            Serial.println("📢 Broadcast/discovery recebido de: " + macToString(message.senderMac));
            break;
            
        case TASK_MSG_HEARTBEAT:
            // Heartbeat silencioso
            break;
            
        default:
            Serial.println("❓ Tipo de mensagem desconhecido: " + String(message.type));
            break;
    }
    
    // Chamar callback se definido
    if (messageCallback) {
        messageCallback(message);
    }
}

void ESPNowTask::updateSlaveStatus(const uint8_t* mac, bool online, int rssi) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    
    for (auto& slave : slaves) {
        if (memcmp(slave.mac, mac, 6) == 0) {
            bool wasOnline = slave.online;
            slave.online = online;
            slave.lastSeen = millis();
            if (rssi != -50) slave.rssi = rssi;
            
            if (!wasOnline && online) {
                Serial.println("✅ Slave online: " + String(slave.name));
                if (statusCallback) {
                    statusCallback(slave.mac, true);
                }
            }
            break;
        }
    }
    
    xSemaphoreGive(mutex);
}

void ESPNowTask::onDataReceived(const uint8_t* mac, const uint8_t* data, int len) {
    if (!instance || len != sizeof(TaskESPNowMessage)) return;
    
    TaskESPNowMessage message;
    memcpy(&message, data, sizeof(message));
    
    // Adicionar à queue para processamento na task
    xQueueSend(instance->messageQueue, &message, 0);
}

void ESPNowTask::onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
    if (!instance) return;
    
    if (status == ESP_NOW_SEND_SUCCESS) {
        // Sucesso silencioso
    } else {
        Serial.println("❌ Falha ao enviar para: " + instance->macToString(mac));
    }
}

// ===== MÉTODOS PARA CONEXÃO AUTOMÁTICA =====

bool ESPNowTask::autoConnectToSlaves() {
    Serial.println("\n🔍 === INICIANDO CONEXÃO AUTOMÁTICA COM SLAVES ===");
    
    if (!initialized) {
        Serial.println("❌ ESP-NOW não inicializado");
        return false;
    }
    
    // ===== MUDANÇA: SLAVES NÃO PRECISAM DE CREDENCIAIS WiFi =====
    // Slaves conectam via ESP-NOW puro, sem WiFi
    // Removido: envio de credenciais WiFi
    // Removido: espera de 20 segundos para slaves conectarem ao WiFi
    
    // 1. Discovery automático (slaves já estão escutando via ESP-NOW)
    Serial.println("📡 Enviando discovery broadcast via ESP-NOW...");
    sendDiscovery();
    
    // 2. Aguardar respostas (5 segundos)
    Serial.println("⏳ Aguardando respostas de slaves...");
    delay(5000);
    
    // 3. Listar slaves encontrados
    printSlavesList();
    
    Serial.println("✅ Conexão automática concluída!");
    return true;
}

// ===== MÉTODO DESABILITADO - SLAVES NÃO PRECISAM DE WiFi =====
/*
bool ESPNowTask::sendWiFiCredentialsBroadcast(const String& ssid, const String& password, uint8_t channel) {
    TaskESPNowMessage message = {};
    message.type = TASK_MSG_WIFI_CREDENTIALS;
    memcpy(message.targetMac, broadcastMac, 6);
    memcpy(message.senderMac, localMac, 6);
    message.timestamp = millis();
    
    WiFiCredentials creds = {};
    strncpy(creds.ssid, ssid.c_str(), sizeof(creds.ssid) - 1);
    strncpy(creds.password, password.c_str(), sizeof(creds.password) - 1);
    creds.channel = channel;
    creds.checksum = calculateChecksum((uint8_t*)&creds, sizeof(creds) - 1);
    
    memcpy(message.data, &creds, sizeof(creds));
    message.dataSize = sizeof(creds);
    message.checksum = calculateChecksum((uint8_t*)&message, sizeof(message) - 1);
    
    esp_err_t result = esp_now_send(broadcastMac, (uint8_t*)&message, sizeof(message));
    
    if (result == ESP_OK) {
        Serial.println("✅ Credenciais WiFi enviadas via broadcast");
        Serial.println("   SSID: " + ssid);
        Serial.println("   Canal: " + String(channel));
        return true;
    } else {
        Serial.println("❌ Erro ao enviar credenciais: " + String(result));
        return false;
    }
}
*///

void ESPNowTask::printSlavesList() {
    Serial.println("\n📋 === SLAVES CONHECIDOS ===");
    
    if (slaves.empty()) {
        Serial.println("   Nenhum slave encontrado");
        return;
    }
    
    Serial.println("   Total: " + String(slaves.size()) + " slave(s)");
    
    for (size_t i = 0; i < slaves.size(); i++) {
        const SlaveInfo& slave = slaves[i];
        String status = slave.online ? "🟢 Online" : "🔴 Offline";
        String lastSeen = slave.online ? "Agora" : String((millis() - slave.lastSeen) / 1000) + "s atrás";
        
        Serial.println("   " + String(i + 1) + ". " + slave.name);
        Serial.println("      MAC: " + macToString(slave.mac));
        Serial.println("      Status: " + status);
        Serial.println("      Última comunicação: " + lastSeen);
        Serial.println("      RSSI: " + String(slave.rssi) + " dBm");
        Serial.println("");
    }
}

uint8_t* ESPNowTask::findSlaveMac(const String& slaveName) {
    for (auto& slave : slaves) {
        if (String(slave.name) == slaveName) {
            return slave.mac;
        }
    }
    return nullptr;
}

