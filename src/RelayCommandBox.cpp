#include "RelayCommandBox.h"
#include <WiFi.h>
#include <nvs_flash.h>
#include <nvs.h>
#include "ESPNowTypes.h"  // Para PersistentRelayStateData

// Tentar incluir Config.h se disponível
#ifndef CONFIG_H
    #include "Config.h"
#endif

// Definir macros de debug se não estiverem definidas
#ifndef DEBUG_PRINTLN
    #define DEBUG_PRINTLN(x) Serial.println(x)
#endif

#ifndef DEBUG_PRINTF
    #define DEBUG_PRINTF(x, ...) Serial.printf(x, __VA_ARGS__)
#endif

RelayCommandBox::RelayCommandBox(uint8_t pcf8574Address, const String& deviceName) 
    : pcf8574(pcf8574Address), i2cAddress(pcf8574Address), deviceName(deviceName), pcfInitialized(false) {
    
    // Inicializar estados dos relés
    for (int i = 0; i < MAX_RELAYS; i++) {
        relayStates[i].isOn = false;
        relayStates[i].startTime = 0;
        relayStates[i].timerSeconds = 0;
        relayStates[i].hasTimer = false;
        relayStates[i].name = "";
    }
    
    // Inicializar nomes padrão
    initializeDefaultNames();
}

bool RelayCommandBox::begin() {
    DEBUG_PRINTLN("🔌 Inicializando RelayCommandBox: " + deviceName);
    DEBUG_PRINTLN("📍 Endereço PCF8574: 0x" + String(i2cAddress, HEX));
    
    // Inicializar PCF8574 (não reinicializar I2C se já estiver inicializado)
    pcfInitialized = pcf8574.begin(false);
    
    if (!pcfInitialized) {
        Serial.println("❌ Erro: PCF8574 não encontrado no endereço 0x" + String(i2cAddress, HEX));
        Serial.println("⚠️ Verifique:");
        Serial.println("   - Conexões I2C (SDA/SCL)");
        Serial.println("   - Alimentação do PCF8574");
        Serial.println("   - Endereço I2C correto");
        return false;
    }
    
    Serial.println("✅ PCF8574 inicializado com sucesso");
    
    // Desligar todos os relés inicialmente
    Serial.println("🔄 Desligando todos os relés...");
    turnOffAllRelays();
    
    Serial.println("✅ RelayCommandBox inicializado: " + deviceName);
    Serial.println("🎯 Relés disponíveis: 0-" + String(MAX_RELAYS - 1));
    
    // 🎯 Carregar estados persistentes ao iniciar
    Serial.println("💾 Carregando estados persistentes de NVS...");
    if (loadPersistentStates()) {
        Serial.println("✅ Estados persistentes carregados e aplicados");
    } else {
        Serial.println("💡 Nenhum estado persistente encontrado - iniciando com estados padrão");
    }
    
    return true;
}

void RelayCommandBox::update() {
    if (!pcfInitialized) return;
    checkTimers();
}

bool RelayCommandBox::setRelay(int relayNumber, bool state) {
    if (!isValidRelayNumber(relayNumber)) {
        Serial.println("❌ Número de relé inválido: " + String(relayNumber));
        return false;
    }
    
    if (!pcfInitialized) {
        Serial.println("❌ PCF8574 não inicializado");
        return false;
    }
    
    // Cancelar timer se existir
    relayStates[relayNumber].hasTimer = false;
    relayStates[relayNumber].timerSeconds = 0;
    
    // Definir novo estado
    relayStates[relayNumber].isOn = state;
    relayStates[relayNumber].startTime = millis();
    
    // Escrever no hardware
    bool success = writeToRelay(relayNumber, state);
    
    if (success) {
        String relayName = relayStates[relayNumber].name.isEmpty() ? 
                          "Relé " + String(relayNumber) : 
                          relayStates[relayNumber].name;
        
        Serial.println("🔌 " + relayName + " " + (state ? "LIGADO" : "DESLIGADO"));
        
        // Chamar callback se definido
        if (stateChangeCallback) {
            stateChangeCallback(relayNumber, state, 0);
        }
    } else {
        Serial.println("❌ Erro ao controlar relé " + String(relayNumber));
    }
    
    return success;
}

