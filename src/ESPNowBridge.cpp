#include "ESPNowBridge.h"
#include <Preferences.h>

// Instância estática para callbacks
ESPNowBridge* ESPNowBridge::instance = nullptr;

ESPNowBridge::ESPNowBridge(RelayCommandBox* relayController, int channel) 
    : localRelayController(relayController), wifiChannel(channel), initialized(false), 
      messageCounter(0), messagesSent(0), messagesReceived(0), messagesLost(0),
      hasLastMasterMac(false) {
    instance = this;
    memset(lastMasterMac, 0, sizeof(lastMasterMac));
    
    // FASE 2: Criar instância do ESPNowController
    espNowController = new ESPNowController("ESP-HIDROWAVE", channel);
}

bool ESPNowBridge::begin() {
    Serial.println("📡 Inicializando ESP-NOW Bridge (FASE 2)...");
    
    // FASE 2: Inicializar ESPNowController
    if (!espNowController->begin()) {
        Serial.println("❌ Erro ao inicializar ESPNowController");
        return false;
    }
    
    // Configurar callbacks do ESPNowController usando function pointers estáticos
    espNowController->setRelayCommandCallback([](const uint8_t* senderMac, uint32_t commandId, int relayNumber,
                                                 const String& action, int duration, const String& mode,
                                                 int cycleOffDuration) {
        if (ESPNowBridge::instance) {
            ESPNowBridge::instance->onRelayCommandReceived(senderMac, commandId, relayNumber, action, duration,
                                                           mode, cycleOffDuration);
        }
    });

    espNowController->setPersistentStateCallback([](const uint8_t* senderMac, const PersistentRelayStateData& states) {
        if (ESPNowBridge::instance) {
            ESPNowBridge::instance->onPersistentStateReceived(senderMac, states);
        }
    });
    
    espNowController->setRelayStatusCallback([](const uint8_t* senderMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name) {
        if (ESPNowBridge::instance) {
            ESPNowBridge::instance->onRelayStatusReceived(senderMac, relayNumber, state, hasTimer, remainingTime, name);
        }
    });
    
    espNowController->setDeviceInfoCallback([](const uint8_t* senderMac, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel) {
        if (ESPNowBridge::instance) {
            ESPNowBridge::instance->onDeviceInfoReceived(senderMac, deviceName, deviceType, numRelays, operational, wifiChannel);
        }
    });
    
    espNowController->setPingCallback([](const uint8_t* senderMac) {
        if (ESPNowBridge::instance) {
            ESPNowBridge::instance->onPingReceived(senderMac);
        }
    });
    
    espNowController->setWiFiCredentialsCallback([](const String& ssid, const String& password, uint8_t channel) {
        if (ESPNowBridge::instance) {
            ESPNowBridge::instance->onWiFiCredentialsReceived(ssid, password, channel);
        }
    });
    
    espNowController->setErrorCallback([](const String& error) {
        if (ESPNowBridge::instance) {
            ESPNowBridge::instance->onErrorReceived(error);
        }
    });

    if (localRelayController) {
        localRelayController->setStateChangeCallback(onStateChangedStatic);
    }
    
    // Manter callbacks de compatibilidade
    esp_now_register_recv_cb(onDataReceived);
    esp_now_register_send_cb(onDataSent);
    
    initialized = true;
    Serial.println("✅ ESP-NOW Bridge inicializado (FASE 2)");
    Serial.println("🆔 MAC Local: " + getLocalMacString());
    Serial.println("📶 Canal: " + String(wifiChannel));
    
    // Enviar broadcast de descoberta
    sendDiscoveryBroadcast();
    
    return true;
}

void ESPNowBridge::update() {
    if (!initialized) return;
    
    // FASE 2: Atualizar ESPNowController
    espNowController->update();
    
    // Limpar dispositivos offline periodicamente (compatibilidade)
    static unsigned long lastCleanup = 0;
    if (millis() - lastCleanup > 60000) {  // A cada 1 minuto
        cleanupOfflineDevices();
        lastCleanup = millis();
    }
    
    // Ping dispositivos conhecidos periodicamente (compatibilidade)
    static unsigned long lastPing = 0;
    if (millis() - lastPing > 30000) {  // A cada 30 segundos
        for (const auto& device : remoteDevices) {
            if (device.online) {
                sendPing(device.mac);
            }
        }
        lastPing = millis();
    }
}

