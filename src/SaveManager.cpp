#include "SaveManager.h"
#include "Config.h"

// Definir macros de debug se não estiverem definidas
#ifndef DEBUG_PRINTLN
    #define DEBUG_PRINTLN(x) Serial.println(x)
#endif

#ifndef DEBUG_PRINTF
    #define DEBUG_PRINTF(x, ...) Serial.printf(x, __VA_ARGS__)
#endif

SaveManager::SaveManager() : initialized(false) {
}

bool SaveManager::begin() {
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        DEBUG_PRINTLN("❌ Erro ao inicializar Preferences");
        return false;
    }
    
    initialized = true;
    DEBUG_PRINTLN("✅ SaveManager inicializado");
    return true;
}

void SaveManager::end() {
    if (initialized) {
        preferences.end();
        initialized = false;
        DEBUG_PRINTLN("📁 SaveManager finalizado");
    }
}

// ===== CONFIGURAÇÕES DE RELÉS =====

bool SaveManager::saveRelayName(int relayNumber, const String& name) {
    if (!initialized || relayNumber < 0 || relayNumber >= 8) {
        return false;
    }
    
    String key = "relay_" + String(relayNumber) + "_name";
    return preferences.putString(key.c_str(), name) > 0;
}

String SaveManager::loadRelayName(int relayNumber) {
    if (!initialized || relayNumber < 0 || relayNumber >= 8) {
        return "";
    }
    
    String key = "relay_" + String(relayNumber) + "_name";
    return preferences.getString(key.c_str(), "");
}

bool SaveManager::saveAllRelayNames(const String relayNames[8]) {
    if (!initialized) return false;
    
    bool success = true;
    for (int i = 0; i < 8; i++) {
        if (!saveRelayName(i, relayNames[i])) {
            success = false;
        }
    }
    
    // Marcar que relés foram configurados
    preferences.putBool("relays_configured", true);
    
    return success;
}

bool SaveManager::loadAllRelayNames(String relayNames[8]) {
    if (!initialized) return false;
    
    // Verificar se relés foram configurados
    if (!preferences.getBool("relays_configured", false)) {
        return false;
    }
    
    for (int i = 0; i < 8; i++) {
        relayNames[i] = loadRelayName(i);
    }
    
    return true;
}

// ===== CONFIGURAÇÕES DE PEERS =====

bool SaveManager::saveKnownPeers(const std::vector<PeerInfo>& peers) {
    if (!initialized) return false;
    
    // Salvar número de peers
    preferences.putInt("peers_count", peers.size());
    
    // Salvar cada peer
    for (size_t i = 0; i < peers.size(); i++) {
        String prefix = "peer_" + String(i) + "_";
        
        preferences.putString((prefix + "mac").c_str(), macToString(peers[i].macAddress));
        preferences.putString((prefix + "name").c_str(), peers[i].deviceName);
        preferences.putString((prefix + "type").c_str(), peers[i].deviceType);
        preferences.putBool((prefix + "online").c_str(), peers[i].online);
        preferences.putULong((prefix + "lastSeen").c_str(), peers[i].lastSeen);
        preferences.putInt((prefix + "rssi").c_str(), peers[i].rssi);
    }
    
    return true;
}

bool SaveManager::loadKnownPeers(std::vector<PeerInfo>& peers) {
    if (!initialized) return false;
    
    peers.clear();
    
    int count = preferences.getInt("peers_count", 0);
    if (count <= 0) return true; // Nenhum peer salvo
    
    for (int i = 0; i < count; i++) {
        String prefix = "peer_" + String(i) + "_";
        
        PeerInfo peer;
        String macStr = preferences.getString((prefix + "mac").c_str(), "");
        
        if (macStr.length() > 0 && stringToMac(macStr, peer.macAddress)) {
            peer.deviceName = preferences.getString((prefix + "name").c_str(), "");
            peer.deviceType = preferences.getString((prefix + "type").c_str(), "");
            peer.online = preferences.getBool((prefix + "online").c_str(), false);
            peer.lastSeen = preferences.getULong((prefix + "lastSeen").c_str(), 0);
            peer.rssi = preferences.getInt((prefix + "rssi").c_str(), -50);
            
            peers.push_back(peer);
        }
    }
    
    return true;
}

bool SaveManager::addPeer(const uint8_t* macAddress, const String& deviceName, const String& deviceType) {
    if (!initialized) return false;
    
    // Carregar peers existentes
    std::vector<PeerInfo> peers;
    loadKnownPeers(peers);
    
    // Verificar se já existe
    for (auto& peer : peers) {
        if (memcmp(peer.macAddress, macAddress, 6) == 0) {
            // Atualizar informações existentes
            peer.deviceName = deviceName;
            peer.deviceType = deviceType;
            peer.online = true;
            peer.lastSeen = millis();
            return saveKnownPeers(peers);
        }
    }
    
    // Adicionar novo peer
    PeerInfo newPeer;
    memcpy(newPeer.macAddress, macAddress, 6);
    newPeer.deviceName = deviceName;
    newPeer.deviceType = deviceType;
    newPeer.online = true;
    newPeer.lastSeen = millis();
    newPeer.rssi = -50;
    
    peers.push_back(newPeer);
    return saveKnownPeers(peers);
}

bool SaveManager::removePeer(const uint8_t* macAddress) {
    if (!initialized) return false;
    
    // Carregar peers existentes
    std::vector<PeerInfo> peers;
    loadKnownPeers(peers);
    
    // Remover peer
    for (auto it = peers.begin(); it != peers.end(); ++it) {
        if (memcmp(it->macAddress, macAddress, 6) == 0) {
            peers.erase(it);
            return saveKnownPeers(peers);
        }
    }
    
    return false; // Peer não encontrado
}

// ===== CONFIGURAÇÕES DO SISTEMA =====

bool SaveManager::saveSystemConfig(const String& deviceName, int channel, int numRelays) {
    if (!initialized) return false;
    
    preferences.putString("device_name", deviceName);
    preferences.putInt("wifi_channel", channel);
    preferences.putInt("num_relays", numRelays);
    preferences.putInt("config_version", CONFIG_VERSION);
    
    return true;
}

bool SaveManager::loadSystemConfig(String& deviceName, int& channel, int& numRelays) {
    if (!initialized) return false;
    
    deviceName = preferences.getString("device_name", "");
    channel = preferences.getInt("wifi_channel", 1);
    numRelays = preferences.getInt("num_relays", 8);
    
    int version = preferences.getInt("config_version", 0);
    if (version != CONFIG_VERSION) {
        DEBUG_PRINTLN("⚠️ Versão de configuração diferente");
        return false;
    }
    
    return !deviceName.isEmpty();
}

// ===== UTILITÁRIOS =====

bool SaveManager::clearAll() {
    if (!initialized) return false;
    
    return preferences.clear();
}

bool SaveManager::hasConfig() {
    if (!initialized) return false;
    
    return preferences.getInt("config_version", 0) > 0;
}

String SaveManager::getStats() {
    if (!initialized) return "{}";
    
    DynamicJsonDocument doc(512);
    
    doc["initialized"] = initialized;
    doc["hasConfig"] = hasConfig();
    doc["peersCount"] = preferences.getInt("peers_count", 0);
    doc["relaysConfigured"] = preferences.getBool("relays_configured", false);
    doc["configVersion"] = preferences.getInt("config_version", 0);
    
    String result;
    serializeJson(doc, result);
    return result;
}

// ===== MÉTODOS PRIVADOS =====

String SaveManager::macToString(const uint8_t* mac) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

bool SaveManager::stringToMac(const String& macStr, uint8_t* mac) {
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
