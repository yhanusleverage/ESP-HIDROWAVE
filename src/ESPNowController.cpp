#include "ESPNowController.h"

// Incluir configurações se disponível
#ifndef CONFIG_H
    #include "Config.h"
#endif
#include "MASTER_CONFIG.h"

// Incluir WiFiCredentialsManager para estrutura WiFiCredentials
#include "WiFiCredentialsManager.h"

// 🔄 FASE 2: Incluir tipos e manager para processar ACKs
#include "ESPNowTypes.h"

// Forward declaration para evitar dependência circular
class MasterSlaveManager;

// Definir macros de debug se não estiverem definidas
#ifndef DEBUG_PRINTLN
    #define DEBUG_PRINTLN(x) Serial.println(x)
#endif

#ifndef DEBUG_PRINTF
    #define DEBUG_PRINTF(x, ...) Serial.printf(x, __VA_ARGS__)
#endif

// 🔄 FASE 2: Include do manager APÓS definições para evitar circular dependency
#include "MasterSlaveManager.h"

// Instância estática para callbacks
static void formatAllRelaysMask(const AllRelaysStatus& status, char* buf) {
    for (uint8_t i = 0; i < 8; i++) {
        if (i < status.numRelays) {
            buf[i] = status.relays[i].state ? 'T' : 'F';
        } else {
            buf[i] = 'F';
        }
    }
    buf[8] = '\0';
}

ESPNowController* ESPNowController::instance = nullptr;

ESPNowController::ESPNowController(const String& deviceName, uint8_t channel) 
    : deviceName(deviceName), wifiChannel(channel), initialized(false), messageCounter(0),
      messagesSent(0), messagesReceived(0), messagesLost(0), lastMessageId(0),
      recentSenderCount(0), lastMasterContactMs(0), masterContactEstablished(false) {
    memset(recentSenders, 0, sizeof(recentSenders));
    instance = this;
}

bool ESPNowController::begin() {
    DEBUG_PRINTLN("📡 Inicializando ESP-NOW Controller: " + deviceName);
    
    // ✅ CORREÇÃO: Preservar modo WiFi existente (AP+STA) se já estiver configurado
    // Não desconectar Station se já estiver conectado
    wifi_mode_t currentMode = WiFi.getMode();
    bool stationWasConnected = (WiFi.status() == WL_CONNECTED);
    
    if (currentMode == WIFI_OFF) {
        // Se WiFi está desligado, configurar como Station
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
    } else if (currentMode == WIFI_AP) {
        // Se só tem AP, adicionar Station sem desconectar
        WiFi.mode(WIFI_AP_STA);
    } else if (currentMode == WIFI_STA) {
        // Se só tem Station, manter mas não desconectar se conectado
        if (!stationWasConnected) {
            WiFi.disconnect();
        }
    }
    // Se já está em WIFI_AP_STA, não fazer nada - preservar conexão Station
    
    // 🚨 CRÍTICO: Detectar canal WiFi REAL antes de configurar
    // Si WiFi está conectado, usar su canal (no el del constructor)
    uint8_t actualChannel = wifiChannel; // Valor por defecto del constructor
    
    if (stationWasConnected || WiFi.isConnected()) {
        // WiFi está conectado - detectar canal REAL
        wifi_second_chan_t secondChan;
        uint8_t detectedChannel;
        esp_wifi_get_channel(&detectedChannel, &secondChan);
        
        if (detectedChannel > 0 && detectedChannel <= 13) {
            uint8_t oldChannel = wifiChannel; // Guardar valor anterior
            actualChannel = detectedChannel;
            wifiChannel = actualChannel; // Actualizar variable interna
            Serial.println("🔍 Canal WiFi detectado: " + String(actualChannel) + " (WiFi conectado)");
            Serial.println("   Actualizando wifiChannel de " + String(oldChannel) + " a " + String(actualChannel));
        }
    } else {
        // Sem WiFi STA: marco cero CONFIG para provisioning com slave
        actualChannel = ESPNOW_CONFIG_CHANNEL;
        wifiChannel = actualChannel;
        esp_wifi_set_channel(actualChannel, WIFI_SECOND_CHAN_NONE);
        Serial.println("📶 Sem WiFi STA — canal ESP-NOW CONFIG: " + String(actualChannel));
    }
    
    // Configurar canal WiFi con el canal REAL detectado
    esp_wifi_set_channel(actualChannel, WIFI_SECOND_CHAN_NONE);
    
    Serial.println("📶 WiFi configurado - Canal: " + String(actualChannel));
    Serial.println("🆔 MAC Local: " + getLocalMacString());
    
    // Inicializar ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ Erro ao inicializar ESP-NOW");
        return false;
    }
    
    Serial.println("✅ ESP-NOW inicializado");
    
    // Registrar callbacks
    esp_now_register_recv_cb(onDataReceived);
    esp_now_register_send_cb(onDataSent);
    
    // Adicionar peer broadcast para descoberta
    esp_now_peer_info_t peerInfo = {};
    memset(peerInfo.peer_addr, 0xFF, 6); // Broadcast address
    peerInfo.channel = actualChannel;  // 🚨 CRÍTICO: Usar canal REAL detectado, no el del constructor
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;  // ⭐ CRÍTICO: Definir interface
    
    esp_err_t addResult = esp_now_add_peer(&peerInfo);
    
    // ✅ CORREÇÃO BUG #2: Aceitar se peer já existe (MultiChannelDiscovery pode ter criado)
    if (addResult == ESP_ERR_ESPNOW_EXIST) {
        Serial.println("ℹ️  Peer broadcast já existe (provavelmente do MultiChannelDiscovery)");
        Serial.println("✅ Continuando normalmente - OK");
    } else if (addResult != ESP_OK) {
        Serial.println("⚠️ Aviso: Não foi possível adicionar peer broadcast");
        Serial.println("   Código de erro: " + String(addResult) + " (0x" + String(addResult, HEX) + ")");
        Serial.println("   Canal tentado: " + String(wifiChannel));
        Serial.println("   💡 Tentando remover e readicionar...");
        
        // Tentar remover primeiro (caso já exista)
        esp_now_del_peer(peerInfo.peer_addr);
        
        // Tentar adicionar novamente
        addResult = esp_now_add_peer(&peerInfo);
        if (addResult == ESP_OK) {
            Serial.println("   ✅ Peer broadcast adicionado na segunda tentativa");
        } else if (addResult == ESP_ERR_ESPNOW_EXIST) {
            Serial.println("   ℹ️  Peer já existe - OK");
        } else {
            Serial.println("   ❌ Falha persistente: " + String(addResult));
        }
    } else {
        Serial.println("✅ Peer broadcast adicionado com sucesso");
    }
    
    initialized = true;
    Serial.println("✅ ESP-NOW Controller inicializado: " + deviceName);
    Serial.println("🎯 Canal: " + String(wifiChannel) + " | MAC: " + getLocalMacString());
    
    // Enviar broadcast de descoberta
    sendDiscoveryBroadcast();

#ifdef SLAVE_MODE
    if (!WiFi.isConnected()) {
        scanAndSyncToMasterChannel();
    }
#endif
    
    return true;
}

void ESPNowController::update() {
    if (!initialized) return;
    
    // Limpar peers offline periodicamente
    static unsigned long lastCleanup = 0;
    if (millis() - lastCleanup > PEER_OFFLINE_TIMEOUT) {
        cleanupOfflinePeers();
        lastCleanup = millis();
    }
    
    // Discovery periódico — MASTER: só espNowTask/rediscoverSlaves (evita triple broadcast)
#ifndef MASTER_MODE
    static unsigned long lastDiscovery = 0;
    if (millis() - lastDiscovery > DISCOVERY_INTERVAL_MS) {
        LOG_ESPNOW_INFO("Auto-discovery broadcast");
        sendDiscoveryBroadcast();
        lastDiscovery = millis();
    }
#endif

#ifdef SLAVE_MODE
    // Re-anunciar a cada 10s enquanto Master não respondeu
    static unsigned long lastSlaveAnnounce = 0;
    if (!masterContactEstablished && millis() - lastSlaveAnnounce > 10000) {
        Serial.println("📡 [SLAVE] Re-anunciando presença (Master ainda não contactou)...");
        sendDiscoveryBroadcast();
        lastSlaveAnnounce = millis();
    }
#endif
}

void ESPNowController::end() {
    if (initialized) {
        esp_now_deinit();
        initialized = false;
        Serial.println("📡 ESP-NOW Controller finalizado");
    }
}

bool ESPNowController::sendRelayCommand(const uint8_t* targetMac, int relayNumber, const String& action,
                                      int duration, uint32_t commandId, int cycleOffDuration,
                                      const String& mode) {
    if (!initialized) {
        Serial.println("❌ ESP-NOW não inicializado");
        return false;
    }
    
    ESPNowMessage message = {};
    message.type = MessageType::RELAY_COMMAND;
    getLocalMac(message.senderId);
    memcpy(message.targetId, targetMac, 6);
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    
    RelayCommandData cmdData = {};
    cmdData.relayNumber = relayNumber;
    cmdData.state = (action == "on");
    cmdData.duration = duration;
    cmdData.commandId = commandId;
    cmdData.cycleOffDuration = cycleOffDuration;
    strncpy(cmdData.action, action.c_str(), sizeof(cmdData.action) - 1);
    if (mode.length() > 0) {
        strncpy(cmdData.mode, mode.c_str(), sizeof(cmdData.mode) - 1);
    }
    
    message.dataSize = sizeof(RelayCommandData);
    memcpy(message.data, &cmdData, sizeof(RelayCommandData));
    message.checksum = calculateChecksum(message);
    
    bool success = sendMessage(message, targetMac);
    
    if (success) {
        Serial.println("📤 Comando enviado: Relé " + String(relayNumber) + " -> " + action + 
                      " para " + macToString(targetMac) +
                      (mode.length() > 0 ? " mode=" + mode : "") +
                      (cycleOffDuration > 0 ? " off=" + String(cycleOffDuration) + "s" : ""));
    }
    
    return success;
}

bool ESPNowController::sendSetRelayMask(const uint8_t* targetMac, uint8_t mask, uint16_t durationSec,
                                         uint32_t commandId) {
    if (!initialized || !targetMac) {
        return false;
    }
    ESPNowMessage message = {};
    message.type = MessageType::SET_RELAY_MASK;
    getLocalMac(message.senderId);
    memcpy(message.targetId, targetMac, 6);
    message.messageId = ++messageCounter;
    message.timestamp = millis();

    RelayMaskCommandData payload = {};
    payload.mask = mask;
    payload.durationSec = durationSec;
    payload.commandId = commandId;
    message.dataSize = sizeof(RelayMaskCommandData);
    memcpy(message.data, &payload, sizeof(payload));
    message.checksum = calculateChecksum(message);

    const bool ok = sendMessage(message, targetMac);
    if (ok) {
        Serial.printf("[PROC] SET_RELAY_MASK mac=%s mask=0x%02X dur=%u id=%u\n",
                      macToString(targetMac).c_str(), mask, (unsigned)durationSec, (unsigned)commandId);
    }
    return ok;
}