void ESPNowBridge::end() {
    if (initialized) {
        // FASE 2: Finalizar ESPNowController
        if (espNowController) {
            espNowController->end();
        }
        
        // Manter compatibilidade
        esp_now_deinit();
        initialized = false;
        Serial.println("📡 ESP-NOW Bridge finalizado");
    }
}

bool ESPNowBridge::sendRelayCommand(const uint8_t* targetMac, int relayNumber, const String& action, int duration) {
    if (!initialized) {
        Serial.println("❌ ESP-NOW não inicializado");
        return false;
    }
    
    // FASE 2: Usar ESPNowController
    bool success = espNowController->sendRelayCommand(targetMac, relayNumber, action, duration);
    
    if (success) {
        Serial.printf("📤 Comando enviado para %s: Relé %d -> %s", 
                     macToString(targetMac).c_str(), relayNumber, action.c_str());
        if (duration > 0) {
            Serial.printf(" (%ds)", duration);
        }
        Serial.println();
    }
    
    return success;
}

bool ESPNowBridge::sendPing(const uint8_t* targetMac) {
    if (!initialized) return false;
    
    // FASE 2: Usar ESPNowController
    return espNowController->sendPing(targetMac);
}

bool ESPNowBridge::sendDiscoveryBroadcast() {
    if (!initialized) return false;
    
    Serial.println("📢 Enviando broadcast de descoberta ESP-NOW...");
    
    // FASE 2: Usar ESPNowController
    return espNowController->sendDiscoveryBroadcast();
}

bool ESPNowBridge::broadcastSensorData(const String& sensorData) {
    if (!initialized) return false;
    
    // Manter compatibilidade com sistema existente
    ESPNowMessage message = {};
    message.type = MessageType::BROADCAST;
    WiFi.macAddress(message.senderId);
    memset(message.targetId, 0xFF, 6);  // Broadcast
    message.messageId = ++messageCounter;
    message.timestamp = millis();
    
    message.dataSize = min(sensorData.length(), sizeof(message.data));
    memcpy(message.data, sensorData.c_str(), message.dataSize);
    message.checksum = calculateChecksum(message);
    
    uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return sendMessage(message, broadcastMac);
}

bool ESPNowBridge::addRemoteDevice(const uint8_t* mac, const String& name) {
    // FASE 2: Usar ESPNowController
    bool success = espNowController->addPeer(mac, name);
    
    if (success) {
        // Manter compatibilidade com lista local
        for (auto& device : remoteDevices) {
            if (memcmp(device.mac, mac, 6) == 0) {
                device.online = true;
                device.lastSeen = millis();
                if (!name.isEmpty()) device.name = name;
                return true;
            }
        }
        
        // Adicionar novo dispositivo
        RemoteDevice newDevice;
        memcpy(newDevice.mac, mac, 6);
        newDevice.name = name.isEmpty() ? "Dispositivo-" + macToString(mac).substring(12) : name;
        newDevice.deviceType = "Unknown";
        newDevice.online = true;
        newDevice.lastSeen = millis();
        newDevice.rssi = -50;
        newDevice.numRelays = 8;
        newDevice.operational = true;
        
        remoteDevices.push_back(newDevice);
        
        Serial.println("✅ Dispositivo remoto adicionado: " + macToString(mac));
    }
    
    return success;
}

bool ESPNowBridge::removeRemoteDevice(const uint8_t* mac) {
    // FASE 2: Usar ESPNowController
    bool success = espNowController->removePeer(mac);
    
    if (success) {
        // Remover da lista local
        for (auto it = remoteDevices.begin(); it != remoteDevices.end(); ++it) {
            if (memcmp(it->mac, mac, 6) == 0) {
                remoteDevices.erase(it);
                break;
            }
        }
        
        Serial.println("✅ Dispositivo remoto removido: " + macToString(mac));
    }
    
    return success;
}

