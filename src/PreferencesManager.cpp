#include "PreferencesManager.h"

// Instância estática
Preferences PreferencesManager::preferences;
bool PreferencesManager::initialized = false;
bool PreferencesManager::useNVS = true;  // Usar NVS por padrão

bool PreferencesManager::begin() {
    if (initialized) {
        return true;
    }
    
    DEBUG_PRINTLN("🔧 Inicializando PreferencesManager...");
    
    // Usar Preferences (mais simples e confiável)
    if (preferences.begin(PREFERENCES_NAMESPACE, false)) {
        useNVS = false;
        DEBUG_PRINTLN("✅ Preferences inicializado com sucesso");
    } else {
        DEBUG_PRINTLN("❌ Falha ao inicializar Preferences");
        return false;
    }
    
    initialized = true;
    
    // Verificar versão da configuração
    uint32_t configVersion = 0;
    if (!getConfigVersion(configVersion)) {
        // Primeira inicialização
        saveConfigVersion(CONFIG_VERSION);
        DEBUG_PRINTLN("📝 Configuração inicial criada - Versão: " + String(CONFIG_VERSION));
    } else if (configVersion < CONFIG_VERSION) {
        // Migrar configuração
        DEBUG_PRINTLN("🔄 Migrando configuração da versão " + String(configVersion) + " para " + String(CONFIG_VERSION));
        migrateConfig(configVersion, CONFIG_VERSION);
    }
    
    DEBUG_PRINTLN("✅ PreferencesManager inicializado");
    return true;
}

void PreferencesManager::end() {
    if (initialized) {
        if (useNVS) {
            // NVS não precisa de end() explícito
        } else {
            preferences.end();
        }
        initialized = false;
        DEBUG_PRINTLN("📁 PreferencesManager finalizado");
    }
}

bool PreferencesManager::initNVS() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition foi truncada e precisa ser erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return (ret == ESP_OK);
}

bool PreferencesManager::ensureInitialized() {
    if (!initialized) {
        return begin();
    }
    return true;
}

bool PreferencesManager::validateKey(const String& key) {
    if (key.length() == 0 || key.length() > 15) {  // NVS key limit
        return false;
    }
    
    // Verificar caracteres válidos
    for (size_t i = 0; i < key.length(); i++) {
        char c = key.charAt(i);
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    
    return true;
}

bool PreferencesManager::validateValue(const String& value) {
    return value.length() <= 4000;  // Limite razoável para valores
}

// ===== CREDENCIAIS WIFI =====

bool PreferencesManager::saveWiFiCredentials(const String& ssid, const String& password, uint8_t channel) {
    if (!ensureInitialized()) return false;
    
    if (!validateKey("wifi_ssid") || !validateKey("wifi_pass") || !validateKey("wifi_chan")) {
        return false;
    }
    
    if (!validateValue(ssid) || !validateValue(password)) {
        return false;
    }
    
    // Usar Preferences (mais simples)
    bool success = true;
    success &= preferences.putString("wifi_ssid", ssid);
    success &= preferences.putString("wifi_pass", password);
    success &= preferences.putUChar("wifi_chan", channel);
    return success;
}

bool PreferencesManager::loadWiFiCredentials(String& ssid, String& password, uint8_t& channel) {
    if (!ensureInitialized()) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) return false;
        
        size_t ssid_len = 32;
        size_t pass_len = 64;
        char ssid_buf[32];
        char pass_buf[64];
        
        esp_err_t ssid_err = nvs_get_str(nvs_handle, "wifi_ssid", ssid_buf, &ssid_len);
        esp_err_t pass_err = nvs_get_str(nvs_handle, "wifi_pass", pass_buf, &pass_len);
        esp_err_t chan_err = nvs_get_u8(nvs_handle, "wifi_chan", &channel);
        
        nvs_close(nvs_handle);
        
        if (ssid_err == ESP_OK && pass_err == ESP_OK && chan_err == ESP_OK) {
            ssid = String(ssid_buf);
            password = String(pass_buf);
            return true;
        }
        return false;
    } else {
        ssid = preferences.getString("wifi_ssid", "");
        password = preferences.getString("wifi_pass", "");
        channel = preferences.getUChar("wifi_chan", 1);
        return !ssid.isEmpty() && !password.isEmpty();
    }
}

bool PreferencesManager::clearWiFiCredentials() {
    if (!ensureInitialized()) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) return false;
        
        nvs_erase_key(nvs_handle, "wifi_ssid");
        nvs_erase_key(nvs_handle, "wifi_pass");
        nvs_erase_key(nvs_handle, "wifi_chan");
        
        esp_err_t commit_err = nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        return (commit_err == ESP_OK);
    } else {
        bool success = true;
        success &= preferences.remove("wifi_ssid");
        success &= preferences.remove("wifi_pass");
        success &= preferences.remove("wifi_chan");
        return success;
    }
}

// ===== CONFIGURAÇÕES DE RELÉS =====