bool ESPNowController::sendRelayCommandAck(const uint8_t* targetMac, const RelayCommandAck& ack) {
    if (!initialized || !targetMac) {
        return false;
    }

    TaskESPNowMessage taskMsg = {};
    taskMsg.type = TASK_MSG_RELAY_ACK;
    WiFi.macAddress(taskMsg.senderMac);
    memcpy(taskMsg.targetMac, targetMac, 6);
    taskMsg.timestamp = millis();
    taskMsg.dataSize = sizeof(RelayCommandAck);

    RelayCommandAck ackCopy = ack;
    uint8_t checksum = 0;
    uint8_t* data = reinterpret_cast<uint8_t*>(&ackCopy);
    for (size_t i = 0; i < sizeof(RelayCommandAck) - 1; i++) {
        checksum ^= data[i];
    }
    ackCopy.checksum = checksum;
    memcpy(taskMsg.data, &ackCopy, sizeof(RelayCommandAck));

    uint8_t msgChecksum = 0;
    uint8_t* msgData = reinterpret_cast<uint8_t*>(&taskMsg);
    for (size_t i = 0; i < sizeof(TaskESPNowMessage) - 1; i++) {
        msgChecksum ^= msgData[i];
    }
    taskMsg.checksum = msgChecksum;

    esp_err_t result = esp_now_send(targetMac, reinterpret_cast<uint8_t*>(&taskMsg), sizeof(TaskESPNowMessage));
    if (result == ESP_OK) {
        messagesSent++;
        return true;
    }
    messagesLost++;
    return false;
}

bool ESPNowController::sendRelayStatus(const uint8_t* targetMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name) {
    if (!initialized) return false;
    
    ESPNowMessage message = {};
    message.type = MessageType::RELAY_STATUS;
    getLocalMac(message.senderId);
    
    if (targetMac) {
        memcpy(message.targetId, targetMac, 6);
    } else {
        memset(message.targetId, 0xFF, 6); // Broadcast
    }
    
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    
    RelayStatusData statusData = {};
    statusData.relayNumber = relayNumber;
    statusData.state = state;
    statusData.hasTimer = hasTimer;
    statusData.remainingTime = remainingTime;
    strncpy(statusData.name, name.c_str(), sizeof(statusData.name) - 1);
    
    message.dataSize = sizeof(RelayStatusData);
    memcpy(message.data, &statusData, sizeof(RelayStatusData));
    message.checksum = calculateChecksum(message);
    
    return sendMessage(message, targetMac);
}

bool ESPNowController::sendAllRelaysStatus(const uint8_t* targetMac, const SingleRelayState relayStates[8], uint8_t numRelays) {
    if (!initialized) return false;
    
    ESPNowMessage message = {};
    message.type = MessageType::ALL_RELAYS_STATUS;
    getLocalMac(message.senderId);
    
    if (targetMac) {
        memcpy(message.targetId, targetMac, 6);
    } else {
        memset(message.targetId, 0xFF, 6); // Broadcast
    }
    
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    
    AllRelaysStatus allStatus = {};
    allStatus.timestamp = millis();
    allStatus.numRelays = (numRelays > 8) ? 8 : numRelays;
    
    // Copiar estados dos relés
    for (uint8_t i = 0; i < allStatus.numRelays; i++) {
        allStatus.relays[i] = relayStates[i];
    }
    
    // Calcular checksum
    uint8_t checksum = 0;
    uint8_t* data = (uint8_t*)&allStatus;
    for (size_t i = 0; i < sizeof(AllRelaysStatus) - 1; i++) {
        checksum ^= data[i];
    }
    allStatus.checksum = checksum;
    
    message.dataSize = sizeof(AllRelaysStatus);
    memcpy(message.data, &allStatus, sizeof(AllRelaysStatus));
    message.checksum = calculateChecksum(message);
    
    Serial.println("\n📤 ========================================");
    Serial.println("📤 ENVIANDO ALL_RELAYS_STATUS");
    Serial.println("📤 ========================================");
    Serial.println("📡 Destino: " + (targetMac ? macToString(targetMac) : "BROADCAST"));
    Serial.println("🔌 Total de relés: " + String(allStatus.numRelays));
    Serial.println("========================================\n");
    
    return sendMessage(message, targetMac);
}

bool ESPNowController::sendPersistentStateSync(const uint8_t* targetMac, const PersistentRelayStateData& states) {
    if (!initialized) return false;
    
    ESPNowMessage message = {};
    message.type = MessageType::PERSISTENT_STATE_SYNC;
    getLocalMac(message.senderId);
    
    if (targetMac) {
        memcpy(message.targetId, targetMac, 6);
    } else {
        memset(message.targetId, 0xFF, 6); // Broadcast
    }
    
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    
    // Copiar estados persistentes (já vêm com checksum calculado)
    PersistentRelayStateData persistentStates = states;
    persistentStates.timestamp = millis(); // Atualizar timestamp
    
    // Recalcular checksum
    uint8_t checksum = 0;
    uint8_t* data = (uint8_t*)&persistentStates;
    for (size_t i = 0; i < sizeof(PersistentRelayStateData) - 1; i++) {
        checksum ^= data[i];
    }
    persistentStates.checksum = checksum;
    
    message.dataSize = sizeof(PersistentRelayStateData);
    memcpy(message.data, &persistentStates, sizeof(PersistentRelayStateData));
    message.checksum = calculateChecksum(message);
    
    Serial.println("\n🎯 ========================================");
    Serial.println("🎯 ENVIANDO PERSISTENT_STATE_SYNC");
    Serial.println("🎯 ========================================");
    Serial.println("📡 Destino: " + (targetMac ? macToString(targetMac) : "BROADCAST"));
    Serial.println("🔌 Total de relés: " + String(persistentStates.numRelays));
    Serial.println("📅 Timestamp: " + String(persistentStates.timestamp));
    Serial.println("========================================\n");
    
    return sendMessage(message, targetMac);
}

void ESPNowController::setDeviceName(const String& newDeviceName) {
    if (newDeviceName.length() > 0) {
        deviceName = newDeviceName;
        Serial.println("✅ Device Name actualizado: " + deviceName);
    }
}

bool ESPNowController::sendDeviceInfo(const uint8_t* targetMac, const String& deviceType, uint8_t numRelays, bool operational, uint32_t uptime, uint32_t freeHeap) {
    if (!initialized) return false;
    
    ESPNowMessage message = {};
    message.type = MessageType::DEVICE_INFO;
    getLocalMac(message.senderId);
    
    if (targetMac) {
        memcpy(message.targetId, targetMac, 6);
    } else {
        memset(message.targetId, 0xFF, 6); // Broadcast
    }
    
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    
    DeviceInfoData infoData = {};
    strncpy(infoData.deviceName, deviceName.c_str(), sizeof(infoData.deviceName) - 1);
    strncpy(infoData.deviceType, deviceType.c_str(), sizeof(infoData.deviceType) - 1);
    infoData.numRelays = numRelays;
    infoData.operational = operational;
    infoData.uptime = uptime;
    infoData.freeHeap = freeHeap;
    infoData.wifiChannel = wifiChannel;  // ✅ Incluir canal WiFi do dispositivo!
    memset(infoData.padding, 0, sizeof(infoData.padding));  // Zerar padding
    
    message.dataSize = sizeof(DeviceInfoData);
    memcpy(message.data, &infoData, sizeof(DeviceInfoData));
    message.checksum = calculateChecksum(message);
    
    return sendMessage(message, targetMac);
}

bool ESPNowController::sendPing(const uint8_t* targetMac) {
    if (!initialized) return false;
    
    ESPNowMessage message = {};
    message.type = MessageType::PING;
    getLocalMac(message.senderId);
    memcpy(message.targetId, targetMac, 6);
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    message.dataSize = 0;
    message.checksum = calculateChecksum(message);
    
    bool success = sendMessage(message, targetMac);
    
    if (success) {
        Serial.println("🏓 Ping enviado para: " + macToString(targetMac));
    }
    
    return success;
}

bool ESPNowController::sendDiscoveryBroadcast() {
    if (!initialized) return false;
    
    Serial.println("📢 Enviando broadcast de descoberta...");
    
    uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // BROADCAST real — Slaves respondem automaticamente com DEVICE_INFO
    ESPNowMessage message = {};
    message.type = MessageType::BROADCAST;
    getLocalMac(message.senderId);
    memcpy(message.targetId, broadcastMac, 6);
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    message.dataSize = 0;
    message.checksum = calculateChecksum(message);
    sendMessage(message, broadcastMac);

#ifdef MASTER_MODE
    return sendDeviceInfo(nullptr, "MasterController", 8, true, millis(), ESP.getFreeHeap());
#else
    return sendDeviceInfo(broadcastMac, "RelayBox", 8, true, millis(), ESP.getFreeHeap());
#endif
}

bool ESPNowController::sendWiFiCredentialsBroadcast(const String& ssid, const String& password, uint8_t channel) {
    if (!initialized) return false;
    
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
    
    // Criar estrutura de credenciais
    WiFiCredentialsData creds;
    
    // Copiar SSID
    strncpy(creds.ssid, ssid.c_str(), sizeof(creds.ssid) - 1);
    creds.ssid[sizeof(creds.ssid) - 1] = '\0';
    
    // Copiar senha
    strncpy(creds.password, password.c_str(), sizeof(creds.password) - 1);
    creds.password[sizeof(creds.password) - 1] = '\0';
    
    // Obter canal WiFi
    if (channel > 0) {
        creds.channel = channel;  // Usar canal fornecido
        Serial.println("📶 Usando canal fornecido: " + String(channel));
    } else {
        // Obter canal atual do WiFi
        wifi_second_chan_t secondChan;
        esp_wifi_get_channel(&creds.channel, &secondChan);
        Serial.println("📶 Usando canal atual: " + String(creds.channel));
    }
    
    // Calcular checksum
    creds.calculateChecksum();
    
    // Debug
    Serial.println("📤 Dados a enviar:");
    Serial.println("   SSID: " + ssid);
    Serial.print("   Senha: ");
    for (size_t i = 0; i < password.length(); i++) Serial.print("*");
    Serial.println();
    Serial.println("   Canal: " + String(creds.channel));
    Serial.println("   Tamanho: " + String(sizeof(creds)) + " bytes");
    Serial.println("   Checksum: 0x" + String(creds.checksum, HEX));
    Serial.println("   Alcance: TODOS os dispositivos");
    
    // Validar antes de enviar
    if (!creds.isValid()) {
        Serial.println("❌ Erro: Checksum inválido antes de enviar!");
        return false;
    }
    
    // Criar mensagem ESP-NOW
    ESPNowMessage message = {};
    message.type = MessageType::WIFI_CREDENTIALS;
    
    // Configurar sender (local)
    getLocalMac(message.senderId);
    
    // Configurar target (broadcast)
    uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(message.targetId, broadcastMac, 6);
    
    // Configurar dados
    message.dataSize = sizeof(WiFiCredentialsData);
    memcpy(message.data, &creds, sizeof(WiFiCredentialsData));
    
    // Enviar mensagem
    bool success = sendMessage(message, broadcastMac);
    
    if (success) {
        Serial.println("✅ Credenciais enviadas em broadcast com sucesso!");
        Serial.println("================================================\n");
        return true;
    } else {
        Serial.println("❌ Falha ao enviar credenciais via ESP-NOW");
        Serial.println("================================================\n");
        return false;
    }
}

