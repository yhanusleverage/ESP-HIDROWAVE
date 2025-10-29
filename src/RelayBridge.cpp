#include "RelayBridge.h"

// Constructor
RelayBridge::RelayBridge(SupabaseClient* supabase, ESPNowTask* espnowTask)
    : supabase(supabase), espnowTask(espnowTask), enabled(false), 
      lastCheck(0), checkInterval(5000), // Check cada 5 segundos
      commandsProcessed(0), commandsSent(0), commandsFailed(0), commandsCompleted(0) {
}

bool RelayBridge::begin() {
    Serial.println("\n🌉 === INICIANDO RELAY BRIDGE ===");
    
    // Verificar componentes
    if (!supabase) {
        Serial.println("❌ SupabaseClient no disponible");
        return false;
    }
    
    if (!espnowTask) {
        Serial.println("❌ ESPNowTask no disponible");
        return false;
    }
    
    if (!supabase->isReady()) {
        Serial.println("⚠️ Supabase no está listo - Bridge en modo standby");
        enabled = false;
        return true; // No es error crítico
    }
    
    if (!espnowTask->isInitialized()) {
        Serial.println("❌ ESP-NOW Task no inicializado");
        return false;
    }
    
    enabled = true;
    Serial.println("✅ RelayBridge inicializado");
    Serial.println("   Intervalo de polling: " + String(checkInterval) + "ms");
    Serial.println("   Supabase: ✅ Conectado");
    Serial.println("   ESP-NOW: ✅ Activo");
    Serial.println("=====================================\n");
    
    return true;
}

void RelayBridge::update() {
    if (!enabled) return;
    
    unsigned long now = millis();
    
    // Polling de comandos pendientes
    if (now - lastCheck >= checkInterval) {
        processSupabaseCommands();
        lastCheck = now;
    }
}

void RelayBridge::processSupabaseCommands() {
    if (!enabled || !supabase || !supabase->isReady()) {
        return;
    }
    
    // Array para almacenar comandos
    const int MAX_COMMANDS = 10;
    RelayCommand commands[MAX_COMMANDS];
    int commandCount = 0;
    
    // Obtener comandos pendientes de Supabase
    if (!supabase->checkForCommands(commands, MAX_COMMANDS, commandCount)) {
        // Error al obtener comandos (no es crítico)
        return;
    }
    
    if (commandCount == 0) {
        // No hay comandos pendientes
        return;
    }
    
    Serial.println("\n🔔 " + String(commandCount) + " comando(s) pendiente(s) en Supabase");
    
    // Procesar cada comando
    for (int i = 0; i < commandCount; i++) {
        RelayCommand& cmd = commands[i];
        
        Serial.println("\n📦 Procesando comando #" + String(cmd.id));
        Serial.println("   Relé: " + String(cmd.relayNumber));
        Serial.println("   Acción: " + cmd.action);
        Serial.println("   Duración: " + String(cmd.durationSeconds) + "s");
        
        // Validar comando
        if (!validateSupabaseCommand(cmd)) {
            Serial.println("❌ Comando inválido");
            markCommandFailed(cmd.id, "Comando inválido");
            commandsFailed++;
            continue;
        }
        
        // Aquí deberías obtener el MAC del slave desde alguna configuración
        // Por ahora, enviar en broadcast o buscar por nombre
        // TODO: Implementar mapeo de relés a slaves
        
        // Convertir y enviar comando
        ESPNowRelayCommand espCmd = toESPNowCommand(cmd);
        
        // Enviar en broadcast por ahora (mejorar después)
        uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        
        bool success = espnowTask->sendRelayCommand(
            broadcastMac,
            espCmd.relayNumber,
            espCmd.action,
            espCmd.duration
        );
        
        if (success) {
            Serial.println("✅ Comando enviado via ESP-NOW");
            markCommandSent(cmd.id);
            commandsSent++;
            
            // Si no tiene duración, marcar como completado inmediatamente
            if (cmd.durationSeconds == 0 && cmd.action != "on") {
                markCommandCompleted(cmd.id);
                commandsCompleted++;
            }
        } else {
            Serial.println("❌ Error al enviar comando via ESP-NOW");
            markCommandFailed(cmd.id, "Error ESP-NOW");
            commandsFailed++;
        }
        
        commandsProcessed++;
        logCommand(cmd, success);
    }
}

bool RelayBridge::sendCommandToSlave(const RelayCommand& supaCmd, const uint8_t* slaveMac) {
    if (!enabled || !espnowTask) {
        return false;
    }
    
    // Convertir comando
    ESPNowRelayCommand espCmd = toESPNowCommand(supaCmd);
    
    // Enviar via ESP-NOW
    bool success = espnowTask->sendRelayCommand(
        slaveMac,
        espCmd.relayNumber,
        espCmd.action,
        espCmd.duration
    );
    
    if (success) {
        commandsSent++;
        logCommand(supaCmd, true);
    } else {
        commandsFailed++;
        logCommand(supaCmd, false);
    }
    
    return success;
}