bool PreferencesManager::saveRelayConfig(int relayNumber, const String& name, bool enabled, int defaultDuration) {
    if (!ensureInitialized()) return false;
    if (relayNumber < 0 || relayNumber >= NUM_RELAYS) return false;
    
    char keyName[32], keyEnabled[32], keyDuration[32];
    snprintf(keyName, sizeof(keyName), "relay_%d_name", relayNumber);
    snprintf(keyEnabled, sizeof(keyEnabled), "relay_%d_enabled", relayNumber);
    snprintf(keyDuration, sizeof(keyDuration), "relay_%d_duration", relayNumber);
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) return false;
        
        nvs_set_str(nvs_handle, keyName, name.c_str());
        nvs_set_u8(nvs_handle, keyEnabled, enabled ? 1 : 0);
        nvs_set_i32(nvs_handle, keyDuration, defaultDuration);
        
        esp_err_t commit_err = nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        return (commit_err == ESP_OK);
    } else {
        bool success = true;
        success &= preferences.putString(keyName, name);
        success &= preferences.putBool(keyEnabled, enabled);
        success &= preferences.putInt(keyDuration, defaultDuration);
        return success;
    }
}

bool PreferencesManager::loadRelayConfig(int relayNumber, String& name, bool& enabled, int& defaultDuration) {
    if (!ensureInitialized()) return false;
    if (relayNumber < 0 || relayNumber >= NUM_RELAYS) return false;
    
    char keyName[32], keyEnabled[32], keyDuration[32];
    snprintf(keyName, sizeof(keyName), "relay_%d_name", relayNumber);
    snprintf(keyEnabled, sizeof(keyEnabled), "relay_%d_enabled", relayNumber);
    snprintf(keyDuration, sizeof(keyDuration), "relay_%d_duration", relayNumber);
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) return false;
        
        size_t name_len = 16;
        char name_buf[16];
        uint8_t enabled_val;
        int32_t duration_val;
        
        esp_err_t name_err = nvs_get_str(nvs_handle, keyName, name_buf, &name_len);
        esp_err_t enabled_err = nvs_get_u8(nvs_handle, keyEnabled, &enabled_val);
        esp_err_t duration_err = nvs_get_i32(nvs_handle, keyDuration, &duration_val);
        
        nvs_close(nvs_handle);
        
        if (name_err == ESP_OK && enabled_err == ESP_OK && duration_err == ESP_OK) {
            name = String(name_buf);
            enabled = (enabled_val == 1);
            defaultDuration = duration_val;
            return true;
        }
        return false;
    } else {
        name = preferences.getString(keyName, "Relé " + String(relayNumber + 1));
        enabled = preferences.getBool(keyEnabled, true);
        defaultDuration = preferences.getInt(keyDuration, 0);
        return true;
    }
}

bool PreferencesManager::saveAllRelayConfigs(const String relayNames[], const bool relayEnabled[], const int relayDurations[]) {
    if (!ensureInitialized()) return false;
    
    bool success = true;
    for (int i = 0; i < NUM_RELAYS; i++) {
        success &= saveRelayConfig(i, relayNames[i], relayEnabled[i], relayDurations[i]);
    }
    return success;
}

bool PreferencesManager::loadAllRelayConfigs(String relayNames[], bool relayEnabled[], int relayDurations[]) {
    if (!ensureInitialized()) return false;
    
    bool success = true;
    for (int i = 0; i < NUM_RELAYS; i++) {
        success &= loadRelayConfig(i, relayNames[i], relayEnabled[i], relayDurations[i]);
    }
    return success;
}

// ===== CONFIGURAÇÕES DE DISPOSITIVO =====

bool PreferencesManager::saveDeviceConfig(const String& deviceName, const String& deviceType, const String& deviceMode) {
    if (!ensureInitialized()) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) return false;
        
        nvs_set_str(nvs_handle, "device_name", deviceName.c_str());
        nvs_set_str(nvs_handle, "device_type", deviceType.c_str());
        nvs_set_str(nvs_handle, "device_mode", deviceMode.c_str());
        
        esp_err_t commit_err = nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        return (commit_err == ESP_OK);
    } else {
        bool success = true;
        success &= preferences.putString("device_name", deviceName);
        success &= preferences.putString("device_type", deviceType);
        success &= preferences.putString("device_mode", deviceMode);
        return success;
    }
}

bool PreferencesManager::loadDeviceConfig(String& deviceName, String& deviceType, String& deviceMode) {
    if (!ensureInitialized()) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) return false;
        
        size_t name_len = 32;
        size_t type_len = 16;
        size_t mode_len = 16;
        char name_buf[32];
        char type_buf[16];
        char mode_buf[16];
        
        esp_err_t name_err = nvs_get_str(nvs_handle, "device_name", name_buf, &name_len);
        esp_err_t type_err = nvs_get_str(nvs_handle, "device_type", type_buf, &type_len);
        esp_err_t mode_err = nvs_get_str(nvs_handle, "device_mode", mode_buf, &mode_len);
        
        nvs_close(nvs_handle);
        
        if (name_err == ESP_OK && type_err == ESP_OK && mode_err == ESP_OK) {
            deviceName = String(name_buf);
            deviceType = String(type_buf);
            deviceMode = String(mode_buf);
            return true;
        }
        return false;
    } else {
        deviceName = preferences.getString("device_name", DEVICE_NAME_PREFIX);
        deviceType = preferences.getString("device_type", "Unknown");
        deviceMode = preferences.getString("device_mode", "Standalone");
        return true;
    }
}