bool ESPNowController::hopToChannel(uint8_t channel) {
    if (channel < 1 || channel > 13) {
        return false;
    }
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) {
        wifiChannel = channel;
        return true;
    }
    Serial.printf("❌ hopToChannel(%u) falhou: %d\n", channel, err);
    return false;
}

uint8_t ESPNowController::getCurrentRadioChannel() const {
    wifi_second_chan_t secondChan;
    uint8_t ch = 0;
    esp_wifi_get_channel(&ch, &secondChan);
    return ch;
}

bool ESPNowController::sendChannelChangeNotification(uint8_t oldChannel, uint8_t newChannel, uint8_t reason) {
    if (!initialized || oldChannel < 1 || oldChannel > 13 || newChannel < 1 || newChannel > 13) {
        return false;
    }

    Serial.println("\n📢 === NOTIFICANDO MUDANÇA DE CANAL ===");
    Serial.printf("   Canal Anterior: %u\n", oldChannel);
    Serial.printf("   Novo Canal: %u\n", newChannel);
    Serial.printf("   Motivo: %u\n", reason);

    ChannelChangeNotification notification = {};
    notification.oldChannel = oldChannel;
    notification.newChannel = newChannel;
    notification.reason = reason;
    notification.changeTime = millis();
    notification.checksum = 0;
    {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&notification);
        uint8_t cs = 0;
        for (size_t i = 0; i < sizeof(notification) - 1; ++i) {
            cs ^= raw[i];
        }
        notification.checksum = cs;
    }

    ESPNowMessage message = {};
    message.type = MessageType::CHANNEL_CHANGE;
    getLocalMac(message.senderId);
    uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(message.targetId, broadcastMac, 6);
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    message.dataSize = sizeof(notification);
    memcpy(message.data, &notification, sizeof(notification));
    message.checksum = calculateChecksum(message);

    const uint8_t savedChannel = getCurrentRadioChannel();
    if (savedChannel != oldChannel) {
        Serial.printf("⚠️ Hop temporário para canal %u (notificar slaves)\n", oldChannel);
        hopToChannel(oldChannel);
        delay(50);
    }

    int successCount = 0;
    for (int i = 0; i < 3; ++i) {
        if (sendMessage(message, broadcastMac)) {
            successCount++;
        }
        delay(100);
    }

    if (savedChannel != newChannel) {
        Serial.printf("📶 Retornando para canal %u\n", newChannel);
        hopToChannel(newChannel);
    }

    const bool ok = successCount > 0;
    Serial.printf("%s Notificação CHANNEL_CHANGE (%d/3)\n", ok ? "✅" : "❌", successCount);
    Serial.println("=====================================\n");
    return ok;
}

bool ESPNowController::addPeer(const uint8_t* macAddress, const String& deviceName) {
    if (!initialized) return false;
    
    // ⚠️ DEPRECADO: Este método usa o canal LOCAL (pode estar errado!)
    // Use addPeerWithChannel() para especificar o canal do peer
    
    // Verificar se peer já existe
    if (peerExists(macAddress)) {
        Serial.println("⚠️ Peer já existe: " + macToString(macAddress));
        return true;
    }
    
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, macAddress, 6);
    peerInfo.channel = wifiChannel;  // ⚠️ Canal LOCAL (pode ser incorreto!)
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    
    esp_err_t result = esp_now_add_peer(&peerInfo);
    
    if (result == ESP_OK) {
        // Adicionar à lista de peers conhecidos
        PeerInfo newPeer;
        memcpy(newPeer.macAddress, macAddress, 6);
        newPeer.deviceName = deviceName.isEmpty() ? "Unknown" : deviceName;
        newPeer.deviceType = "Unknown";
        newPeer.online = true;
        newPeer.lastSeen = millis();
        newPeer.rssi = -50;
        
        knownPeers.push_back(newPeer);
        
        Serial.println("✅ Peer adicionado: " + macToString(macAddress) + 
                      (deviceName.isEmpty() ? "" : " (" + deviceName + ")"));
        return true;
    } else {
        Serial.println("❌ Erro ao adicionar peer: " + macToString(macAddress) + 
                      " (Código: " + String(result) + ")");
        return false;
    }
}

bool ESPNowController::addPeerWithChannel(const uint8_t* macAddress, uint8_t peerChannel, const String& deviceName) {
    if (!initialized) {
        LOG_ESPNOW_ERROR("ESP-NOW não inicializado");
        return false;
    }

    if (peerChannel < 1 || peerChannel > 13) {
        LOG_ESPNOW_ERROR("Canal inválido: " + String(peerChannel));
        return false;
    }

    uint8_t currentChannel;
    wifi_second_chan_t secondChan;
    esp_wifi_get_channel(&currentChannel, &secondChan);
    bool wifiConnected = (WiFi.status() == WL_CONNECTED);

    if (peerChannel != currentChannel && wifiConnected) {
        peerChannel = currentChannel;
    }

    // Debounce: skip re-add se peer já existe no mesmo canal
    if (esp_now_is_peer_exist(macAddress)) {
        esp_now_peer_info_t existingPeer = {};
        if (esp_now_get_peer(macAddress, &existingPeer) == ESP_OK &&
            existingPeer.channel == peerChannel) {
            updatePeerInfo(macAddress, deviceName, "");
            LOG_ESPNOW_DEBUG("Peer inalterado (canal " + String(peerChannel) + "): " + macToString(macAddress));
            return true;
        }
    }

    LOG_ESPNOW_DEBUG("\n🔧 ADD PEER: " + macToString(macAddress) + " canal " + String(peerChannel));
    
    // Configurar peer com CANAL CORRETO
    wifi_mode_t wifiMode = WiFi.getMode();
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, macAddress, 6);
    peerInfo.channel = peerChannel;
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    (void)wifiMode;
    
    // Se canal mudou, remover peer antigo antes de re-adicionar
    if (esp_now_is_peer_exist(macAddress)) {
        esp_now_del_peer(macAddress);
        for (auto it = knownPeers.begin(); it != knownPeers.end(); ++it) {
            if (memcmp(it->macAddress, macAddress, 6) == 0) {
                knownPeers.erase(it);
                break;
            }
        }
    }

    LOG_ESPNOW_DEBUG("🔗 Adicionando peer no canal " + String(peerChannel) + "...");
    
    // Adicionar peer
    esp_err_t result = esp_now_add_peer(&peerInfo);
    
    if (result == ESP_OK) {
        // Adicionar à lista de peers conhecidos
        PeerInfo newPeer;
        memcpy(newPeer.macAddress, macAddress, 6);
        newPeer.deviceName = deviceName.isEmpty() ? "Unknown" : deviceName;
        newPeer.deviceType = "Unknown";
        newPeer.online = true;
        newPeer.lastSeen = millis();
        newPeer.rssi = -50;
        
        knownPeers.push_back(newPeer);
        
        LOG_ESPNOW_INFO("✅ Peer adicionado canal " + String(peerChannel) + ": " + macToString(macAddress));
        return true;
        
    } else {
        LOG_ESPNOW_ERROR("Erro ao adicionar peer " + macToString(macAddress) + " código " + String(result));
        return false;
    }
}

bool ESPNowController::addPeerSafe(const uint8_t* macAddress, const String& deviceName) {
    if (!initialized) {
        Serial.println("❌ ESP-NOW não inicializado");
        return false;
    }
    
    Serial.println("\n🔧 === ADD PEER SAFE ===");
    Serial.println("📍 MAC: " + macToString(macAddress));
    Serial.println("📝 Nome: " + (deviceName.isEmpty() ? "Unknown" : deviceName));
    
    // 1. Verificar canal WiFi atual
    uint8_t currentChannel;
    wifi_second_chan_t secondChan;
    esp_wifi_get_channel(&currentChannel, &secondChan);
    
    Serial.println("📶 Canal WiFi atual: " + String(currentChannel));
    Serial.println("📶 Canal configurado: " + String(wifiChannel));
    
    // 2. Se peer já existe, remover para forçar atualização
    if (esp_now_is_peer_exist(macAddress)) {
        Serial.println("⚠️ Peer já existe - removendo para atualizar...");
        esp_now_del_peer(macAddress);
        
        // Remover da lista local também
        for (auto it = knownPeers.begin(); it != knownPeers.end(); ++it) {
            if (memcmp(it->macAddress, macAddress, 6) == 0) {
                knownPeers.erase(it);
                break;
            }
        }
    }
    
    // 3. Configurar informações do peer com CANAL CORRETO
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, macAddress, 6);
    peerInfo.channel = currentChannel;  // ✅ Usar canal WiFi atual!
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    
    Serial.println("🔗 Adicionando peer no canal " + String(peerInfo.channel) + "...");
    
    // 4. Adicionar peer
    esp_err_t result = esp_now_add_peer(&peerInfo);
    
    if (result == ESP_OK) {
        // Adicionar à lista de peers conhecidos
        PeerInfo newPeer;
        memcpy(newPeer.macAddress, macAddress, 6);
        newPeer.deviceName = deviceName.isEmpty() ? "Unknown" : deviceName;
        newPeer.deviceType = "Unknown";
        newPeer.online = true;
        newPeer.lastSeen = millis();
        newPeer.rssi = -50;
        
        knownPeers.push_back(newPeer);
        
        Serial.println("✅ Peer adicionado com sucesso!");
        Serial.println("========================\n");
        return true;
        
    } else {
        Serial.println("❌ ERRO ao adicionar peer!");
        Serial.println("💡 Código de erro: 0x" + String(result, HEX));
        
        // Detalhar erros comuns
        if (result == ESP_ERR_ESPNOW_EXIST) {
            Serial.println("⚠️ Erro: Peer já existe (ESP_ERR_ESPNOW_EXIST)");
        } else if (result == ESP_ERR_ESPNOW_FULL) {
            Serial.println("⚠️ Erro: Lista de peers cheia (ESP_ERR_ESPNOW_FULL)");
        } else if (result == ESP_ERR_ESPNOW_ARG) {
            Serial.println("⚠️ Erro: Argumento inválido (ESP_ERR_ESPNOW_ARG)");
        }
        
        Serial.println("========================\n");
        return false;
    }
}