bool RelayCommandBox::setRelayWithTimer(int relayNumber, bool state, int seconds) {
    if (!isValidRelayNumber(relayNumber)) {
        Serial.println("❌ Número de relé inválido: " + String(relayNumber));
        return false;
    }
    
    if (!pcfInitialized) {
        Serial.println("❌ PCF8574 não inicializado");
        return false;
    }
    
    if (seconds <= 0) {
        return setRelay(relayNumber, state);
    }
    
    // Validar duração máxima
    if (seconds > DEFAULT_MAX_DURATION) {
        Serial.println("⚠️ Duração limitada a " + String(DEFAULT_MAX_DURATION) + " segundos");
        seconds = DEFAULT_MAX_DURATION;
    }
    
    // Configurar estado com timer
    relayStates[relayNumber].isOn = state;
    relayStates[relayNumber].startTime = millis();
    relayStates[relayNumber].timerSeconds = seconds;
    relayStates[relayNumber].hasTimer = true;
    
    // Escrever no hardware
    bool success = writeToRelay(relayNumber, state);
    
    if (success) {
        String relayName = relayStates[relayNumber].name.isEmpty() ? 
                          "Relé " + String(relayNumber) : 
                          relayStates[relayNumber].name;
        
        Serial.println("⏰ " + relayName + " " + (state ? "LIGADO" : "DESLIGADO") + 
                      " por " + String(seconds) + " segundos");
        
        // Chamar callback se definido
        if (stateChangeCallback) {
            stateChangeCallback(relayNumber, state, seconds);
        }
    } else {
        Serial.println("❌ Erro ao controlar relé " + String(relayNumber));
    }
    
    return success;
}

bool RelayCommandBox::toggleRelay(int relayNumber) {
    if (!isValidRelayNumber(relayNumber)) {
        return false;
    }
    
    bool currentState = getRelayState(relayNumber);
    return setRelay(relayNumber, !currentState);
}

bool RelayCommandBox::processCommand(int relayNumber, String action, int duration) {
    if (!isValidRelayNumber(relayNumber)) {
        Serial.println("❌ Comando inválido - Relé: " + String(relayNumber));
        return false;
    }
    
    action.toLowerCase();
    action.trim();
    
    // Chamar callback de comando se definido
    if (commandCallback) {
        commandCallback(relayNumber, action, duration);
    }
    
    if (action == "on") {
        bool success;
        if (duration > 0) {
            // ⏰ ON COM TIMER - estado temporário
            success = setRelayWithTimer(relayNumber, true, duration);
        } else {
            // 🔒 ON PERMANENTE (on_forever) - estado persistente
            success = setRelay(relayNumber, true);
        }
        // Guardar estados persistentes após mudança
        if (success) {
            savePersistentStates();
        }
        return success;
    } 
    else if (action == "off") {
        bool success = setRelay(relayNumber, false);
        // Guardar estados persistentes após mudança
        if (success) {
            savePersistentStates();
        }
        return success;
    } 
    else if (action == "toggle") {
        return toggleRelay(relayNumber);
    }
    else if (action == "status") {
        // Comando de status - apenas retorna informação
        String relayName = getRelayName(relayNumber);
        bool state = getRelayState(relayNumber);
        int remaining = getRemainingTime(relayNumber);
        
        Serial.println("📊 " + relayName + ": " + (state ? "ON" : "OFF") + 
                      (remaining > 0 ? " (" + String(remaining) + "s restantes)" : ""));
        return true;
    }
    else {
        Serial.println("❌ Ação inválida: '" + action + "' (use: on, off, toggle, status)");
        return false;
    }
}

void RelayCommandBox::turnOffAllRelays() {
    Serial.println("🔄 Desligando todos os relés...");
    
    for (int i = 0; i < MAX_RELAYS; i++) {
        setRelay(i, false);
        delay(50); // Pequeno delay entre comandos
    }
    
    Serial.println("✅ Todos os relés desligados");
}

