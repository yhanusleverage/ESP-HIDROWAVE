/**
 * @file MultiChannelDiscovery.cpp
 * @brief Implementação do Discovery Automático Multi-Canal
 */

#include "MultiChannelDiscovery.h"

// Instância estática para callback
MultiChannelDiscovery* MultiChannelDiscovery::instance = nullptr;

// ===== CONSTRUTOR / DESTRUTOR =====

MultiChannelDiscovery::MultiChannelDiscovery() 
    : initialized(false), currentChannel(1), masterFound(false),
      abortFlag(false), masterFoundCallback(nullptr), progressCallback(nullptr) {
    
    memset(masterMac, 0, 6);
    instance = this;
}

MultiChannelDiscovery::~MultiChannelDiscovery() {
    end();
}

// ===== INICIALIZAÇÃO =====

bool MultiChannelDiscovery::begin() {
    if (initialized) {
        Serial.println("⚠️ MultiChannelDiscovery: Já inicializado");
        return true;
    }
    
    Serial.println("\n🔍 === INICIALIZANDO MULTI-CHANNEL DISCOVERY ===");
    Serial.println("================================================");
    
    // Configurar WiFi em modo Station
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    
    // Inicializar ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ Erro ao inicializar ESP-NOW");
        return false;
    }
    
    // Registrar callback de recebimento
    esp_now_register_recv_cb(onDataReceivedStatic);
    
    // Carregar cache
    if (MCD_CACHE_ENABLED) {
        loadCache();
        Serial.println("✅ Cache carregado: Canal " + String(cache.lastChannel));
    }
    
    // Resetar estatísticas se primeira vez
    if (stats.totalAttempts == 0) {
        resetStats();
    }
    
    initialized = true;
    Serial.println("✅ MultiChannelDiscovery inicializado");
    Serial.println("================================================\n");
    
    return true;
}

void MultiChannelDiscovery::end() {
    if (!initialized) return;
    
    // Salvar cache antes de finalizar
    if (MCD_CACHE_ENABLED) {
        saveCache();
    }
    
    // Desinicializar ESP-NOW
    esp_now_deinit();
    
    initialized = false;
    Serial.println("🔍 MultiChannelDiscovery finalizado");
}

// ===== DISCOVERY PRINCIPAL =====