bool ESPNowController::removePeer(const uint8_t* macAddress) {
    if (!initialized) return false;
    
    esp_err_t result = esp_now_del_peer(macAddress);
    
    if (result == ESP_OK) {
        // Remover da lista de peers conhecidos
        for (auto it = knownPeers.begin(); it != knownPeers.end(); ++it) {
            if (memcmp(it->macAddress, macAddress, 6) == 0) {
                knownPeers.erase(it);
                break;
            }
        }
        
        Serial.println("✅ Peer removido: " + macToString(macAddress));
        return true;
    } else {
        Serial.println("❌ Erro ao remover peer: " + macToString(macAddress));
        return false;
    }
}

std::vector<PeerInfo> ESPNowController::getPeerList() {
    return knownPeers;
}

bool ESPNowController::peerExists(const uint8_t* macAddress) {
    return esp_now_is_peer_exist(macAddress);
}

int ESPNowController::getPeerCount() {
    esp_now_peer_num_t peerNum;
    esp_now_get_peer_num(&peerNum);
    return peerNum.total_num;
}

void ESPNowController::setRelayCommandCallback(
    std::function<void(const uint8_t* senderMac, uint32_t commandId, int relayNumber, const String& action,
                       int duration, const String& mode, int cycleOffDuration)> callback) {
    this->relayCommandCallback = callback;
}

void ESPNowController::setPersistentStateCallback(std::function<void(const uint8_t* senderMac, const PersistentRelayStateData& states)> callback) {
    this->persistentStateCallback = callback;
}

void ESPNowController::setRelayStatusCallback(std::function<void(const uint8_t* senderMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name)> callback) {
    this->relayStatusCallback = callback;
}

void ESPNowController::setDeviceInfoCallback(std::function<void(const uint8_t* senderMac, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel)> callback) {
    this->deviceInfoCallback = callback;
}

void ESPNowController::setPingCallback(void (*callback)(const uint8_t* senderMac)) {
    this->pingCallback = callback;
}

void ESPNowController::setPongCallback(void (*callback)(const uint8_t* senderMac)) {
    this->pongCallback = callback;
}

void ESPNowController::setWiFiCredentialsCallback(void (*callback)(const String& ssid, const String& password, uint8_t channel)) {
    this->wifiCredentialsCallback = callback;
}

void ESPNowController::setErrorCallback(void (*callback)(const String& error)) {
    this->errorCallback = callback;
}

void ESPNowController::setHandshakeCallback(void (*callback)(const uint8_t* senderMac, uint32_t sessionId, const String& deviceName, bool wifiConnected)) {
    this->handshakeCallback = callback;
}

String ESPNowController::macToString(const uint8_t* mac) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}


bool ESPNowController::stringToMac(const String& macStr, uint8_t* mac) {
    if (macStr.length() != 17) return false;
    
    int values[6];
    if (sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", 
               &values[0], &values[1], &values[2], 
               &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)values[i];
    }
    
    return true;
}

void ESPNowController::getLocalMac(uint8_t* mac) {
    WiFi.macAddress(mac);
}

String ESPNowController::getLocalMacString() {
    uint8_t mac[6];
    getLocalMac(mac);
    return macToString(mac);
}

String ESPNowController::getStatsJSON() {
    DynamicJsonDocument doc(512);
    
    doc["deviceName"] = deviceName;
    doc["initialized"] = initialized;
    doc["channel"] = wifiChannel;
    doc["localMac"] = getLocalMacString();
    doc["messagesSent"] = messagesSent;
    doc["messagesReceived"] = messagesReceived;
    doc["messagesLost"] = messagesLost;
    doc["peerCount"] = getPeerCount();
    doc["knownPeersCount"] = knownPeers.size();
    
    JsonArray peers = doc.createNestedArray("peers");
    for (const auto& peer : knownPeers) {
        JsonObject peerObj = peers.createNestedObject();
        peerObj["mac"] = macToString(peer.macAddress);
        peerObj["name"] = peer.deviceName;
        peerObj["type"] = peer.deviceType;
        peerObj["online"] = peer.online;
        peerObj["lastSeen"] = peer.lastSeen;
        peerObj["rssi"] = peer.rssi;
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}

void ESPNowController::printStatus() {
    Serial.println("📡 === STATUS ESP-NOW ===");
    Serial.println("🆔 Dispositivo: " + deviceName);
    Serial.println("📶 Canal: " + String(wifiChannel));
    Serial.println("🆔 MAC Local: " + getLocalMacString());
    Serial.println("✅ Inicializado: " + String(initialized ? "Sim" : "Não"));
    Serial.println("📊 Mensagens enviadas: " + String(messagesSent));
    Serial.println("📊 Mensagens recebidas: " + String(messagesReceived));
    Serial.println("📊 Mensagens perdidas: " + String(messagesLost));
    Serial.println("👥 Peers conectados: " + String(getPeerCount()));
    Serial.println("👥 Peers conhecidos: " + String(knownPeers.size()));
    
    if (!knownPeers.empty()) {
        Serial.println("\n👥 === PEERS CONHECIDOS ===");
        for (const auto& peer : knownPeers) {
            Serial.println("   " + macToString(peer.macAddress) + " | " + 
                          peer.deviceName + " (" + peer.deviceType + ") | " +
                          (peer.online ? "Online" : "Offline") + " | RSSI: " + String(peer.rssi));
        }
    }
    
    Serial.println("=========================");
}

// ===== MÉTODOS PRIVADOS =====

bool ESPNowController::sendMessage(const ESPNowMessage& message, const uint8_t* targetMac) {
    if (!initialized) return false;
    
    uint8_t* sendMac = nullptr;
    bool isBroadcast = false;
    
    if (targetMac) {
        sendMac = const_cast<uint8_t*>(targetMac);
        // Verificar se é broadcast
        isBroadcast = (targetMac[0] == 0xFF && targetMac[1] == 0xFF && 
                      targetMac[2] == 0xFF && targetMac[3] == 0xFF && 
                      targetMac[4] == 0xFF && targetMac[5] == 0xFF);
    } else {
        static uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        sendMac = broadcastMac;
        isBroadcast = true;
    }
    
    // ⭐ CORREÇÃO CRÍTICA: Adicionar peer automaticamente se for UNICAST!
    if (!isBroadcast && !peerExists(sendMac)) {
        Serial.println("🔗 Peer não registrado, adicionando automaticamente: " + macToString(sendMac));
        
        // 🚨 CRÍTICO: Obter canal ATUAL do Slave (home channel)
        // Si el Slave está escaneando canales, wifiChannel puede no ser el canal actual
        uint8_t currentChannel;
        wifi_second_chan_t secondChan;
        esp_wifi_get_channel(&currentChannel, &secondChan);
        
        // Si el canal actual es 0 o inválido, usar wifiChannel como fallback
        if (currentChannel == 0 || currentChannel > 13) {
            currentChannel = wifiChannel;
        }
        
        Serial.println("   📶 Usando canal atual do Slave (home channel): " + String(currentChannel));
        
        // Adicionar peer com informações básicas
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, sendMac, 6);
        peerInfo.channel = currentChannel;  // ✅ Usar canal ATUAL, no wifiChannel!
        peerInfo.encrypt = false;
        peerInfo.ifidx = WIFI_IF_STA;
        
        esp_err_t addResult = esp_now_add_peer(&peerInfo);
        
        if (addResult == ESP_OK) {
            Serial.println("✅ Peer adicionado automaticamente no canal " + String(currentChannel) + "!");
            
            // Atualizar lista local
            PeerInfo newPeer;
            memcpy(newPeer.macAddress, sendMac, 6);
            newPeer.deviceName = "Auto-" + macToString(sendMac).substring(12);
            newPeer.deviceType = "Unknown";
            newPeer.online = true;
            newPeer.lastSeen = millis();
            newPeer.rssi = -50;
            knownPeers.push_back(newPeer);
        } else {
            Serial.println("❌ Falha ao adicionar peer automaticamente: " + String(addResult));
            Serial.println("⚠️ Mensagem pode não ser entregue!");
            // Continua tentando enviar mesmo assim
        }
    } else if (!isBroadcast && peerExists(sendMac)) {
        // 🚨 CRÍTICO: Verificar si el canal del peer coincide con el home channel actual
        // Si no coincide, ESP-NOW fallará con "Peer channel is not equal to the home channel"
        uint8_t currentChannel;
        wifi_second_chan_t secondChan;
        esp_wifi_get_channel(&currentChannel, &secondChan);
        
        esp_now_peer_info_t peerInfo;
        if (esp_now_get_peer(sendMac, &peerInfo) == ESP_OK) {
            if (peerInfo.channel != currentChannel && currentChannel > 0 && currentChannel <= 13) {
                Serial.println("\n⚠️ ========================================");
                Serial.println("⚠️ CONFLITO DE CANAL DETECTADO!");
                Serial.println("⚠️ ========================================");
                Serial.println("   📶 Canal do peer: " + String(peerInfo.channel));
                Serial.println("   📶 Canal atual (home channel): " + String(currentChannel));
                Serial.println("   💡 Actualizando peer para sincronizar canales...");
                
                // Remover peer existente
                esp_now_del_peer(sendMac);
                
                // Agregar peer con canal actual
                peerInfo.channel = currentChannel;
                esp_err_t addResult = esp_now_add_peer(&peerInfo);
                
                if (addResult == ESP_OK) {
                    Serial.println("   ✅ Peer actualizado al canal " + String(currentChannel) + "!");
                } else {
                    Serial.println("   ❌ Falha ao actualizar peer: " + String(addResult));
                }
                Serial.println("========================================\n");
            }
        }
    }
    
    // Enviar mensagem
    esp_err_t result = esp_now_send(sendMac, (uint8_t*)&message, sizeof(ESPNowMessage));
    
    if (result == ESP_OK) {
        messagesSent++;
        return true;
    } else {
        messagesLost++;
        Serial.println("❌ Erro ao enviar mensagem: " + String(result));
        Serial.println("💡 Código de erro: 0x" + String(result, HEX));
        if (result == ESP_ERR_ESPNOW_NOT_FOUND) {
            Serial.println("⚠️ Peer não encontrado - Tentando adicionar...");
        }
        return false;
    }
}