std::vector<RemoteDevice> ESPNowBridge::getRemoteDevices() {
    return remoteDevices;
}

bool ESPNowBridge::isDeviceOnline(const uint8_t* mac) {
    for (const auto& device : remoteDevices) {
        if (memcmp(device.mac, mac, 6) == 0) {
            return device.online;
        }
    }
    return false;
}

int ESPNowBridge::getOnlineDeviceCount() {
    int count = 0;
    for (const auto& device : remoteDevices) {
        if (device.online) count++;
    }
    return count;
}

// ===== NOVOS MÉTODOS FASE 2 =====

std::vector<PeerInfo> ESPNowBridge::getPeerList() {
    if (espNowController) {
        return espNowController->getPeerList();
    }
    return std::vector<PeerInfo>();
}

String ESPNowBridge::getDetailedStatsJSON() {
    if (espNowController) {
        return espNowController->getStatsJSON();
    }
    return "{}";
}

bool ESPNowBridge::forceDiscovery() {
    if (espNowController) {
        return espNowController->sendDiscoveryBroadcast();
    }
    return false;
}

// ===== CALLBACKS =====

void ESPNowBridge::setRemoteRelayStatusCallback(void (*callback)(const uint8_t* mac, int relay, bool state, int remainingTime, const String& name)) {
    this->remoteRelayStatusCallback = callback;
}

void ESPNowBridge::setDeviceDiscoveryCallback(void (*callback)(const uint8_t* mac, const String& name, const String& type, bool operational)) {
    this->deviceDiscoveryCallback = callback;
}

void ESPNowBridge::setErrorCallback(void (*callback)(const String& error)) {
    this->errorCallback = callback;
}

// ===== CALLBACKS DO ESPNOWCONTROLLER =====

void ESPNowBridge::onRelayCommandReceived(const uint8_t* senderMac, uint32_t commandId, int relayNumber,
                                          const String& action, int duration, const String& mode,
                                          int cycleOffDuration) {
    if (!instance) return;
    Serial.println("📥 Comando recebido de " + macToString(senderMac) + 
                  ": Relé " + String(relayNumber) + " -> " + action +
                  (mode.length() > 0 ? " mode=" + mode : ""));

    memcpy(instance->lastMasterMac, senderMac, 6);
    instance->hasLastMasterMac = true;

    if (action == "status") {
        instance->publishAllRelaysStatus(senderMac);
        instance->sendCommandFeedback(senderMac, commandId, relayNumber, true);
        return;
    }
    
    // Processar comando no RelayController local
    bool success = false;
    if (instance->localRelayController) {
        success = instance->localRelayController->processCommand(relayNumber, action, duration, mode,
                                                                 cycleOffDuration);
    }

    instance->sendCommandFeedback(senderMac, commandId, relayNumber, success);
    if (success) {
        instance->publishAllRelaysStatus(senderMac);
    }
}

void ESPNowBridge::onPersistentStateReceived(const uint8_t* senderMac, const PersistentRelayStateData& states) {
    if (!instance || !instance->localRelayController) return;
    Serial.println("💾 Aplicando PERSISTENT_STATE_SYNC de " + macToString(senderMac));
    if (instance->localRelayController->applyAndSavePersistentStates(states)) {
        instance->publishAllRelaysStatus(senderMac);
    }
}

void ESPNowBridge::sendCommandFeedback(const uint8_t* masterMac, uint32_t commandId, int relayNumber, bool success) {
    if (!espNowController || !masterMac) return;

    RelayCommandAck ack = {};
    ack.commandId = commandId > 0 ? commandId : messageCounter;
    ack.relayNumber = static_cast<uint8_t>(relayNumber);
    ack.success = success ? 1 : 0;
    if (localRelayController && relayNumber >= 0 && relayNumber < 8) {
        ack.currentState = localRelayController->getRelayState(relayNumber) ? 1 : 0;
    }
    ack.timestamp = millis();

    bool ackSent = false;
    for (int attempt = 0; attempt < 3 && !ackSent; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ackSent = espNowController->sendRelayCommandAck(masterMac, ack);
    }
    if (!ackSent) {
        Serial.println("⚠️ [SLAVE] ACK não enviado após 3 tentativas — master usará ALL_RELAYS fallback");
    }
}