DiscoveryResult MultiChannelDiscovery::discoverMaster() {
    if (!initialized) {
        Serial.println("❌ Discovery: Sistema não inicializado");
        return DiscoveryResult::ERROR_ESP_NOW;
    }
    
    Serial.println("\n🔍 === INICIANDO DISCOVERY MULTI-CANAL ===");
    Serial.println("==========================================");
    
    unsigned long startTime = millis();
    masterFound = false;
    abortFlag = false;
    stats.totalAttempts++;
    
    // ===== FASE 0: CANAL DO CACHE =====
    if (MCD_CACHE_ENABLED && cache.lastChannel > 0 && cache.successRate > 50) {
        Serial.println("📦 Fase 0: Tentando canal do cache");
        Serial.print("   Canal " + String(cache.lastChannel) + " (taxa: " + String(cache.successRate) + "%): ");
        
        if (tryChannel(cache.lastChannel, MCD_TIMEOUT_PER_CHANNEL)) {
            Serial.println("✅ MASTER ENCONTRADO!");
            unsigned long elapsedTime = millis() - startTime;
            updateStats(true, elapsedTime, cache.lastChannel);
            return DiscoveryResult::SUCCESS;
        }
        Serial.println("⚪ Sem resposta");
    }
    
    // ===== FASE 1: CANAIS PRIORITÁRIOS =====
    Serial.println("\n📡 Fase 1: Canais prioritários (1, 6, 11)");
    
    for (uint8_t i = 0; i < MCD_PRIORITY_COUNT; i++) {
        if (abortFlag) {
            Serial.println("⚠️ Discovery abortado");
            return DiscoveryResult::ABORTED;
        }
        
        uint8_t channel = MCD_PRIORITY_CHANNELS[i];
        
        // Pular se já testado no cache
        if (MCD_CACHE_ENABLED && channel == cache.lastChannel) {
            continue;
        }
        
        Serial.print("   Canal " + String(channel) + ": ");
        
        // Callback de progresso
        if (progressCallback) {
            progressCallback(channel, MCD_MAX_CHANNEL);
        }
        
        if (tryChannel(channel, MCD_TIMEOUT_PER_CHANNEL)) {
            Serial.println("✅ MASTER ENCONTRADO!");
            unsigned long elapsedTime = millis() - startTime;
            updateStats(true, elapsedTime, channel);
            
            // Salvar no cache
            cache.lastChannel = channel;
            cache.usageCount++;
            cache.successRate = min(100, cache.successRate + 10);
            saveCache();
            
            return DiscoveryResult::SUCCESS;
        }
        
        Serial.println("⚪ Sem resposta");
    }
    
    // ===== FASE 2: VARREDURA COMPLETA =====
    Serial.println("\n📡 Fase 2: Varredura completa (2-5, 7-10, 12-13)");
    
    for (uint8_t channel = MCD_MIN_CHANNEL; channel <= MCD_MAX_CHANNEL; channel++) {
        if (abortFlag) {
            Serial.println("⚠️ Discovery abortado");
            return DiscoveryResult::ABORTED;
        }
        
        // Pular canais já testados
        bool skip = false;
        if (MCD_CACHE_ENABLED && channel == cache.lastChannel) skip = true;
        for (uint8_t i = 0; i < MCD_PRIORITY_COUNT; i++) {
            if (channel == MCD_PRIORITY_CHANNELS[i]) skip = true;
        }
        if (skip) continue;
        
        Serial.print("   Canal " + String(channel) + ": ");
        
        // Callback de progresso
        if (progressCallback) {
            progressCallback(channel, MCD_MAX_CHANNEL);
        }
        
        if (tryChannel(channel, MCD_TIMEOUT_PER_CHANNEL)) {
            Serial.println("✅ MASTER ENCONTRADO!");
            unsigned long elapsedTime = millis() - startTime;
            updateStats(true, elapsedTime, channel);
            
            // Salvar no cache
            cache.lastChannel = channel;
            cache.usageCount++;
            cache.successRate = min(100, cache.successRate + 5);
            saveCache();
            
            return DiscoveryResult::SUCCESS;
        }
        
        Serial.println("⚪ Sem resposta");
    }
    
    // ===== RESULTADO: NÃO ENCONTRADO =====
    Serial.println("\n❌ === MASTER NÃO ENCONTRADO ===");
    Serial.println("⚠️ Possíveis causas:");
    Serial.println("   - MASTER não está ligado");
    Serial.println("   - MASTER fora de alcance ESP-NOW (>100m)");
    Serial.println("   - Interferência no sinal 2.4GHz");
    Serial.println("   - MASTER não enviou resposta");
    Serial.println("=====================================\n");
    
    unsigned long elapsedTime = millis() - startTime;
    updateStats(false, elapsedTime, 0);
    
    // Degradar cache se falhou
    if (MCD_CACHE_ENABLED && cache.successRate > 0) {
        cache.successRate = max(0, cache.successRate - 20);
        saveCache();
    }
    
    return DiscoveryResult::TIMEOUT;
}

bool MultiChannelDiscovery::tryChannel(uint8_t channel, uint32_t timeout) {
    if (!initialized) return false;
    
    // Configurar canal
    if (!setChannel(channel)) {
        return false;
    }
    
    currentChannel = channel;
    
    // Tentar múltiplas vezes se necessário
    for (uint8_t attempt = 0; attempt < MCD_MAX_RETRY_ATTEMPTS; attempt++) {
        // Enviar discovery
        if (!sendDiscoveryBroadcast()) {
            delay(50);
            continue;
        }
        
        // Aguardar resposta
        if (waitForMasterResponse(timeout)) {
            return true;
        }
        
        // Pequeno delay entre tentativas
        if (attempt < MCD_MAX_RETRY_ATTEMPTS - 1) {
            delay(100);
        }
    }
    
    return false;
}