void ESPNowController::processReceivedMessage(const ESPNowMessage& message, const uint8_t* senderMac) {
    if (!validateMessage(message, senderMac)) {
        Serial.println("❌ Mensagem inválida recebida de: " + macToString(senderMac));
        return;
    }

    // Contacto com Master (Slave) — mensagem unicast válida de outro dispositivo
    static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (senderMac && memcmp(senderMac, broadcastMac, 6) != 0) {
        lastMasterContactMs = millis();
        masterContactEstablished = true;
    }
    
    // 🔧 AUTO-ADD PEER: Adicionar remetente automaticamente usando método seguro
    bool peerExists = esp_now_is_peer_exist(senderMac);
    bool isNewPeer = false;
    if (!peerExists) {
        Serial.println("🔗 Auto-adicionando peer (método seguro): " + macToString(senderMac));
        addPeerSafe(senderMac, "Auto-" + macToString(senderMac).substring(12));
        isNewPeer = true;
        
        // ⭐ POTENCIA MÁXIMA: Solicitar DEVICE_INFO inmediatamente para obtener nombre real
        if (MasterSlaveManager::getInstance()) {
            Serial.println("📋 Solicitando DEVICE_INFO del nuevo peer...");
            delay(50); // Pequeño delay para asegurar que el peer está listo
            MasterSlaveManager::getInstance()->requestSlaveInfo(senderMac);
        }
    }
    
    // Atualizar informações do peer
    updatePeerInfo(senderMac, "", "");
    
    switch (message.type) {
        case MessageType::RELAY_COMMAND: {
            if (relayCommandCallback && message.dataSize >= 24) {
                RelayCommandData cmdData = {};
                size_t copySize = message.dataSize < sizeof(RelayCommandData)
                                      ? message.dataSize
                                      : sizeof(RelayCommandData);
                memcpy(&cmdData, message.data, copySize);
                
                String modeStr = cmdData.mode[0] != '\0' ? String(cmdData.mode) : "";
                Serial.println("📥 Comando recebido de " + macToString(senderMac) + 
                              ": Relé " + String(cmdData.relayNumber) + " -> " + String(cmdData.action) +
                              (modeStr.length() > 0 ? " mode=" + modeStr : "") +
                              " (cmdId=" + String(cmdData.commandId) + ")");
                
                relayCommandCallback(senderMac, cmdData.commandId, cmdData.relayNumber,
                                     String(cmdData.action), cmdData.duration, modeStr,
                                     cmdData.cycleOffDuration);
            }
            break;
        }
        
        case MessageType::RELAY_STATUS: {
            if (relayStatusCallback && message.dataSize >= sizeof(RelayStatusData)) {
                RelayStatusData statusData;
                memcpy(&statusData, message.data, sizeof(RelayStatusData));
                
                Serial.println("📥 Status recebido de " + macToString(senderMac) + 
                              ": " + String(statusData.name) + " -> " + (statusData.state ? "ON" : "OFF"));
                
                relayStatusCallback(senderMac, statusData.relayNumber, statusData.state, 
                                  statusData.hasTimer, statusData.remainingTime, String(statusData.name));
            }
            break;
        }
        
        case MessageType::DEVICE_INFO: {
            LOG_ESPNOW_DEBUG("\n🔍 DEVICE_INFO de " + macToString(senderMac));
            
            if (deviceInfoCallback && message.dataSize >= sizeof(DeviceInfoData)) {
                DeviceInfoData infoData;
                memcpy(&infoData, message.data, sizeof(DeviceInfoData));
                
                LOG_ESPNOW_INFO("📥 DEVICE_INFO: " + String(infoData.deviceName) +
                               " canal " + String(infoData.wifiChannel));
                
                // ✅ SINCRONIZAÇÃO DE CANAL - CRÍTICO!
                // ESPNOW_LOCK_WIFI_CHANNEL: manter canal do WiFi STA (não saltar de canal)
                if (infoData.wifiChannel > 0 && infoData.wifiChannel != wifiChannel) {
                    bool stationConnected = (WiFi.status() == WL_CONNECTED);

                    if (stationConnected || ESPNOW_LOCK_WIFI_CHANNEL) {
                        Serial.println("\n⚠️ === CONFLITO DE CANAL DETECTADO ===");
                        Serial.println("⚠️ Dispositivo remoto está no canal " + String(infoData.wifiChannel));
                        Serial.println("⚠️ Nós estamos no canal " + String(wifiChannel) +
                                         (stationConnected ? " (WiFi Station conectado)" : " (canal fixo)"));
                        Serial.println("💡 Mantendo canal local — peer deve sincronizar");
                        Serial.println("=======================================\n");
                    } else {
                        Serial.println("\n🔧 === SINCRONIZAÇÃO DE CANAL DETECTADA ===");
                        Serial.println("⚠️ Dispositivo remoto está no canal " + String(infoData.wifiChannel));
                        Serial.println("⚠️ Nós estamos no canal " + String(wifiChannel));
                        Serial.println("🔄 Atualizando para canal " + String(infoData.wifiChannel) + "...");

                        esp_wifi_set_channel(infoData.wifiChannel, WIFI_SECOND_CHAN_NONE);
                        wifiChannel = infoData.wifiChannel;
                        delay(100);

                        Serial.println("✅ Canal local atualizado para " + String(wifiChannel));
                        Serial.println("=======================================\n");
                    }
                }
                
                // ✅ ADICIONAR PEER COM CANAL CORRETO (novo método!)
                // ⚠️ CORREÇÃO: Sempre usar o canal do MASTER (wifiChannel), não do Slave
                // Se WiFi Station está conectado, o Master não pode mudar de canal
                // Então o peer deve ser adicionado com o canal atual do Master
                addPeerWithChannel(senderMac, wifiChannel, String(infoData.deviceName));
                
                // Atualizar informações detalhadas do peer
                updatePeerInfo(senderMac, String(infoData.deviceName), String(infoData.deviceType));
                
                LOG_ESPNOW_DEBUG("🔍 deviceInfoCallback: " + String(infoData.deviceName));
                
                deviceInfoCallback(senderMac, String(infoData.deviceName), String(infoData.deviceType), 
                                 infoData.numRelays, infoData.operational, infoData.wifiChannel);
            } else {
                if (!deviceInfoCallback) {
                    LOG_ESPNOW_WARN("deviceInfoCallback não configurado");
                }
                if (message.dataSize < sizeof(DeviceInfoData)) {
                    Serial.println("❌ [DEBUG] Tamanho insuficiente: " + String(message.dataSize) + " < " + String(sizeof(DeviceInfoData)));
                }
            }
            break;
        }
        
        case MessageType::PING: {
            LOG_ESPNOW_DEBUG("🏓 Ping de " + macToString(senderMac));
            
            // Responder com PONG
            ESPNowMessage pongMsg = {};
            pongMsg.type = MessageType::PONG;
            getLocalMac(pongMsg.senderId);
            memcpy(pongMsg.targetId, senderMac, 6);
            pongMsg.messageId = ++messageCounter;
            pongMsg.timestamp = millis();
            pongMsg.dataSize = 0;
            pongMsg.checksum = calculateChecksum(pongMsg);
            
            sendMessage(pongMsg, senderMac);
            
            if (pingCallback) {
                pingCallback(senderMac);
            }
            break;
        }
        
        case MessageType::PONG: {
            Serial.println("🏓 Pong recebido de: " + macToString(senderMac));
            if (pongCallback) {
                pongCallback(senderMac);
            }
            break;
        }
        
        case MessageType::BROADCAST: {
            Serial.println("📢 Broadcast recebido de: " + macToString(senderMac));
            
            // ✅ CORREÇÃO CRÍTICA: Slaves devem responder automaticamente com DEVICE_INFO quando recebem BROADCAST
            // Isso permite que o master descubra slaves rapidamente
            #ifndef MASTER_MODE
            // Modo SLAVE: Responder automaticamente com DEVICE_INFO quando recebe BROADCAST
            Serial.println("\n📤 ========================================");
            Serial.println("📤 [SLAVE] RESPONDENDO AO BROADCAST");
            Serial.println("📤 ========================================");
            Serial.println("📡 De: " + macToString(senderMac));
            Serial.println("📤 Enviando DEVICE_INFO...");
            delay(100); // Pequeno delay para evitar colisões
            bool sent = sendDeviceInfo(senderMac, "RelayCommandBox", 8, true, millis(), ESP.getFreeHeap());
            if (sent) {
                Serial.println("✅ DEVICE_INFO enviado ao master com sucesso!");
            } else {
                Serial.println("❌ Falha ao enviar DEVICE_INFO ao master");
            }
            Serial.println("========================================\n");
            #elif defined(MASTER_MODE)
            if (MasterSlaveManager::getInstance()) {
                TrustedSlave* slave = MasterSlaveManager::getInstance()->getTrustedSlave(senderMac);
                if (slave) {
                    MasterSlaveManager::getInstance()->touchSlaveLink(senderMac, "broadcast_rx");
                }
                bool needsInfo = isNewPeer || !slave || 
                                 slave->deviceName.startsWith("Auto-") || 
                                 slave->deviceName.startsWith("Slave-") || 
                                 slave->deviceName == "Unknown";
                
                addPeerSafe(senderMac, "Auto-" + macToString(senderMac).substring(12));
                
                if (needsInfo) {
                    Serial.println("📋 Broadcast slave — handshake + PONG + DEVICE_INFO...");
                    delay(50);
                    MasterSlaveManager::getInstance()->requestSlaveInfo(senderMac);
                }
                
                // PONG unicast acelera discovery multi-canal do slave
                ESPNowMessage pongMsg = {};
                pongMsg.type = MessageType::PONG;
                getLocalMac(pongMsg.senderId);
                memcpy(pongMsg.targetId, senderMac, 6);
                pongMsg.messageId = ++messageCounter;
                pongMsg.timestamp = millis();
                pongMsg.dataSize = 0;
                pongMsg.checksum = calculateChecksum(pongMsg);
                sendMessage(pongMsg, senderMac);
                
                delay(50);
                sendDeviceInfo(senderMac, deviceName, 8, true, millis(), ESP.getFreeHeap());
            }
            #endif
            break;
        }
        
        case MessageType::ERROR: {
            Serial.println("\n❌ ========================================");
            Serial.println("❌ MENSAGEM DE ERRO RECEBIDA");
            Serial.println("❌ ========================================");
            Serial.println("📨 De: " + macToString(senderMac));
            
            if (message.dataSize > 0 && message.dataSize < 200) {
                String errorMsg = String((char*)message.data);
                errorMsg = errorMsg.substring(0, message.dataSize);
                Serial.println("💬 Mensagem: " + errorMsg);
            }
            
            // Chamar callback de erro se definido
            if (errorCallback) {
                String errorMsg = String((char*)message.data);
                errorCallback(errorMsg.substring(0, message.dataSize));
            }
            
            Serial.println("========================================\n");
            break;
        }
        
        case MessageType::WIFI_CREDENTIALS: {
            if (wifiCredentialsCallback && message.dataSize >= sizeof(WiFiCredentialsData)) {
                WiFiCredentialsData creds;
                memcpy(&creds, message.data, sizeof(WiFiCredentialsData));
                
                Serial.println("📶 Credenciais WiFi recebidas de: " + macToString(senderMac));
                
                // Validar credenciais usando checksum
                if (validateWiFiCredentials(creds)) {
                    Serial.println("✅ Credenciais validadas com sucesso!");
                    Serial.println("   SSID: " + String(creds.ssid));
                    Serial.println("   Canal: " + String(creds.channel));
                    Serial.println("   Checksum: 0x" + String(creds.checksum, HEX));
                    
                    // Chamar callback para processar credenciais
                    wifiCredentialsCallback(String(creds.ssid), String(creds.password), creds.channel);
                } else {
                    Serial.println("❌ Credenciais WiFi inválidas (checksum falhou)");
                }
            }
            break;
        }
        
        case MessageType::HANDSHAKE_REQUEST: {
            if (message.dataSize >= sizeof(HandshakeData)) {
                HandshakeData handshake;
                memcpy(&handshake, message.data, sizeof(HandshakeData));
                
                if (validateHandshake(handshake)) {
                    Serial.println("🤝 Handshake recebido de: " + macToString(senderMac));
                    Serial.println("   Sessão: " + String(handshake.sessionId));
                    Serial.println("   Dispositivo: " + String(handshake.deviceName));
                    Serial.println("   WiFi: " + String(handshake.wifiConnected ? "Conectado" : "Desconectado"));
                    
                    // Responder ao handshake
                    respondToHandshake(senderMac, handshake.sessionId);
                    
                    // Chamar callback se definido
                    if (handshakeCallback) {
                        handshakeCallback(senderMac, handshake.sessionId, String(handshake.deviceName), handshake.wifiConnected);
                    }
                } else {
                    Serial.println("❌ Handshake inválido recebido de: " + macToString(senderMac));
                }
            }
            break;
        }
        
        case MessageType::HANDSHAKE_RESPONSE: {
            if (message.dataSize >= sizeof(HandshakeData)) {
                HandshakeData handshake;
                memcpy(&handshake, message.data, sizeof(HandshakeData));
                
                if (validateHandshake(handshake)) {
                    Serial.println("🤝 Resposta de handshake recebida de: " + macToString(senderMac));
                    Serial.println("   Sessão: " + String(handshake.sessionId));
                    Serial.println("   Dispositivo: " + String(handshake.deviceName));
                    Serial.println("   WiFi: " + String(handshake.wifiConnected ? "Conectado" : "Desconectado"));
                    
                    // Chamar callback se definido
                    if (handshakeCallback) {
                        handshakeCallback(senderMac, handshake.sessionId, String(handshake.deviceName), handshake.wifiConnected);
                    }
                } else {
                    Serial.println("❌ Resposta de handshake inválida de: " + macToString(senderMac));
                }
            }
            break;
        }
        
        case MessageType::CONNECTIVITY_CHECK: {
            Serial.println("🔍 Solicitação de verificação de conectividade de: " + macToString(senderMac));
            
            // Responder com relatório de conectividade
            uint32_t sessionId = generateSessionId();
            sendConnectivityReport(senderMac, sessionId);
            
            // Chamar callback se definido
            if (connectivityCheckCallback) {
                connectivityCheckCallback(senderMac);
            }
            break;
        }
        
        case MessageType::CONNECTIVITY_REPORT: {
            if (message.dataSize >= sizeof(ConnectivityReportData)) {
                ConnectivityReportData report;
                memcpy(&report, message.data, sizeof(ConnectivityReportData));
                
                Serial.println("📊 Relatório de conectividade recebido de: " + macToString(senderMac));
                Serial.println("   Sessão: " + String(report.sessionId));
                Serial.println("   WiFi: " + String(report.wifiConnected ? "Conectado" : "Desconectado"));
                Serial.println("   RSSI: " + String(report.wifiRSSI) + " dBm");
                Serial.println("   Canal: " + String(report.wifiChannel));
                Serial.println("   Uptime: " + String(report.uptime / 1000) + "s");
                Serial.println("   Heap: " + String(report.freeHeap) + " bytes");
                Serial.println("   Mensagens: " + String(report.messageCount));
                Serial.println("   Operacional: " + String(report.operational ? "Sim" : "Não"));
                
                // Chamar callback se definido
                if (connectivityReportCallback) {
                    connectivityReportCallback(senderMac, report.sessionId, report.wifiConnected, report.wifiRSSI, report.uptime);
                }
            }
            break;
        }
        
        case MessageType::ALL_RELAYS_STATUS: {
            // 🔄 FASE 3: Estado completo de todos os relays do slave
            if (message.dataSize >= sizeof(AllRelaysStatus)) {
                AllRelaysStatus allStatus;
                memcpy(&allStatus, message.data, sizeof(AllRelaysStatus));
                
                // ✅ PROTECCIÓN: Evitar procesar el mismo mensaje múltiples veces
                static uint32_t lastProcessedTimestamp = 0;
                static uint8_t lastProcessedMacBytes[6] = {0};
                static bool hasLastProcessed = false;
                
                // Verificar si es el mismo mensaje (mismo MAC y timestamp similar dentro de 200ms)
                bool isDuplicate = false;
                if (hasLastProcessed && 
                    memcmp(senderMac, lastProcessedMacBytes, 6) == 0 &&
                    abs((int32_t)(allStatus.timestamp - lastProcessedTimestamp)) < 200) {
                    isDuplicate = true;
                }
                
                if (!isDuplicate) {
                    // Actualizar tracking
                    lastProcessedTimestamp = allStatus.timestamp;
                    memcpy(lastProcessedMacBytes, senderMac, 6);
                    hasLastProcessed = true;
                } else {
                    // ✅ OTIMIZADO: Mensaje duplicado, solo procesar callbacks sin imprimir
                    MasterSlaveManager* masterManager = MasterSlaveManager::getInstance();
                    if (masterManager) {
                        masterManager->setProcessingStatusResponse(true);
                    }
                    
                    if (relayStatusCallback) {
                        for (uint8_t i = 0; i < allStatus.numRelays && i < 8; i++) {
                            relayStatusCallback(senderMac, i, 
                                              allStatus.relays[i].state == 1, 
                                              allStatus.relays[i].hasTimer == 1,
                                              allStatus.relays[i].remainingTime,
                                              "Relé " + String(i));
                        }
                    }
                    
                    if (masterManager) {
                        bool relayStates[8] = {false};
                        const uint8_t n = (allStatus.numRelays < 8) ? allStatus.numRelays : 8;
                        for (uint8_t i = 0; i < n; i++) {
                            relayStates[i] = allStatus.relays[i].state == 1;
                        }
                        masterManager->notifyAllRelaysStatusReceived(senderMac, relayStates, n);
                        masterManager->setProcessingStatusResponse(false);
                    }
                    break; // Salir sin imprimir
                }
                
                char relayMask[9];
                formatAllRelaysMask(allStatus, relayMask);
                Serial.printf("[ESPNOW] ALL_RELAYS %s mask=%s\n",
                              macToString(senderMac).c_str(), relayMask);
                LOG_ESPNOW_DEBUG("\n📊 ESTADO COMPLETO DE TODOS OS RELAYS");
                LOG_ESPNOW_DEBUG("📥 De: " + macToString(senderMac));
                
                for (uint8_t i = 0; i < allStatus.numRelays && i < 8; i++) {
                    LOG_ESPNOW_DEBUG("   Relé " + String(i) + ": " +
                                    (allStatus.relays[i].state ? "ON" : "OFF"));
                }
                
                // ✅ Proteção contra loop infinito: marcar que estamos processando resposta de status
                MasterSlaveManager* masterManager = MasterSlaveManager::getInstance();
                if (masterManager) {
                    masterManager->setProcessingStatusResponse(true);
                }
                
                // ⭐ POTENCIA MÁXIMA: Chamar callback de relayStatus para cada relé
                // Este callback é configurado em main.cpp e chama processRelayStatusReceived no MasterSlaveManager
                if (relayStatusCallback) {
                    // ✅ OTIMIZADO: Sin logs individuales en callbacks (ya se mostró arriba)
                    for (uint8_t i = 0; i < allStatus.numRelays && i < 8; i++) {
                        relayStatusCallback(senderMac, i, 
                                          allStatus.relays[i].state == 1, 
                                          allStatus.relays[i].hasTimer == 1,
                                          allStatus.relays[i].remainingTime,
                                          "Relé " + String(i));
                    }
                } else {
                    Serial.println("❌ ERRO: relayStatusCallback NÃO configurado!");
                }
                
                // ✅ Desmarcar flag após processar (sem delay desnecessário)
                if (masterManager) {
                    bool relayStates[8] = {false};
                    const uint8_t n = (allStatus.numRelays < 8) ? allStatus.numRelays : 8;
                    for (uint8_t i = 0; i < n; i++) {
                        relayStates[i] = allStatus.relays[i].state == 1;
                    }
                    masterManager->notifyAllRelaysStatusReceived(senderMac, relayStates, n);
                    masterManager->setProcessingStatusResponse(false);
                }
            } else {
                Serial.printf("❌ ALL_RELAYS_STATUS tamanho inválido: %d (esperado: %d)\n", 
                             message.dataSize, sizeof(AllRelaysStatus));
            }
            break;
        }
        
        case MessageType::PERSISTENT_STATE_SYNC: {
            // 🎯 PERSISTÊNCIA: Estados persistentes sincronizados via ESP-NOW
            Serial.println("\n🎯 ========================================");
            Serial.println("🎯 PERSISTENT_STATE_SYNC RECEBIDO!");
            Serial.println("🎯 ========================================");
            Serial.println("📥 De: " + macToString(senderMac));
            Serial.println("📦 Tamanho recebido: " + String(message.dataSize) + " bytes");
            Serial.println("📦 Tamanho esperado: " + String(sizeof(PersistentRelayStateData)) + " bytes");
            
            if (message.dataSize >= sizeof(PersistentRelayStateData)) {
                PersistentRelayStateData persistentStates;
                memcpy(&persistentStates, message.data, sizeof(PersistentRelayStateData));
                
                // Validar checksum
                uint8_t calculatedChecksum = 0;
                uint8_t* data = (uint8_t*)&persistentStates;
                for (size_t i = 0; i < sizeof(PersistentRelayStateData) - 1; i++) {
                    calculatedChecksum ^= data[i];
                }
                
                if (calculatedChecksum != persistentStates.checksum) {
                    Serial.println("❌ ERRO: Checksum inválido!");
                    Serial.println("   Esperado: " + String(calculatedChecksum));
                    Serial.println("   Recebido: " + String(persistentStates.checksum));
                    break;
                }
                
                Serial.println("\n💾 ========================================");
                Serial.println("💾 ESTADOS PERSISTENTES RECEBIDOS");
                Serial.println("💾 ========================================");
                Serial.println("📥 De: " + macToString(senderMac));
                Serial.println("⏰ Timestamp: " + String(persistentStates.timestamp));
                Serial.println("🔌 Total de relays: " + String(persistentStates.numRelays));
                Serial.println("📦 Versão: " + String(persistentStates.version));
                Serial.println("----------------------------------------");
                
                // Mostrar estado de cada relé
                for (uint8_t i = 0; i < persistentStates.numRelays && i < 8; i++) {
                    String stateIcon = persistentStates.relays[i].state ? "🟢 ON " : "🔴 OFF";
                    String timerInfo = "";
                    String persistentInfo = "";
                    
                    if (persistentStates.relays[i].hasTimer) {
                        uint32_t remaining = 0;
                        if (persistentStates.relays[i].timerEndTime > millis()) {
                            remaining = (persistentStates.relays[i].timerEndTime - millis()) / 1000;
                        }
                        timerInfo = " ⏱️ " + String(remaining) + "s restantes";
                    }
                    
                    if (persistentStates.relays[i].isPersistent) {
                        persistentInfo = " 🔒 PERSISTENTE";
                    }
                    
                    Serial.println("   Relé " + String(i) + ": " + stateIcon + timerInfo + persistentInfo);
                }
                
                Serial.println("========================================\n");
                if (persistentStateCallback) {
                    persistentStateCallback(senderMac, persistentStates);
                } else {
                    Serial.println("💡 Estados persistentes recebidos - Prontos para guardar em NVS");
                }
            } else {
                Serial.println("❌ ERRO: PERSISTENT_STATE_SYNC recebido mas tamanho inválido!");
                Serial.println("   Recebido: " + String(message.dataSize) + " bytes");
                Serial.println("   Esperado: " + String(sizeof(PersistentRelayStateData)) + " bytes");
            }
            break;
        }
        
        default:
            Serial.println("❓ Tipo de mensagem desconhecido: " + String((int)message.type));
            break;
    }
}