void ESPNowBridge::publishAllRelaysStatus(const uint8_t* masterMac) {
    if (!espNowController || !localRelayController || !masterMac) return;

    SingleRelayState states[8];
    localRelayController->fillAllRelaysStatus(states);
    espNowController->sendAllRelaysStatus(masterMac, states, 8);
}

void ESPNowBridge::onStateChangedStatic(int relayNumber, bool state, int remainingTime) {
    (void)relayNumber;
    (void)state;
    (void)remainingTime;
    if (!instance || !instance->hasLastMasterMac) return;
    instance->publishAllRelaysStatus(instance->lastMasterMac);
}

void ESPNowBridge::onRelayStatusReceived(const uint8_t* senderMac, int relayNumber, bool state, bool hasTimer, int remainingTime, const String& name) {
    if (!instance) return;
    Serial.printf("📥 Status remoto de %s: %s -> %s", 
                 macToString(senderMac).c_str(),
                 name.c_str(), 
                 state ? "ON" : "OFF");
    
    if (hasTimer) {
        Serial.printf(" (%ds restantes)", remainingTime);
    }
    Serial.println();
    
    // Chamar callback de compatibilidade
    if (instance->remoteRelayStatusCallback) {
        instance->remoteRelayStatusCallback(senderMac, relayNumber, state, remainingTime, name);
    }
}

void ESPNowBridge::onDeviceInfoReceived(const uint8_t* senderMac, const String& deviceName, const String& deviceType, uint8_t numRelays, bool operational, uint8_t wifiChannel) {
    if (!instance) return;
    
    Serial.println("\n🎉 === DEVICE INFO RECEBIDO! ===");
    Serial.println("📥 Dispositivo descoberto: " + deviceName + " (" + deviceType + ")");
    Serial.println("📡 MAC: " + macToString(senderMac));
    Serial.println("🔌 Relés: " + String(numRelays));
    Serial.println("✅ Operacional: " + String(operational ? "Sim" : "Não"));
    
    // ⭐ CORREÇÃO CRÍTICA: Registrar peer para comunicação bidirecional!
    if (instance->espNowController) {
        if (!instance->espNowController->peerExists(senderMac)) {
            Serial.println("🔗 Registrando peer bidirecional...");
            if (instance->espNowController->addPeer(senderMac, deviceName)) {
                Serial.println("✅ Peer bidirecional registrado - Master PODE receber do Slave!");
            } else {
                Serial.println("❌ Falha ao registrar peer bidirecional");
            }
        } else {
            Serial.println("ℹ️ Peer já registrado");
        }
    }
    
    // Atualizar lista local
    instance->updateRemoteDevice(senderMac, deviceName, deviceType, operational);
    
    // Chamar callback de compatibilidade
    Serial.println("📞 Chamando callback deviceDiscoveryCallback...");
    if (instance->deviceDiscoveryCallback) {
        instance->deviceDiscoveryCallback(senderMac, deviceName, deviceType, operational);
        Serial.println("✅ Callback executado - Slave deve estar na lista!");
    } else {
        Serial.println("⚠️ Callback NÃO configurado!");
    }
    Serial.println("================================\n");
}

void ESPNowBridge::onPingReceived(const uint8_t* senderMac) {
    if (!instance) return;
    Serial.println("🏓 Ping recebido de: " + macToString(senderMac));
    
    // ⭐ CORREÇÃO CRÍTICA: Registrar sender como peer se não existir
    if (instance->espNowController) {
        if (!instance->espNowController->peerExists(senderMac)) {
            Serial.println("🔗 Registrando peer ao receber ping: " + macToString(senderMac));
            instance->espNowController->addPeer(senderMac, "Master");
        }
        
        // Responder com PONG
        Serial.println("🏓 Enviando PONG para: " + macToString(senderMac));
        instance->espNowController->sendPing(senderMac);
    }
}