bool RelayCommandBox::getRelayState(int relayNumber) {
    if (!isValidRelayNumber(relayNumber)) {
        return false;
    }
    return relayStates[relayNumber].isOn;
}

int RelayCommandBox::getRemainingTime(int relayNumber) {
    if (!isValidRelayNumber(relayNumber) || !relayStates[relayNumber].hasTimer) {
        return 0;
    }
    
    unsigned long elapsed = (millis() - relayStates[relayNumber].startTime) / 1000;
    int remaining = relayStates[relayNumber].timerSeconds - elapsed;
    
    return remaining > 0 ? remaining : 0;
}

String RelayCommandBox::getRelayName(int relayNumber) {
    if (!isValidRelayNumber(relayNumber)) {
        return "Relé Inválido";
    }
    
    if (relayStates[relayNumber].name.isEmpty()) {
        return "Relé " + String(relayNumber);
    }
    
    return relayStates[relayNumber].name;
}

void RelayCommandBox::setRelayName(int relayNumber, const String& name) {
    if (isValidRelayNumber(relayNumber)) {
        relayStates[relayNumber].name = name;
        Serial.println("📝 Relé " + String(relayNumber) + " renomeado para: " + name);
    }
}

void RelayCommandBox::printStatus() {
    Serial.println("🔌 === STATUS " + deviceName + " ===");
    Serial.println("📍 PCF8574: 0x" + String(i2cAddress, HEX) + " (" + 
                  (pcfInitialized ? "Online" : "Offline") + ")");
    
    for (int i = 0; i < MAX_RELAYS; i++) {
        String status = "   " + getRelayName(i) + ": " + 
                       (relayStates[i].isOn ? "ON" : "OFF");
        
        if (relayStates[i].hasTimer) {
            int remaining = getRemainingTime(i);
            status += " (Timer: " + String(remaining) + "s)";
        }
        
        Serial.println(status);
    }
    Serial.println("===============================");
}

String RelayCommandBox::getStatusJSON() {
    DynamicJsonDocument doc(1024);
    
    doc["device"] = deviceName;
    doc["pcf8574_address"] = "0x" + String(i2cAddress, HEX);
    doc["operational"] = pcfInitialized;
    doc["timestamp"] = millis();
    
    JsonArray relays = doc.createNestedArray("relays");
    
    for (int i = 0; i < MAX_RELAYS; i++) {
        JsonObject relay = relays.createNestedObject();
        relay["number"] = i;
        relay["name"] = getRelayName(i);
        relay["state"] = relayStates[i].isOn;
        relay["hasTimer"] = relayStates[i].hasTimer;
        
        if (relayStates[i].hasTimer) {
            relay["remainingTime"] = getRemainingTime(i);
            relay["totalTime"] = relayStates[i].timerSeconds;
        }
    }
    
    String result;
    serializeJson(doc, result);
    return result;
}

String RelayCommandBox::getDeviceInfoJSON() {
    DynamicJsonDocument doc(512);
    
    doc["deviceName"] = deviceName;
    doc["deviceType"] = "RelayCommandBox";
    doc["numRelays"] = MAX_RELAYS;
    doc["pcf8574Address"] = "0x" + String(i2cAddress, HEX);
    doc["operational"] = pcfInitialized;
    doc["uptime"] = millis();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["macAddress"] = WiFi.macAddress();
    
    String result;
    serializeJson(doc, result);
    return result;
}

void RelayCommandBox::setStateChangeCallback(void (*callback)(int relayNumber, bool state, int remainingTime)) {
    this->stateChangeCallback = callback;
}

void RelayCommandBox::setCommandCallback(void (*callback)(int relayNumber, String action, int duration)) {
    this->commandCallback = callback;
}

// ===== MÉTODOS PRIVADOS =====