uint8_t ESPNowController::calculateChecksum(const ESPNowMessage& message) {
    uint8_t checksum = 0;
    const uint8_t* data = (const uint8_t*)&message;
    
    // Calcular checksum de todos os campos exceto o próprio checksum
    for (size_t i = 0; i < sizeof(ESPNowMessage) - 1; i++) {
        checksum ^= data[i];
    }
    
    return checksum;
}

bool ESPNowController::isDuplicateMessage(const ESPNowMessage& message) {
    const uint8_t* mac = message.senderId;
    if (message.messageId == 0) {
        return false;
    }

    for (size_t i = 0; i < recentSenderCount; i++) {
        if (memcmp(recentSenders[i].mac, mac, 6) == 0) {
            if (recentSenders[i].lastMessageId == message.messageId &&
                (millis() - recentSenders[i].lastSeenMs) < 60000UL) {
                return true;
            }
            return false;
        }
    }
    return false;
}

void ESPNowController::recordMessageId(const ESPNowMessage& message) {
    if (message.messageId == 0) {
        return;
    }

    const uint8_t* mac = message.senderId;
    for (size_t i = 0; i < recentSenderCount; i++) {
        if (memcmp(recentSenders[i].mac, mac, 6) == 0) {
            recentSenders[i].lastMessageId = message.messageId;
            recentSenders[i].lastSeenMs = millis();
            return;
        }
    }

    if (recentSenderCount < MAX_RECENT_SENDERS) {
        memcpy(recentSenders[recentSenderCount].mac, mac, 6);
        recentSenders[recentSenderCount].lastMessageId = message.messageId;
        recentSenders[recentSenderCount].lastSeenMs = millis();
        recentSenderCount++;
    } else {
        size_t oldestIdx = 0;
        unsigned long oldestSeen = recentSenders[0].lastSeenMs;
        for (size_t i = 1; i < MAX_RECENT_SENDERS; i++) {
            if (recentSenders[i].lastSeenMs < oldestSeen) {
                oldestSeen = recentSenders[i].lastSeenMs;
                oldestIdx = i;
            }
        }
        memcpy(recentSenders[oldestIdx].mac, mac, 6);
        recentSenders[oldestIdx].lastMessageId = message.messageId;
        recentSenders[oldestIdx].lastSeenMs = millis();
    }
}