void ESPNowBridge::onWiFiCredentialsReceived(const String& ssid, const String& password, uint8_t channel) {
    if (!instance) return;
    
    Serial.println("\n📶 === CREDENCIAIS WiFi RECEBIDAS ===");
    Serial.println("📡 Recebidas de: Master");
    Serial.println("   SSID: " + ssid);
    Serial.println("   Canal: " + String(channel));
    Serial.println("   Senha: [OCULTA]");
    
    // ⭐ CORREÇÃO CRÍTICA: Preparar para comunicação bidirecional
    // O Master será registrado como peer quando enviar primeira mensagem
    Serial.println("🔗 Preparando comunicação bidirecional com Master...");
    
    // Conectar ao WiFi
    Serial.println("🔌 Conectando ao WiFi...");
    
    // Configurar canal se necessário
    if (channel > 0 && channel <= 14) {
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        Serial.println("📶 Canal WiFi configurado para: " + String(channel));
    }
    
    // Conectar ao WiFi
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Aguardar conexão (máximo 30 segundos)
    unsigned long startTime = millis();
    int dots = 0;
    
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime < 30000)) {
        delay(500);
        Serial.print(".");
        dots++;
        if (dots >= 60) {
            Serial.println();
            dots = 0;
        }
    }
    
    if (dots > 0) Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("✅ Conectado ao WiFi com sucesso!");
        Serial.println("🌐 IP: " + WiFi.localIP().toString());
        Serial.println("📶 SSID: " + WiFi.SSID());
        Serial.println("📡 Canal: " + String(channel));
        
        // Salvar credenciais para uso futuro
        Preferences prefs;
        prefs.begin("wifi", false);
        prefs.putString("ssid", ssid);
        prefs.putString("password", password);
        prefs.putUChar("channel", channel);
        prefs.end();
        
        Serial.println("💾 Credenciais salvas para reconexão automática");
        Serial.println("==========================================\n");
    } else {
        Serial.println("❌ Falha ao conectar ao WiFi");
        Serial.println("💡 Verifique se as credenciais estão corretas");
        Serial.println("💡 Verifique se a rede está no alcance");
        Serial.println("==========================================\n");
    }
}

void ESPNowBridge::onErrorReceived(const String& error) {
    if (!instance) return;
    Serial.println("❌ Erro ESP-NOW: " + error);
    
    if (instance->errorCallback) {
        instance->errorCallback(error);
    }
}

// ===== UTILITÁRIOS =====

String ESPNowBridge::macToString(const uint8_t* mac) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