bool RelayCommandBox::writeToRelay(int relayNumber, bool state) {
    if (!isValidRelayNumber(relayNumber) || !pcfInitialized) {
        return false;
    }
    
    try {
        // PCF8574 usa lógica invertida (LOW = relé ligado, HIGH = relé desligado)
        // Isso é comum em módulos de relé que usam optoacopladores
        bool pcfState = !state;  // Inverter o estado
        
        pcf8574.write(relayNumber, pcfState ? LOW : HIGH);
        
        // Pequeno delay para estabilizar
        delay(10);
        
        return true;
    } catch (...) {
        Serial.println("❌ Exceção ao escrever no relé " + String(relayNumber));
        return false;
    }
}

void RelayCommandBox::checkTimers() {
    for (int i = 0; i < MAX_RELAYS; i++) {
        if (relayStates[i].hasTimer && relayStates[i].isOn) {
            unsigned long elapsed = (millis() - relayStates[i].startTime) / 1000;
            
            if (elapsed >= relayStates[i].timerSeconds) {
                // Timer expirou - desligar relé
                String relayName = getRelayName(i);
                Serial.println("⏰ Timer do " + relayName + " expirou - desligando");
                
                relayStates[i].isOn = false;
                relayStates[i].hasTimer = false;
                relayStates[i].timerSeconds = 0;
                
                writeToRelay(i, false);
                
                // Chamar callback se definido
                if (stateChangeCallback) {
                    stateChangeCallback(i, false, 0);
                }
            }
        }
    }
}

bool RelayCommandBox::isValidRelayNumber(int relayNumber) {
    return relayNumber >= 0 && relayNumber < MAX_RELAYS;
}

void RelayCommandBox::initializeDefaultNames() {
    // Usar nomes do Config.h
    for (int i = 0; i < MAX_RELAYS; i++) {
        relayStates[i].name = String(RELAY_NAMES[i]);
    }
    
    DEBUG_PRINTLN("✅ Nomes padrão dos relés carregados do Config.h");
}

// ===== 🎯 PERSISTÊNCIA DE ESTADOS (NVS) =====

bool RelayCommandBox::savePersistentStates() {
    PersistentRelayStateData states = {};
    states.timestamp = millis();
    states.numRelays = MAX_RELAYS;
    states.version = 1;  // Versão inicial
    
    // Preencher estados de cada relé
    for (int i = 0; i < MAX_RELAYS; i++) {
        states.relays[i].state = relayStates[i].isOn ? 1 : 0;
        states.relays[i].hasTimer = relayStates[i].hasTimer ? 1 : 0;
        
        if (relayStates[i].hasTimer && relayStates[i].isOn) {
            // Calcular timestamp de expiração do timer
            states.relays[i].timerEndTime = relayStates[i].startTime + (relayStates[i].timerSeconds * 1000);
        } else {
            states.relays[i].timerEndTime = 0;
        }
        
        // Estado persistente = ON sem timer (on_forever)
        states.relays[i].isPersistent = (relayStates[i].isOn && !relayStates[i].hasTimer) ? 1 : 0;
    }
    
    // Calcular checksum
    uint8_t checksum = 0;
    uint8_t* data = (uint8_t*)&states;
    for (size_t i = 0; i < sizeof(PersistentRelayStateData) - 1; i++) {
        checksum ^= data[i];
    }
    states.checksum = checksum;
    
    // Guardar em NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open("relay_states", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        Serial.println("❌ Erro ao abrir NVS para guardar estados: " + String(esp_err_to_name(err)));
        return false;
    }
    
    err = nvs_set_blob(handle, "persistent_states", &states, sizeof(PersistentRelayStateData));
    if (err != ESP_OK) {
        Serial.println("❌ Erro ao guardar estados em NVS: " + String(esp_err_to_name(err)));
        nvs_close(handle);
        return false;
    }
    
    err = nvs_commit(handle);
    nvs_close(handle);
    
    if (err == ESP_OK) {
        Serial.println("💾 Estados persistentes guardados em NVS");
        return true;
    } else {
        Serial.println("❌ Erro ao fazer commit em NVS: " + String(esp_err_to_name(err)));
        return false;
    }
}