DiscoveryResult MultiChannelDiscovery::rediscoverMaster(bool quickScan) {
    Serial.println("\n🔄 === RE-DISCOVERY ===");
    
    if (quickScan) {
        Serial.println("⚡ Modo rápido: Canal anterior + prioritários");
        
        // Tentar canal anterior primeiro
        if (cache.lastChannel > 0) {
            Serial.print("   Canal " + String(cache.lastChannel) + " (anterior): ");
            if (tryChannel(cache.lastChannel, MCD_TIMEOUT_PER_CHANNEL)) {
                Serial.println("✅ RECONECTADO!");
                return DiscoveryResult::SUCCESS;
            }
            Serial.println("⚪ Sem resposta");
        }
        
        // Tentar canais prioritários
        for (uint8_t i = 0; i < MCD_PRIORITY_COUNT; i++) {
            uint8_t channel = MCD_PRIORITY_CHANNELS[i];
            if (channel == cache.lastChannel) continue;
            
            Serial.print("   Canal " + String(channel) + ": ");
            if (tryChannel(channel, MCD_TIMEOUT_PER_CHANNEL)) {
                Serial.println("✅ RECONECTADO!");
                cache.lastChannel = channel;
                saveCache();
                return DiscoveryResult::SUCCESS;
            }
            Serial.println("⚪ Sem resposta");
        }
        
        Serial.println("⚠️ Quick scan falhou - tentando varredura completa...");
    }
    
    // Varredura completa se quick scan falhou
    return discoverMaster();
}

// ===== CALLBACKS =====

void MultiChannelDiscovery::setMasterFoundCallback(void (*callback)(uint8_t channel, const uint8_t* masterMac)) {
    masterFoundCallback = callback;
}

void MultiChannelDiscovery::setProgressCallback(void (*callback)(uint8_t channel, uint8_t totalChannels)) {
    progressCallback = callback;
}

// ===== GETTERS =====

bool MultiChannelDiscovery::getMasterMac(uint8_t* mac) const {
    if (!masterFound) return false;
    memcpy(mac, masterMac, 6);
    return true;
}

// ===== CONTROLE =====

void MultiChannelDiscovery::resetStats() {
    stats = DiscoveryStats();
    Serial.println("📊 Estatísticas resetadas");
}

void MultiChannelDiscovery::clearCache() {
    cache = ChannelCache();
    if (MCD_CACHE_ENABLED) {
        prefs.begin(MCD_NVS_NAMESPACE, false);
        prefs.clear();
        prefs.end();
    }
    Serial.println("🗑️ Cache limpo");
}

void MultiChannelDiscovery::saveCache() {
    saveCacheInternal();
}

// ===== UTILITÁRIOS =====

String MultiChannelDiscovery::resultToString(DiscoveryResult result) {
    switch (result) {
        case DiscoveryResult::SUCCESS: return "SUCCESS";
        case DiscoveryResult::TIMEOUT: return "TIMEOUT";
        case DiscoveryResult::ERROR_ESP_NOW: return "ERROR_ESP_NOW";
        case DiscoveryResult::ERROR_WIFI: return "ERROR_WIFI";
        case DiscoveryResult::ABORTED: return "ABORTED";
        default: return "UNKNOWN";
    }
}

void MultiChannelDiscovery::printStats() const {
    Serial.println("\n📊 === ESTATÍSTICAS DISCOVERY ===");
    Serial.println("Total de tentativas: " + String(stats.totalAttempts));
    Serial.println("Sucessos: " + String(stats.successCount));
    Serial.println("Falhas: " + String(stats.failureCount));
    
    if (stats.totalAttempts > 0) {
        float successRate = (stats.successCount * 100.0) / stats.totalAttempts;
        Serial.println("Taxa de sucesso: " + String(successRate, 1) + "%");
    }
    
    Serial.println("Tempo médio: " + String(stats.averageTimeMs) + "ms");
    Serial.println("Último canal: " + String(stats.lastChannelFound));
    Serial.println("==================================\n");
}

String MultiChannelDiscovery::getStatsJSON() const {
    String json = "{";
    json += "\"total_attempts\":" + String(stats.totalAttempts) + ",";
    json += "\"success_count\":" + String(stats.successCount) + ",";
    json += "\"failure_count\":" + String(stats.failureCount) + ",";
    json += "\"average_time_ms\":" + String(stats.averageTimeMs) + ",";
    json += "\"last_channel\":" + String(stats.lastChannelFound) + ",";
    json += "\"cache_channel\":" + String(cache.lastChannel) + ",";
    json += "\"cache_success_rate\":" + String(cache.successRate);
    json += "}";
    return json;
}

// ===== MÉTODOS PRIVADOS =====

bool MultiChannelDiscovery::loadCache() {
    if (!MCD_CACHE_ENABLED) return false;
    
    prefs.begin(MCD_NVS_NAMESPACE, true); // read-only
    
    cache.lastChannel = prefs.getUChar("channel", 1);
    cache.lastSuccess = prefs.getUInt("last_success", 0);
    cache.usageCount = prefs.getUInt("usage_count", 0);
    cache.successRate = prefs.getUChar("success_rate", 0);
    
    prefs.end();
    
    return (cache.lastChannel > 0 && cache.lastChannel <= 13);
}