// ===== CONFIGURAÇÕES GERAIS =====

bool PreferencesManager::saveConfig(const String& key, const String& value) {
    if (!ensureInitialized()) return false;
    if (!validateKey(key) || !validateValue(value)) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) return false;
        
        nvs_set_str(nvs_handle, key.c_str(), value.c_str());
        
        esp_err_t commit_err = nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        return (commit_err == ESP_OK);
    } else {
        return preferences.putString(key.c_str(), value);
    }
}

bool PreferencesManager::loadConfig(const String& key, String& value) {
    if (!ensureInitialized()) return false;
    if (!validateKey(key)) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) return false;
        
        size_t value_len = 4000;
        char value_buf[4000];
        
        esp_err_t get_err = nvs_get_str(nvs_handle, key.c_str(), value_buf, &value_len);
        nvs_close(nvs_handle);
        
        if (get_err == ESP_OK) {
            value = String(value_buf);
            return true;
        }
        return false;
    } else {
        value = preferences.getString(key.c_str(), "");
        return !value.isEmpty();
    }
}

bool PreferencesManager::saveConfigInt(const String& key, int32_t value) {
    if (!ensureInitialized()) return false;
    if (!validateKey(key)) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) return false;
        
        nvs_set_i32(nvs_handle, key.c_str(), value);
        
        esp_err_t commit_err = nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        return (commit_err == ESP_OK);
    } else {
        return preferences.putInt(key.c_str(), value);
    }
}

bool PreferencesManager::loadConfigInt(const String& key, int32_t& value) {
    if (!ensureInitialized()) return false;
    if (!validateKey(key)) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) return false;
        
        esp_err_t get_err = nvs_get_i32(nvs_handle, key.c_str(), &value);
        nvs_close(nvs_handle);
        
        return (get_err == ESP_OK);
    } else {
        value = preferences.getInt(key.c_str(), 0);
        return true;
    }
}

bool PreferencesManager::saveConfigFloat(const String& key, float value) {
    if (!ensureInitialized()) return false;
    if (!validateKey(key)) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) return false;
        
        nvs_set_blob(nvs_handle, key.c_str(), &value, sizeof(float));
        
        esp_err_t commit_err = nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        return (commit_err == ESP_OK);
    } else {
        return preferences.putFloat(key.c_str(), value);
    }
}

bool PreferencesManager::loadConfigFloat(const String& key, float& value) {
    if (!ensureInitialized()) return false;
    if (!validateKey(key)) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) return false;
        
        size_t blob_len = sizeof(float);
        esp_err_t get_err = nvs_get_blob(nvs_handle, key.c_str(), &value, &blob_len);
        nvs_close(nvs_handle);
        
        return (get_err == ESP_OK && blob_len == sizeof(float));
    } else {
        value = preferences.getFloat(key.c_str(), 0.0);
        return true;
    }
}

// ===== UTILITÁRIOS =====

bool PreferencesManager::configExists(const String& key) {
    if (!ensureInitialized()) return false;
    if (!validateKey(key)) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) return false;
        
        size_t required_size = 0;
        esp_err_t get_err = nvs_get_str(nvs_handle, key.c_str(), NULL, &required_size);
        nvs_close(nvs_handle);
        
        return (get_err == ESP_OK);
    } else {
        return preferences.isKey(key.c_str());
    }
}

bool PreferencesManager::removeConfig(const String& key) {
    if (!ensureInitialized()) return false;
    if (!validateKey(key)) return false;
    
    if (useNVS) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(PREFERENCES_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) return false;
        
        nvs_erase_key(nvs_handle, key.c_str());
        
        esp_err_t commit_err = nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        return (commit_err == ESP_OK);
    } else {
        return preferences.remove(key.c_str());
    }
}

bool PreferencesManager::getConfigVersion(uint32_t& version) {
    return loadConfigInt("config_version", (int32_t&)version);
}

bool PreferencesManager::saveConfigVersion(uint32_t version) {
    return saveConfigInt("config_version", (int32_t)version);
}

bool PreferencesManager::migrateConfig(uint32_t fromVersion, uint32_t toVersion) {
    DEBUG_PRINTLN("🔄 Iniciando migração de configuração...");
    
    // Implementar migrações específicas conforme necessário
    switch (fromVersion) {
        case 1:
            // Migrar da versão 1 para 2
            DEBUG_PRINTLN("📝 Migrando configurações da versão 1 para 2");
            // Adicionar novas configurações padrão se necessário
            break;
            
        default:
            DEBUG_PRINTLN("⚠️ Versão de origem não suportada: " + String(fromVersion));
            break;
    }
    
    // Atualizar versão
    saveConfigVersion(toVersion);
    DEBUG_PRINTLN("✅ Migração concluída para versão " + String(toVersion));
    
    return true;
}
