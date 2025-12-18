#ifndef DECISION_ENGINE_LOOP_H
#define DECISION_ENGINE_LOOP_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "MasterSlaveManager.h"
#include "GlobalEventBus.h"

// ✅ REGRA DE DECISÃO
struct DecisionRule {
    String id;
    String condition;      // Ex: "temperature > 25 && ph < 6.5"
    String slave_mac;   // MAC do slave (ou "local" para Master)
    int relay_number;
    String action;        // "on" | "off"
    int duration;        // 0 = permanente, >0 = temporário (segundos)
    bool enabled;
    unsigned long lastExecuted;
    int cooldownSeconds;  // Tempo mínimo entre execuções
};

class DecisionEngineLoop {
private:
    std::vector<DecisionRule> rules;
    MasterSlaveManager* masterManager;
    GlobalEventBus* eventBus;
    
    // ✅ FUNÇÕES DE AVALIAÇÃO
    float getSensorValue(const String& sensorName);
    bool getRelayState(const String& slaveMac, int relayNumber);
    bool evaluateCondition(const String& condition);
    
    // ✅ EXECUÇÃO
    void executeRule(const DecisionRule& rule);
    bool shouldExecuteRule(const DecisionRule& rule);
    
public:
    DecisionEngineLoop();
    
    void begin(MasterSlaveManager* masterManager, GlobalEventBus* eventBus);
    
    // ✅ GERENCIAR REGRAS
    void addRule(const DecisionRule& rule);
    void removeRule(const String& ruleId);
    void enableRule(const String& ruleId, bool enabled);
    
    // ✅ LOOP PRIMÁRIO: Avaliar e executar regras
    void loop();
    
    // ✅ OBTER REGRAS
    std::vector<DecisionRule> getRules() const { return rules; }
};

#endif