bool RelayBridge::sendCommandToSlaveByName(const RelayCommand& supaCmd, const String& slaveName) {
    if (!enabled || !espnowTask) {
        return false;
    }
    
    // Buscar MAC del slave por nombre
    uint8_t* slaveMac = espnowTask->findSlaveMac(slaveName);
    
    if (!slaveMac) {
        Serial.println("❌ Slave no encontrado: " + slaveName);
        return false;
    }
    
    return sendCommandToSlave(supaCmd, slaveMac);
}

bool RelayBridge::updateCommandStatus(int commandId, const String& status) {
    if (!enabled || !supabase) {
        return false;
    }
    
    // Actualizar en Supabase según el status
    if (status == "sent") {
        return supabase->markCommandSent(commandId);
    } else if (status == "completed") {
        return supabase->markCommandCompleted(commandId);
    } else if (status == "failed") {
        return supabase->markCommandFailed(commandId, "Error desconocido");
    }
    
    return false;
}

bool RelayBridge::markCommandSent(int commandId) {
    return updateCommandStatus(commandId, "sent");
}

bool RelayBridge::markCommandCompleted(int commandId) {
    return updateCommandStatus(commandId, "completed");
}

bool RelayBridge::markCommandFailed(int commandId, const String& errorMessage) {
    if (!enabled || !supabase) {
        return false;
    }
    
    return supabase->markCommandFailed(commandId, errorMessage);
}

void RelayBridge::setAutoProcessing(bool enabled) {
    this->enabled = enabled;
    Serial.println("🌉 RelayBridge auto-processing: " + String(enabled ? "✅ Habilitado" : "❌ Deshabilitado"));
}

String RelayBridge::getStatsJSON() {
    String json = "{";
    json += "\"enabled\":" + String(enabled ? "true" : "false") + ",";
    json += "\"commandsProcessed\":" + String(commandsProcessed) + ",";
    json += "\"commandsSent\":" + String(commandsSent) + ",";
    json += "\"commandsFailed\":" + String(commandsFailed) + ",";
    json += "\"commandsCompleted\":" + String(commandsCompleted) + ",";
    json += "\"checkInterval\":" + String(checkInterval);
    json += "}";
    return json;
}

void RelayBridge::printStats() {
    Serial.println("\n📊 === RELAY BRIDGE STATS ===");
    Serial.println("Estado: " + String(enabled ? "✅ Activo" : "❌ Inactivo"));
    Serial.println("Comandos procesados: " + String(commandsProcessed));
    Serial.println("Comandos enviados: " + String(commandsSent));
    Serial.println("Comandos completados: " + String(commandsCompleted));
    Serial.println("Comandos fallidos: " + String(commandsFailed));
    Serial.println("Intervalo de polling: " + String(checkInterval) + "ms");
    Serial.println("=============================\n");
}

// ===== MÉTODOS PRIVADOS =====

ESPNowRelayCommand RelayBridge::toESPNowCommand(const RelayCommand& supaCmd) {
    ESPNowRelayCommand espCmd = {};
    
    // Convertir número de relé
    espCmd.relayNumber = (uint8_t)supaCmd.relayNumber;
    
    // Convertir acción (String a char array)
    strncpy(espCmd.action, supaCmd.action.c_str(), sizeof(espCmd.action) - 1);
    espCmd.action[sizeof(espCmd.action) - 1] = '\0'; // Asegurar null terminator
    
    // Convertir duración
    espCmd.duration = (uint32_t)supaCmd.durationSeconds;
    
    // Calcular checksum
    espCmd.checksum = calculateChecksum(&espCmd);
    
    return espCmd;
}

uint8_t RelayBridge::calculateChecksum(const ESPNowRelayCommand* cmd) {
    uint8_t checksum = 0;
    const uint8_t* data = (const uint8_t*)cmd;
    
    // XOR de todos los bytes excepto el checksum
    size_t size = sizeof(ESPNowRelayCommand) - sizeof(cmd->checksum);
    
    for (size_t i = 0; i < size; i++) {
        checksum ^= data[i];
    }
    
    return checksum;
}

bool RelayBridge::validateSupabaseCommand(const RelayCommand& supaCmd) {
    // Validar número de relé (0-15)
    if (supaCmd.relayNumber < 0 || supaCmd.relayNumber > 15) {
        Serial.println("❌ Número de relé inválido: " + String(supaCmd.relayNumber));
        return false;
    }
    
    // Validar acción
    if (supaCmd.action != "on" && supaCmd.action != "off" && 
        supaCmd.action != "toggle" && supaCmd.action != "on_forever") {
        Serial.println("❌ Acción inválida: " + supaCmd.action);
        return false;
    }
    
    // Validar duración
    if (supaCmd.durationSeconds < 0) {
        Serial.println("❌ Duración inválida: " + String(supaCmd.durationSeconds));
        return false;
    }
    
    return true;
}

void RelayBridge::logCommand(const RelayCommand& supaCmd, bool success) {
    String status = success ? "✅" : "❌";
    Serial.println(status + " Comando #" + String(supaCmd.id) + 
                   " | Relé: " + String(supaCmd.relayNumber) + 
                   " | Acción: " + supaCmd.action + 
                   " | Duración: " + String(supaCmd.durationSeconds) + "s");
}