bool ESPNowBridge::stringToMac(const String& macStr, uint8_t* mac) {
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

String ESPNowBridge::getLocalMacString() {
    if (espNowController) {
        return espNowController->getLocalMacString();
    }
    
    uint8_t mac[6];
    WiFi.macAddress(mac);
    return macToString(mac);
}

ESPNowController* ESPNowBridge::getESPNowController() {
    return espNowController;
}

String ESPNowBridge::getStatsJSON() {
    DynamicJsonDocument doc(512);
    
    doc["initialized"] = initialized;
    doc["channel"] = wifiChannel;
    doc["localMac"] = getLocalMacString();
    doc["messagesSent"] = messagesSent;
    doc["messagesReceived"] = messagesReceived;
    doc["messagesLost"] = messagesLost;
    doc["remoteDevices"] = remoteDevices.size();
    doc["onlineDevices"] = getOnlineDeviceCount();
    
    // FASE 2: Adicionar estatísticas do ESPNowController
    if (espNowController) {
        doc["espNowController"] = true;
        doc["peerCount"] = espNowController->getPeerCount();
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}

void ESPNowBridge::printStatus() {
    Serial.println("\n📡 === STATUS ESP-NOW BRIDGE (FASE 2) ===");
    Serial.println("✅ Inicializado: " + String(initialized ? "Sim" : "Não"));
    Serial.println("📶 Canal: " + String(wifiChannel));
    Serial.println("🆔 MAC Local: " + getLocalMacString());
    Serial.println("📊 Mensagens enviadas: " + String(messagesSent));
    Serial.println("📊 Mensagens recebidas: " + String(messagesReceived));
    Serial.println("📊 Mensagens perdidas: " + String(messagesLost));
    Serial.println("👥 Dispositivos remotos: " + String(remoteDevices.size()));
    Serial.println("🟢 Dispositivos online: " + String(getOnlineDeviceCount()));
    
    // FASE 2: Mostrar estatísticas do ESPNowController
    if (espNowController) {
        Serial.println("🔧 ESPNowController: Ativo");
        Serial.println("👥 Peers ESP-NOW: " + String(espNowController->getPeerCount()));
    }
    
    if (!remoteDevices.empty()) {
        Serial.println("\n👥 === DISPOSITIVOS REMOTOS ===");
        for (const auto& device : remoteDevices) {
            Serial.println("   " + macToString(device.mac) + " | " + 
                          device.name + " (" + device.deviceType + ") | " +
                          (device.online ? "🟢 Online" : "🔴 Offline") + 
                          " | Relés: " + String(device.numRelays));
        }
    }
    
    Serial.println("================================\n");
}

// ===== MÉTODOS PRIVADOS (COMPATIBILIDADE) =====

bool ESPNowBridge::sendMessage(const ESPNowMessage& message, const uint8_t* targetMac) {
    if (!initialized) return false;
    
    esp_err_t result = esp_now_send(targetMac, (uint8_t*)&message, sizeof(ESPNowMessage));
    
    if (result == ESP_OK) {
        messagesSent++;
        return true;
    } else {
        messagesLost++;
        Serial.println("❌ Erro ao enviar mensagem ESP-NOW: " + String(result));
        return false;
    }
}

void ESPNowBridge::processReceivedMessage(const ESPNowMessage& message, const uint8_t* senderMac) {
    // FASE 2: Usar validação do ESPNowController se disponível
    if (espNowController) {
        // Deixar o ESPNowController processar a mensagem
        return;
    }
    
    // Validação de compatibilidade (legacy)
    if (!validateMessage(message)) {
        Serial.println("❌ Mensagem ESP-NOW inválida de: " + macToString(senderMac));
        return;
    }
    
    // Atualizar dispositivo remoto
    updateRemoteDevice(senderMac, "", "", true);
    
    switch (message.type) {
        case MessageType::RELAY_STATUS: {
            if (remoteRelayStatusCallback && message.dataSize >= sizeof(RelayStatusData)) {
                RelayStatusData statusData;
                memcpy(&statusData, message.data, sizeof(RelayStatusData));
                
                Serial.printf("📥 Status remoto de %s: %s -> %s", 
                             macToString(senderMac).c_str(),
                             statusData.name, 
                             statusData.state ? "ON" : "OFF");
                
                if (statusData.hasTimer) {
                    Serial.printf(" (%ds restantes)", statusData.remainingTime);
                }
                Serial.println();
                
                remoteRelayStatusCallback(senderMac, statusData.relayNumber, statusData.state, 
                                        statusData.remainingTime, String(statusData.name));
            }
            break;
        }
        
        case MessageType::BROADCAST: {
            Serial.println("📢 Broadcast recebido de: " + macToString(senderMac));
            #ifndef MASTER_MODE
            if (espNowController) {
                espNowController->sendDeviceInfo(senderMac, "RelayCommandBox", 8, true, millis(), ESP.getFreeHeap());
            }
            #endif
            break;
        }
        
        default:
            Serial.println("❓ Tipo de mensagem ESP-NOW desconhecido: " + String((int)message.type));
            break;
    }
}

uint8_t ESPNowBridge::calculateChecksum(const ESPNowMessage& message) {
    uint8_t checksum = 0;
    const uint8_t* data = (const uint8_t*)&message;
    
    for (size_t i = 0; i < sizeof(ESPNowMessage) - 1; i++) {
        checksum ^= data[i];
    }
    
    return checksum;
}

bool ESPNowBridge::validateMessage(const ESPNowMessage& message) {
    // Verificar checksum
    ESPNowMessage tempMsg = message;
    tempMsg.checksum = 0;
    uint8_t calculatedChecksum = calculateChecksum(tempMsg);
    
    if (calculatedChecksum != message.checksum) {
        Serial.println("❌ Checksum inválido");
        Serial.println("   Esperado: " + String(calculatedChecksum));
        Serial.println("   Recebido: " + String(message.checksum));
        return false;
    }
    
    // Verificar tamanho dos dados
    if (message.dataSize > sizeof(message.data)) {
        Serial.println("❌ Tamanho de dados inválido: " + String(message.dataSize));
        return false;
    }
    
    // Anti-replay local — não validar millis() cross-device (ESPNowController trata)
    if (message.dataSize > sizeof(message.data)) {
        Serial.println("❌ Tamanho de dados inválido: " + String(message.dataSize));
        return false;
    }
    
    if (message.type > MessageType::PERSISTENT_STATE_SYNC) {
        Serial.println("❌ Tipo de mensagem inválido: " + String((int)message.type));
        return false;
    }
    
    return true;
}

void ESPNowBridge::updateRemoteDevice(const uint8_t* mac, const String& name, const String& deviceType, bool operational) {
    for (auto& device : remoteDevices) {
        if (memcmp(device.mac, mac, 6) == 0) {
            device.online = true;
            device.lastSeen = millis();
            device.operational = operational;
            
            if (!name.isEmpty()) device.name = name;
            if (!deviceType.isEmpty()) device.deviceType = deviceType;
            return;
        }
    }
    
    // Se não encontrou, adicionar automaticamente
    addRemoteDevice(mac, name);
}

void ESPNowBridge::cleanupOfflineDevices() {
    unsigned long currentTime = millis();
    const unsigned long offlineTimeout = 120000;  // 2 minutos
    
    for (auto& device : remoteDevices) {
        if (currentTime - device.lastSeen > offlineTimeout) {
            if (device.online) {
                Serial.println("🔴 Dispositivo offline: " + macToString(device.mac));
                device.online = false;
            }
        }
    }
}

// ===== CALLBACKS ESTÁTICOS (COMPATIBILIDADE) =====

void ESPNowBridge::onDataReceived(const uint8_t* mac, const uint8_t* incomingData, int len) {
    if (!instance) return;
    
    if (len != sizeof(ESPNowMessage)) {
        Serial.println("❌ Tamanho de mensagem ESP-NOW inválido: " + String(len));
        return;
    }
    
    ESPNowMessage message;
    memcpy(&message, incomingData, sizeof(ESPNowMessage));
    
    instance->messagesReceived++;
    
    // FASE 2: Se ESPNowController está ativo, deixar ele processar
    if (instance->espNowController) {
        // ESPNowController já processa via seu próprio callback
        return;
    }
    
    // Processar via método legacy
    instance->processReceivedMessage(message, mac);
}

void ESPNowBridge::onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (!instance) return;
    
    if (status != ESP_NOW_SEND_SUCCESS) {
        instance->messagesLost++;
        Serial.println("❌ Falha ao enviar para: " + macToString(mac_addr));
    }
}

bool ESPNowBridge::initiateHandshake(const uint8_t* targetMac) {
    if (!espNowController) return false;
    
    return espNowController->initiateHandshake(targetMac);
}

bool ESPNowBridge::requestConnectivityCheck(const uint8_t* targetMac) {
    if (!espNowController) return false;
    
    return espNowController->requestConnectivityCheck(targetMac);
}

// ===== CALLBACKS JÁ IMPLEMENTADOS ANTERIORMENTE =====
// As funções callback já estão implementadas nas linhas 285-399
// Esta seção duplicada foi comentada para evitar erro de redefinição
//
// NOTA: A implementação nas linhas 285-399 é mais completa:
// - Usa singleton pattern (if (!instance) return)
// - Tem logs detalhados
// - onWiFiCredentialsReceived() conecta ao WiFi (para Slaves)
// 
// Esta implementação duplicada tinha diferenças:
// - Sem singleton pattern
// - Sem logs
// - onWiFiCredentialsReceived() apenas ignora (para Master)
//
// Para manter compatibilidade Master/Slave, mantive a implementação
// completa que detecta automaticamente o contexto.