bool RelayCommandBox::loadPersistentStates() {
    PersistentRelayStateData states = {};
    
    // Carregar de NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open("relay_states", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        Serial.println("💡 Nenhum estado persistente encontrado em NVS");
        return false;
    }
    
    size_t required_size = sizeof(PersistentRelayStateData);
    err = nvs_get_blob(handle, "persistent_states", &states, &required_size);
    nvs_close(handle);
    
    if (err != ESP_OK) {
        Serial.println("💡 Nenhum estado persistente encontrado em NVS");
        return false;
    }
    
    // Validar checksum
    uint8_t calculatedChecksum = 0;
    uint8_t* data = (uint8_t*)&states;
    for (size_t i = 0; i < sizeof(PersistentRelayStateData) - 1; i++) {
        calculatedChecksum ^= data[i];
    }
    
    if (calculatedChecksum != states.checksum) {
        Serial.println("❌ Checksum inválido ao carregar estados persistentes");
        return false;
    }
    
    // Aplicar estados carregados
    return applyPersistentStates(states);
}

bool RelayCommandBox::applyPersistentStates(const PersistentRelayStateData& states) {
    Serial.println("\n🎯 ========================================");
    Serial.println("🎯 APLICANDO ESTADOS PERSISTENTES");
    Serial.println("🎯 ========================================");
    Serial.println("📦 Total de relés: " + String(states.numRelays));
    Serial.println("📅 Timestamp: " + String(states.timestamp));
    
    bool applied = false;
    
    for (int i = 0; i < MAX_RELAYS && i < states.numRelays; i++) {
        if (states.relays[i].isPersistent) {
            // 🔒 ESTADO PERSISTENTE (on_forever) - aplicar imediatamente
            Serial.println("🔒 Relé " + String(i) + ": Aplicando estado PERSISTENTE " + 
                         (states.relays[i].state ? "ON" : "OFF"));
            setRelay(i, states.relays[i].state == 1);
            applied = true;
        } 
        else if (states.relays[i].hasTimer && states.relays[i].state == 1) {
            // ⏰ TIMER ATIVO - verificar se ainda válido
            uint32_t currentTime = millis();
            if (states.relays[i].timerEndTime > currentTime) {
                uint32_t remaining = (states.relays[i].timerEndTime - currentTime) / 1000;
                Serial.println("⏰ Relé " + String(i) + ": Aplicando timer com " + String(remaining) + "s restantes");
                setRelayWithTimer(i, true, remaining);
                applied = true;
            } else {
                Serial.println("⏰ Relé " + String(i) + ": Timer expirado - desligando");
                setRelay(i, false);
            }
        }
        else if (!states.relays[i].state) {
            // OFF - garantir que está desligado
            setRelay(i, false);
        }
    }
    
    Serial.println("========================================\n");
    
    if (applied) {
        Serial.println("✅ Estados persistentes aplicados com sucesso");
    } else {
        Serial.println("💡 Nenhum estado persistente para aplicar");
    }
    
    return applied;
}

bool RelayCommandBox::getPersistentStates(PersistentRelayStateData& states) {
    states = {};
    states.timestamp = millis();
    states.numRelays = MAX_RELAYS;
    states.version = 1;
    
    // Preencher estados atuais
    for (int i = 0; i < MAX_RELAYS; i++) {
        states.relays[i].state = relayStates[i].isOn ? 1 : 0;
        states.relays[i].hasTimer = relayStates[i].hasTimer ? 1 : 0;
        
        if (relayStates[i].hasTimer && relayStates[i].isOn) {
            states.relays[i].timerEndTime = relayStates[i].startTime + (relayStates[i].timerSeconds * 1000);
        } else {
            states.relays[i].timerEndTime = 0;
        }
        
        // Estado persistente = ON sem timer (on_forever)
        states.relays[i].isPersistent = (relayStates[i].isOn && !relayStates[i].hasTimer) ? 1 : 0;
    }
    
    // Calcular checksum
    uint8_t checksum = 0;
    uint8_t* data = (uint8_t*)&states;
    for (size_t i = 0; i < sizeof(PersistentRelayStateData) - 1; i++) {
        checksum ^= data[i];
    }
    states.checksum = checksum;
    
    return true;
}