bool ESPNowController::validateMessage(const ESPNowMessage& message, const uint8_t* senderMac) {
    ESPNowMessage tempMsg = message;
    tempMsg.checksum = 0;
    uint8_t calculatedChecksum = calculateChecksum(tempMsg);
    
    if (calculatedChecksum != message.checksum) {
        Serial.println("❌ Checksum inválido");
        return false;
    }
    
    if (message.dataSize > sizeof(message.data)) {
        Serial.println("❌ Tamanho de dados inválido");
        return false;
    }

    ESPNowMessage dedupMsg = message;
    if (senderMac) {
        memcpy(dedupMsg.senderId, senderMac, 6);
    }

    if (isDuplicateMessage(dedupMsg)) {
        DEBUG_PRINTLN("❌ Mensagem duplicada (messageId repetido)");
        return false;
    }

    recordMessageId(dedupMsg);
    return true;
}

bool ESPNowController::setWifiChannel(uint8_t channel) {
    if (channel < 1 || channel > 13) {
        return false;
    }

#if ESPNOW_LOCK_WIFI_CHANNEL
    if (WiFi.status() == WL_CONNECTED) {
        channel = WiFi.channel();
    }
#endif

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    wifiChannel = channel;

    uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (esp_now_is_peer_exist(broadcastMac)) {
        esp_now_del_peer(broadcastMac);
    }

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastMac, 6);
    peerInfo.channel = channel;
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    return esp_now_add_peer(&peerInfo) == ESP_OK || esp_now_add_peer(&peerInfo) == ESP_ERR_ESPNOW_EXIST;
}

bool ESPNowController::scanAndSyncToMasterChannel() {
    if (!initialized || WiFi.isConnected()) {
        return true;
    }

    Serial.println("\n🔍 === SCAN MULTI-CANAL (Slave → Master) ===");
    const uint8_t scanOrder[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
    const size_t scanCount = sizeof(scanOrder) / sizeof(scanOrder[0]);

    for (size_t i = 0; i < scanCount; i++) {
        uint8_t channel = scanOrder[i];
        Serial.print("   Canal " + String(channel) + ": ");

        masterContactEstablished = false;
        if (!setWifiChannel(channel)) {
            Serial.println("❌ falha ao mudar canal");
            continue;
        }

        sendDiscoveryBroadcast();
        delay(450);

        if (masterContactEstablished) {
            Serial.println("✅ Master detectado!");
            Serial.println("================================================\n");
            return true;
        }
        Serial.println("⚪ sem resposta");
    }

    Serial.println("⚠️ Master não encontrado no scan — mantendo canal " + String(wifiChannel));
    Serial.println("================================================\n");
    return false;
}

bool ESPNowController::hasRecentMasterContact() const {
    return masterContactEstablished && (millis() - lastMasterContactMs) < 120000UL;
}

void ESPNowController::updatePeerInfo(const uint8_t* macAddress, const String& deviceName, const String& deviceType) {
    // Procurar peer existente
    for (auto& peer : knownPeers) {
        if (memcmp(peer.macAddress, macAddress, 6) == 0) {
            peer.online = true;
            peer.lastSeen = millis();
            
            if (!deviceName.isEmpty()) {
                peer.deviceName = deviceName;
            }
            if (!deviceType.isEmpty()) {
                peer.deviceType = deviceType;
            }
            return;
        }
    }
    
    // Se não encontrou, adicionar novo peer (FALLBACK - não deveria acontecer!)
    if (!peerExists(macAddress)) {
        Serial.println("⚠️ updatePeerInfo: Peer não existe! Adicionando como fallback...");
        Serial.println("💡 Isso não deveria acontecer se addPeerWithChannel() foi chamado corretamente");
        
        // ⚠️ FALLBACK: Adicionar com canal atual (pode estar errado!)
        uint8_t currentChannel;
        wifi_second_chan_t secondChan;
        esp_wifi_get_channel(&currentChannel, &secondChan);
        
        addPeerWithChannel(macAddress, currentChannel, deviceName);
    }
}

void ESPNowController::cleanupOfflinePeers() {
    unsigned long currentTime = millis();
    
    for (auto& peer : knownPeers) {
        if (currentTime - peer.lastSeen > PEER_OFFLINE_TIMEOUT) {
            peer.online = false;
        }
    }
}

// ===== CALLBACKS ESTÁTICOS =====

void ESPNowController::onDataReceived(const uint8_t* mac, const uint8_t* incomingData, int len) {
    if (!instance) return;
    
    // 🔄 FASE 2: RECONHECER TaskESPNowMessage (do SLAVE) para ACKs
    if (len == sizeof(TaskESPNowMessage)) {
        TaskESPNowMessage taskMsg;
        memcpy(&taskMsg, incomingData, sizeof(TaskESPNowMessage));
        
        // Verificar se é um ACK de relay
        if (taskMsg.type == TASK_MSG_RELAY_ACK) {
            Serial.println("\n🎊 === TaskESPNowMessage ACK DETECTADO ===");
            Serial.println("📥 De: " + macToString(mac));
            Serial.println("📦 Tamanho: " + String(len) + " bytes");
            Serial.println("🆔 Tipo: TASK_MSG_RELAY_ACK");
            
            // Extrair dados do ACK
            if (taskMsg.dataSize >= sizeof(RelayCommandAck)) {
                RelayCommandAck ack;
                memcpy(&ack, taskMsg.data, sizeof(RelayCommandAck));
                
                // Validar checksum (zero checksum byte antes do XOR, igual validateMessage)
                uint8_t receivedChecksum = ack.checksum;
                ack.checksum = 0;
                uint8_t expectedChecksum = 0;
                for (size_t i = 0; i < sizeof(RelayCommandAck) - 1; i++) {
                    expectedChecksum ^= ((uint8_t*)&ack)[i];
                }
                
                if (receivedChecksum == expectedChecksum) {
                    Serial.println("✅ Checksum válido");
                    Serial.println("🆔 Command ID: " + String(ack.commandId));
                    Serial.println("🔌 Relé: " + String(ack.relayNumber));
                    Serial.println("✅ Success: " + String(ack.success ? "Sim" : "Não"));
                    Serial.println("💡 Estado: " + String(ack.currentState ? "ON" : "OFF"));
                    Serial.println("==========================================\n");
                    
                    // Notificar MasterSlaveManager se houver instância
                    if (MasterSlaveManager::getInstance()) {
                        MasterSlaveManager::getInstance()->processRelayCommandAck(ack, mac);
                    }
                } else {
                    Serial.println("❌ Checksum inválido!");
                    Serial.println("==========================================\n");
                }
            }
            return; // Já processado, não tentar como ESPNowMessage
        }
    }
    
    // Aceitar pequenas diferenças de alinhamento (±4 bytes) para ESPNowMessage legacy
    int expectedSize = sizeof(ESPNowMessage);
    int sizeDiff = abs(len - expectedSize);
    
    if (sizeDiff > 4) {
        Serial.println("❌ Tamanho de mensagem inválido: " + String(len));
        Serial.println("💡 Esperado: ~" + String(expectedSize) + " bytes (±4 para alinhamento)");
        Serial.println("💡 Ou: " + String(sizeof(TaskESPNowMessage)) + " bytes (TaskESPNowMessage)");
        return;
    }
    
    // Copiar mensagem com segurança
    ESPNowMessage message;
    memset(&message, 0, sizeof(ESPNowMessage));
    int copySize = min(len, (int)sizeof(ESPNowMessage));
    memcpy(&message, incomingData, copySize);
    
    instance->messagesReceived++;
    instance->processReceivedMessage(message, mac);
}

void ESPNowController::onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (!instance) return;
    
    if (status != ESP_NOW_SEND_SUCCESS) {
        instance->messagesLost++;
        // ✅ OTIMIZADO: Solo loguear si es un problema persistente (evitar spam de falsos negativos)
        // ESP-NOW puede reportar fallo pero el mensaje puede llegar (timing/interferencia)
        static unsigned long lastFailureLog = 0;
        static const uint8_t* lastFailedMac = nullptr;
        static uint8_t lastFailedMacBytes[6] = {0};
        static uint8_t failureCount = 0;
        
        unsigned long now = millis();
        bool isSameMac = (lastFailedMac != nullptr && memcmp(mac_addr, lastFailedMacBytes, 6) == 0);
        
        if (isSameMac && (now - lastFailureLog) < 5000) {
            // Mismo MAC dentro de 5 segundos - incrementar contador
            failureCount++;
        } else {
            // Nuevo MAC o pasaron más de 5 segundos - resetear
            failureCount = 1;
            memcpy(lastFailedMacBytes, mac_addr, 6);
            lastFailedMac = lastFailedMacBytes;
        }
        
        lastFailureLog = now;
        
        // Solo loguear si hay múltiples fallos consecutivos (probable problema real)
        if (failureCount >= 3) {
            Serial.printf("⚠️ Falha ao enviar para %s (%d tentativas consecutivas)\n", 
                         macToString(mac_addr).c_str(), failureCount);
            if (MasterSlaveManager::getInstance()) {
                MasterSlaveManager::getInstance()->notifyEspNowSendFail(mac_addr, failureCount);
            }
        }
    } else {
        // ✅ Resetear contador en caso de éxito
        static uint8_t lastSuccessMacBytes[6] = {0};
        if (memcmp(mac_addr, lastSuccessMacBytes, 6) != 0) {
            memcpy(lastSuccessMacBytes, mac_addr, 6);
        }
    }
}

