#include "DecisionEngineLoop.h"
#include "ESPNowController.h"
#include <ArduinoJson.h>

DecisionEngineLoop::DecisionEngineLoop() : 
    masterManager(nullptr),
    eventBus(nullptr),
    relayCoordinator(nullptr) {
}

void DecisionEngineLoop::begin(MasterSlaveManager* masterManager, GlobalEventBus* eventBus,
                                RelayCoordinator* relayCoordinator) {
    this->masterManager = masterManager;
    this->eventBus = eventBus;
    this->relayCoordinator = relayCoordinator;
    
    Serial.println("✅ DecisionEngineLoop inicializado");
    Serial.println("   🧠 Loop primário de decisão ativo");
    Serial.println("   ⚡ Avaliação sistemática de regras");
}

void DecisionEngineLoop::addRule(const DecisionRule& rule) {
    rules.push_back(rule);
    Serial.printf("✅ Regra adicionada: %s\n", rule.id.c_str());
}

void DecisionEngineLoop::removeRule(const String& ruleId) {
    rules.erase(
        std::remove_if(rules.begin(), rules.end(),
            [&ruleId](const DecisionRule& r) { return r.id == ruleId; }),
        rules.end()
    );
}

void DecisionEngineLoop::enableRule(const String& ruleId, bool enabled) {
    for (auto& rule : rules) {
        if (rule.id == ruleId) {
            rule.enabled = enabled;
            Serial.printf("✅ Regra %s %s\n", ruleId.c_str(), enabled ? "habilitada" : "desabilitada");
            return;
        }
    }
}

float DecisionEngineLoop::getSensorValue(const String& sensorName) {
    // ✅ INTEGRAR: Com HydroControl para obter valores de sensores
    // Por enquanto, retornar 0 (placeholder)
    // Futuro: Integrar com hydroControl->getTemperature(), getPH(), etc.
    return 0.0f;
}

bool DecisionEngineLoop::getRelayState(const String& slaveMac, int relayNumber) {
    if (!masterManager) return false;
    
    // ✅ OBTER: Estado do relé do slave
    if (slaveMac == "local" || slaveMac.isEmpty()) {
        // Relé local (Master) - integrar com HydroControl
        return false; // Placeholder
    } else {
        // Relé remoto (Slave) - obter do MasterSlaveManager
        uint8_t mac[6];
        sscanf(slaveMac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
        
        // Obter estado do slave (precisa implementar getRelayState no MasterSlaveManager)
        return false; // Placeholder
    }
}

bool DecisionEngineLoop::evaluateCondition(const String& condition) {
    // ✅ PARSER SIMPLES: Avaliar condições
    // Ex: "temperature > 25" → getSensorValue("temperature") > 25
    // Ex: "ph < 6.5" → getSensorValue("ph") < 6.5
    // Ex: "relay_1 == on" → getRelayState("local", 1) == true
    
    // Por enquanto, parser simples
    // Futuro: Parser mais robusto com suporte a &&, ||, etc.
    
    if (condition.indexOf("temperature") >= 0) {
        float temp = getSensorValue("temperature");
        if (condition.indexOf(">") >= 0) {
            float threshold = condition.substring(condition.indexOf(">") + 1).toFloat();
            return temp > threshold;
        } else if (condition.indexOf("<") >= 0) {
            float threshold = condition.substring(condition.indexOf("<") + 1).toFloat();
            return temp < threshold;
        }
    }
    
    // Placeholder: sempre retorna false
    return false;
}

bool DecisionEngineLoop::shouldExecuteRule(const DecisionRule& rule) {
    if (!rule.enabled) return false;
    
    // ✅ COOLDOWN: Verificar se passou tempo suficiente desde última execução
    if (rule.cooldownSeconds > 0) {
        unsigned long timeSinceLastExecution = (millis() - rule.lastExecuted) / 1000;
        if (timeSinceLastExecution < rule.cooldownSeconds) {
            return false; // Ainda em cooldown
        }
    }
    
    return true;
}

void DecisionEngineLoop::executeRule(const DecisionRule& rule) {
    Serial.printf("\n⚡ [DecisionEngine] Executando regra: %s\n", rule.id.c_str());
    Serial.printf("   Condição: %s\n", rule.condition.c_str());
    Serial.printf("   Ação: Relé %d -> %s\n", rule.relay_number, rule.action.c_str());
    
    // ✅ CONVERTER: MAC string para uint8_t[6]
    uint8_t mac[6] = {0};
    if (rule.slave_mac != "local" && !rule.slave_mac.isEmpty()) {
        sscanf(rule.slave_mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
    }
    
    // ✅ ENVIAR: Comando via ESP-NOW
    if (masterManager) {
        bool success = false;
        
        if (rule.slave_mac == "local" || rule.slave_mac.isEmpty()) {
            // Comando local (Master) - integrar com HydroControl
            // Por enquanto, não implementado
        } else {
            // Comando remoto (Slave)
            if (relayCoordinator) {
                success = relayCoordinator->actuateSlave(
                    RelayOwner::DecisionRule, mac, rule.relay_number, rule.action, rule.duration) > 0;
            } else {
                success = masterManager->sendRelayCommandToSlave(
                    mac, rule.relay_number, rule.action, rule.duration, 0, true
                );
            }
        }
        
        if (success) {
            // ✅ BROADCAST: Evento de decisão executada
            if (eventBus) {
                DynamicJsonDocument eventDoc(256);
                eventDoc["rule_id"] = rule.id;
                eventDoc["condition"] = rule.condition;
                eventDoc["slave_mac"] = rule.slave_mac;
                eventDoc["relay_number"] = rule.relay_number;
                eventDoc["action"] = rule.action;
                eventDoc["source"] = "decision_engine";
                
                eventBus->publish(GlobalEventType::DECISION_EXECUTED, eventDoc.as<JsonObject>(), "decision_engine");
            }
            
            // ✅ ATUALIZAR: Timestamp da última execução
            for (auto& r : rules) {
                if (r.id == rule.id) {
                    r.lastExecuted = millis();
                    break;
                }
            }
        }
    }
}

// ✅ LOOP PRIMÁRIO: Avaliar e executar regras sistematicamente
void DecisionEngineLoop::loop() {
    if (!masterManager) return;
    
    // ✅ AVALIAR: Todas as regras habilitadas
    for (const auto& rule : rules) {
        if (!shouldExecuteRule(rule)) continue;
        
        // ✅ AVALIAR: Condição da regra
        if (evaluateCondition(rule.condition)) {
            // ✅ EXECUTAR: Regra
            executeRule(rule);
        }
    }
}