bool MultiChannelDiscovery::saveCacheInternal() {
    if (!MCD_CACHE_ENABLED) return false;
    
    prefs.begin(MCD_NVS_NAMESPACE, false); // read-write
    
    prefs.putUChar("channel", cache.lastChannel);
    prefs.putUInt("last_success", millis());
    prefs.putUInt("usage_count", cache.usageCount);
    prefs.putUChar("success_rate", cache.successRate);
    
    prefs.end();
    
    return true;
}

bool MultiChannelDiscovery::setChannel(uint8_t channel) {
    if (channel < MCD_MIN_CHANNEL || channel > MCD_MAX_CHANNEL) {
        Serial.println("❌ Canal inválido: " + String(channel));
        return false;
    }
    
    // Configurar canal WiFi (necessário para ESP-NOW)
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        Serial.println("❌ Erro ao configurar canal " + String(channel) + ": " + String(err));
        return false;
    }
    
    delay(50); // Estabilizar
    return true;
}

bool MultiChannelDiscovery::sendDiscoveryBroadcast() {
    // Criar mensagem de discovery
    ESPNowMessage msg;
    msg.type = MessageType::BROADCAST;
    
    // Obter MAC local
    esp_wifi_get_mac(WIFI_IF_STA, msg.senderId);
    
    // Target: Broadcast
    memset(msg.targetId, 0xFF, 6);
    
    msg.messageId = millis();
    msg.timestamp = millis();
    msg.dataSize = 0;
    
    // Calcular checksum
    msg.checksum = 0;
    uint8_t* ptr = (uint8_t*)&msg;
    for (size_t i = 0; i < sizeof(ESPNowMessage) - 1; i++) {
        msg.checksum ^= ptr[i];
    }
    
    // Adicionar peer broadcast se necessário
    esp_now_peer_info_t peerInfo = {};
    memset(peerInfo.peer_addr, 0xFF, 6);
    peerInfo.channel = currentChannel;
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    
    // Tentar adicionar (ignora erro se já existe)
    esp_now_add_peer(&peerInfo);
    
    // Enviar
    esp_err_t err = esp_now_send(peerInfo.peer_addr, (uint8_t*)&msg, sizeof(ESPNowMessage));
    
    return (err == ESP_OK);
}

bool MultiChannelDiscovery::waitForMasterResponse(uint32_t timeout) {
    unsigned long start = millis();
    masterFound = false;
    
    while (millis() - start < timeout) {
        if (masterFound) {
            return true;
        }
        delay(10);
    }
    
    return false;
}

void MultiChannelDiscovery::handleReceivedMessage(const uint8_t* mac, const uint8_t* data, int len) {
    if (len != sizeof(ESPNowMessage)) {
        return; // Mensagem com tamanho incorreto
    }
    
    ESPNowMessage* msg = (ESPNowMessage*)data;
    
    // Verificar se é resposta ao discovery
    if (msg->type == MessageType::DEVICE_INFO || 
        msg->type == MessageType::PONG ||
        msg->type == MessageType::ACK) {
        
        // Master encontrado!
        masterFound = true;
        memcpy(masterMac, mac, 6);
        
        // Callback
        if (masterFoundCallback) {
            masterFoundCallback(currentChannel, mac);
        }
        
        if (MCD_DEBUG_ENABLED) {
            Serial.printf("\n✅ Master respondeu: %02X:%02X:%02X:%02X:%02X:%02X\n",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
}

void MultiChannelDiscovery::updateStats(bool success, uint32_t timeMs, uint8_t channelFound) {
    if (success) {
        stats.successCount++;
        stats.lastChannelFound = channelFound;
    } else {
        stats.failureCount++;
    }
    
    // Atualizar tempo médio
    if (stats.averageTimeMs == 0) {
        stats.averageTimeMs = timeMs;
    } else {
        stats.averageTimeMs = (stats.averageTimeMs + timeMs) / 2;
    }
    
    stats.lastAttemptTime = millis();
}

// ===== CALLBACK ESTÁTICO =====

void MultiChannelDiscovery::onDataReceivedStatic(const uint8_t* mac, const uint8_t* data, int len) {
    if (instance) {
        instance->handleReceivedMessage(mac, data, len);
    }
}