// ===== MÉTODOS DE VALIDAÇÃO BIDIRECIONAL =====

bool ESPNowController::validateWiFiCredentials(const WiFiCredentialsData& credentials) {
    // Validação SIMPLIFICADA (apenas checksum e dados básicos)
    
    // Verificar SSID não vazio
    if (strlen(credentials.ssid) == 0 || strlen(credentials.ssid) > 32) {
        Serial.println("❌ SSID inválido (vazio ou muito longo)");
        return false;
    }
    
    // Verificar canal válido (1-13)
    if (credentials.channel < 1 || credentials.channel > 13) {
        Serial.println("❌ Canal inválido: " + String(credentials.channel) + " (deve ser 1-13)");
        return false;
    }
    
    // Verificar checksum
    if (!credentials.isValid()) {
        Serial.println("❌ Checksum inválido das credenciais WiFi");
        return false;
    }
    
    Serial.println("✅ Credenciais WiFi validadas com sucesso");
    Serial.println("   SSID: " + String(credentials.ssid));
    Serial.println("   Canal: " + String(credentials.channel));
    return true;
}

// Mantido para uso em handshakes (não usado para credenciais WiFi)
uint8_t ESPNowController::generateValidationCode(const String& text1, const String& text2, uint32_t value) {
    // Gerar código de validação genérico (usado para handshakes)
    // NÃO usado para credenciais WiFi (que usam checksum próprio)
    uint8_t code = 0;
    
    // XOR com texto 1
    for (size_t i = 0; i < text1.length(); i++) {
        code ^= text1.charAt(i);
    }
    
    // XOR com texto 2
    for (size_t i = 0; i < text2.length(); i++) {
        code ^= text2.charAt(i);
    }
    
    // XOR com valor (bytes individuais)
    code ^= (value & 0xFF);
    code ^= ((value >> 8) & 0xFF);
    code ^= ((value >> 16) & 0xFF);
    code ^= ((value >> 24) & 0xFF);
    
    // Adicionar constante para evitar código zero
    code ^= 0xAA;
    
    return code;
}

bool ESPNowController::initiateHandshake(const uint8_t* targetMac) {
    if (!initialized) return false;
    
    ESPNowMessage message = {};
    message.type = MessageType::HANDSHAKE_REQUEST;
    getLocalMac(message.senderId);
    memcpy(message.targetId, targetMac, 6);
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    
    HandshakeData handshake = {};
    handshake.sessionId = generateSessionId();
    handshake.timestamp = message.timestamp;
    handshake.deviceType = 1; // Slave
    strncpy(handshake.deviceName, deviceName.c_str(), sizeof(handshake.deviceName) - 1);
    handshake.protocolVersion = 1;
    handshake.wifiConnected = WiFi.isConnected();
    handshake.validationCode = generateValidationCode(deviceName, String(handshake.sessionId), handshake.timestamp);
    
    handshake.deviceName[sizeof(handshake.deviceName) - 1] = '\0';
    
    message.dataSize = sizeof(HandshakeData);
    memcpy(message.data, &handshake, sizeof(HandshakeData));
    message.checksum = calculateChecksum(message);
    
    Serial.println("🤝 Iniciando handshake bidirecional com " + macToString(targetMac));
    Serial.println("   Sessão: " + String(handshake.sessionId));
    Serial.println("   Dispositivo: " + deviceName);
    Serial.println("   WiFi: " + String(handshake.wifiConnected ? "Conectado" : "Desconectado"));
    
    return sendMessage(message, targetMac);
}

bool ESPNowController::respondToHandshake(const uint8_t* targetMac, uint32_t sessionId) {
    if (!initialized) return false;
    
    ESPNowMessage message = {};
    message.type = MessageType::HANDSHAKE_RESPONSE;
    getLocalMac(message.senderId);
    memcpy(message.targetId, targetMac, 6);
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    
    HandshakeData handshake = {};
    handshake.sessionId = sessionId; // Usar mesmo ID da sessão
    handshake.timestamp = message.timestamp;
    handshake.deviceType = 1; // Slave
    strncpy(handshake.deviceName, deviceName.c_str(), sizeof(handshake.deviceName) - 1);
    handshake.protocolVersion = 1;
    handshake.wifiConnected = WiFi.isConnected();
    handshake.validationCode = generateValidationCode(deviceName, String(sessionId), handshake.timestamp);
    
    handshake.deviceName[sizeof(handshake.deviceName) - 1] = '\0';
    
    message.dataSize = sizeof(HandshakeData);
    memcpy(message.data, &handshake, sizeof(HandshakeData));
    message.checksum = calculateChecksum(message);
    
    Serial.println("🤝 Respondendo handshake para " + macToString(targetMac));
    Serial.println("   Sessão: " + String(sessionId));
    Serial.println("   Dispositivo: " + deviceName);
    Serial.println("   WiFi: " + String(handshake.wifiConnected ? "Conectado" : "Desconectado"));
    
    return sendMessage(message, targetMac);
}

bool ESPNowController::sendConnectivityReport(const uint8_t* targetMac, uint32_t sessionId) {
    if (!initialized) return false;
    
    ESPNowMessage message = {};
    message.type = MessageType::CONNECTIVITY_REPORT;
    getLocalMac(message.senderId);
    if (targetMac) {
        memcpy(message.targetId, targetMac, 6);
    } else {
        memset(message.targetId, 0xFF, 6); // Broadcast
    }
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    
    ConnectivityReportData report = {};
    report.sessionId = sessionId;
    report.timestamp = message.timestamp;
    report.wifiConnected = WiFi.isConnected();
    report.wifiRSSI = WiFi.RSSI();
    report.wifiChannel = WiFi.channel();
    report.uptime = millis();
    report.freeHeap = ESP.getFreeHeap();
    report.messageCount = messagesSent + messagesReceived;
    report.operational = initialized && (ESP.getFreeHeap() > 10000);
    
    message.dataSize = sizeof(ConnectivityReportData);
    memcpy(message.data, &report, sizeof(ConnectivityReportData));
    message.checksum = calculateChecksum(message);
    
    Serial.println("📊 Enviando relatório de conectividade");
    Serial.println("   Sessão: " + String(sessionId));
    Serial.println("   WiFi: " + String(report.wifiConnected ? "Conectado" : "Desconectado"));
    Serial.println("   RSSI: " + String(report.wifiRSSI) + " dBm");
    Serial.println("   Canal: " + String(report.wifiChannel));
    Serial.println("   Heap: " + String(report.freeHeap) + " bytes");
    
    return sendMessage(message, targetMac);
}

bool ESPNowController::requestConnectivityCheck(const uint8_t* targetMac) {
    if (!initialized) return false;
    
    ESPNowMessage message = {};
    message.type = MessageType::CONNECTIVITY_CHECK;
    getLocalMac(message.senderId);
    memcpy(message.targetId, targetMac, 6);
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    message.dataSize = 0;
    message.checksum = calculateChecksum(message);
    
    Serial.println("🔍 Solicitando verificação de conectividade de " + macToString(targetMac));
    
    return sendMessage(message, targetMac);
}

bool ESPNowController::validateHandshake(const HandshakeData& handshake) {
    // Verificar se não é muito antigo (30 segundos)
    unsigned long currentTime = millis();
    if (currentTime > handshake.timestamp && (currentTime - handshake.timestamp) > 30000) {
        Serial.println("❌ Handshake muito antigo");
        return false;
    }
    
    // Verificar versão do protocolo
    if (handshake.protocolVersion != 1) {
        Serial.println("❌ Versão de protocolo incompatível: " + String(handshake.protocolVersion));
        return false;
    }
    
    // Verificar código de validação
    String deviceNameStr = String(handshake.deviceName);
    String sessionStr = String(handshake.sessionId);
    uint8_t expectedCode = generateValidationCode(deviceNameStr, sessionStr, handshake.timestamp);
    
    if (handshake.validationCode != expectedCode) {
        Serial.println("❌ Código de validação do handshake inválido");
        Serial.println("   Esperado: " + String(expectedCode));
        Serial.println("   Recebido: " + String(handshake.validationCode));
        return false;
    }
    
    Serial.println("✅ Handshake validado com sucesso");
    return true;
}

uint32_t ESPNowController::generateSessionId() {
    // Gerar ID único baseado em timestamp e MAC
    uint32_t sessionId = millis();
    uint8_t mac[6];
    getLocalMac(mac);
    
    // XOR com MAC para tornar único
    for (int i = 0; i < 6; i++) {
        sessionId ^= (mac[i] << (i % 4) * 8);
    }
    
    return sessionId;
}