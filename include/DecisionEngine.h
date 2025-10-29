#ifndef DECISION_ENGINE_H
#define DECISION_ENGINE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include "DataTypes.h"
#include "Config.h"

// ===== ESTRUTURAS DO MOTOR DE DECISÕES =====

/**
 * @brief Tipos de condições para avaliação de regras
 */
enum ConditionType {
    SENSOR_COMPARE,     // Comparar valor do sensor (pH < 6.0)
    TIME_WINDOW,        // Janela de tempo (entre 08:00-18:00)
    RELAY_STATE,        // Estado de outro relé
    SYSTEM_STATUS,      // Status do sistema (nível ok, wifi conectado)
    COMPOSITE           // Condição composta (AND/OR)
};

/**
 * @brief Operadores de comparação
 */
enum CompareOperator {
    OP_LESS_THAN,           // <
    OP_LESS_EQUAL,          // <=
    OP_GREATER_THAN,        // >
    OP_GREATER_EQUAL,       // >=
    OP_EQUAL,               // ==
    OP_NOT_EQUAL,           // !=
    OP_BETWEEN,             // between min-max
    OP_OUTSIDE              // outside min-max
};

/**
 * @brief Tipos de ação que podem ser executadas
 */
enum ActionType {
    RELAY_ON,               // Ligar relé
    RELAY_OFF,              // Desligar relé
    RELAY_PULSE,            // Pulso com duração específica
    RELAY_PWM,              // Controle PWM (para dosagem proporcional)
    SYSTEM_ALERT,           // Enviar alerta
    LOG_EVENT,              // Registrar evento
    SUPABASE_UPDATE         // Atualizar dados no Supabase
};

/**
 * @brief Estrutura de condição individual
 */
struct RuleCondition {
    ConditionType type;
    String sensor_name;         // "ph", "tds", "temp_water", "temp_env", "humidity", "level"
    CompareOperator op;
    float value_min;
    float value_max;
    String string_value;        // Para comparações de string
    bool negate;                // Negação da condição (!condition)
    
    // Para condições compostas
    std::vector<RuleCondition> sub_conditions;
    String logic_operator;      // "AND", "OR"
    
    RuleCondition() : type(SENSOR_COMPARE), op(OP_GREATER_THAN), 
                     value_min(0.0), value_max(0.0), negate(false) {}
};

/**
 * @brief Estrutura de ação a ser executada
 */
struct RuleAction {
    ActionType type;
    int target_relay;           // ID do relé alvo (0-15)
    unsigned long duration_ms;  // Duração da ação em ms
    float value;                // Valor para PWM ou dosagem
    String message;             // Mensagem para logs/alertas
    bool repeat;                // Se deve repetir a ação
    unsigned long repeat_interval_ms; // Intervalo de repetição
    
    RuleAction() : type(RELAY_ON), target_relay(0), duration_ms(0), 
                  value(0.0), repeat(false), repeat_interval_ms(0) {}
};

/**
 * @brief Estrutura de segurança (interlocks)
 */
struct SafetyCheck {
    String name;
    RuleCondition condition;
    String error_message;
    bool is_critical;           // Se true, para todo o sistema
    
    SafetyCheck() : is_critical(false) {}
};

/**
 * @brief Estrutura principal de uma regra
 */
struct DecisionRule {
    String id;                  // ID único da regra
    String name;                // Nome descritivo
    String description;         // Descrição detalhada
    bool enabled;               // Se a regra está ativa
    int priority;               // Prioridade (0-100, maior = mais importante)
    
    // Condições
    RuleCondition condition;    // Condição principal
    std::vector<SafetyCheck> safety_checks; // Verificações de segurança
    
    // Ações
    std::vector<RuleAction> actions;
    
    // Controle de execução
    String trigger_type;        // "periodic", "on_change", "scheduled"
    unsigned long trigger_interval_ms; // Para triggers periódicos
    unsigned long cooldown_ms;  // Tempo mínimo entre execuções
    unsigned long max_executions_per_hour; // Limite de execuções por hora
    
    // Estado runtime
    unsigned long last_execution;
    unsigned long execution_count_hour;
    unsigned long hour_reset_time;
    bool currently_active;
    
    DecisionRule() : enabled(true), priority(50), trigger_interval_ms(30000),
                    cooldown_ms(0), max_executions_per_hour(0),
                    last_execution(0), execution_count_hour(0),
                    hour_reset_time(0), currently_active(false) {}
};

/**
 * @brief Estado atual do sistema para avaliação de regras
 */
struct SystemState {
    // Leituras dos sensores
    float ph;
    float tds;
    float ec;
    float temp_water;
    float temp_environment;
    float humidity;
    bool water_level_ok;
    
    // Estados dos relés
    bool relay_states[MAX_RELAYS];
    unsigned long relay_start_times[MAX_RELAYS];
    
    // Status do sistema
    bool wifi_connected;
    bool supabase_connected;
    unsigned long uptime;
    uint32_t free_heap;
    
    // Timestamp da última atualização
    unsigned long last_update;
    
    SystemState() : ph(7.0), tds(0.0), ec(0.0), temp_water(20.0),
                   temp_environment(20.0), humidity(50.0), water_level_ok(false),
                   wifi_connected(false), supabase_connected(false),
                   uptime(0), free_heap(0), last_update(0) {
        memset(relay_states, false, sizeof(relay_states));
        memset(relay_start_times, 0, sizeof(relay_start_times));
    }
};

/**
 * @brief Motor de Decisões Principal
 */
class DecisionEngine {
private:
    std::vector<DecisionRule> rules;
    SystemState current_state;
    
    // Controle de execução
    unsigned long last_evaluation;
    unsigned long evaluation_interval;
    bool dry_run_mode;
    
    // Estatísticas
    unsigned long total_evaluations;
    unsigned long total_actions_executed;
    unsigned long total_safety_blocks;
    
    // Configurações
    static const size_t MAX_RULES = 50;
    static const size_t JSON_BUFFER_SIZE = 8192;
    static const unsigned long DEFAULT_EVALUATION_INTERVAL = 5000; // 5s
    
public:
    DecisionEngine();
    ~DecisionEngine();
    
    // ===== CONTROLE PRINCIPAL =====
    bool begin();
    void loop();
    void end();
    
    // ===== GERENCIAMENTO DE REGRAS =====
    bool loadRulesFromFile(const String& filename = "/rules.json");
    bool saveRulesToFile(const String& filename = "/rules.json");
    bool addRule(const DecisionRule& rule);
    bool removeRule(const String& rule_id);
    bool updateRule(const String& rule_id, const DecisionRule& new_rule);
    DecisionRule* getRule(const String& rule_id);
    std::vector<DecisionRule>& getAllRules() { return rules; }
    
    // ===== AVALIAÇÃO E EXECUÇÃO =====
    void updateSystemState(const SystemState& state);
    void evaluateAllRules();
    bool evaluateCondition(const RuleCondition& condition, const SystemState& state);
    bool checkSafetyConstraints(const DecisionRule& rule, const SystemState& state);
    void executeActions(const std::vector<RuleAction>& actions, const String& rule_id);
    
    // ===== CONTROLE DE MODO =====
    void setDryRunMode(bool enabled) { dry_run_mode = enabled; }
    bool isDryRunMode() const { return dry_run_mode; }
    void setEvaluationInterval(unsigned long interval_ms) { evaluation_interval = interval_ms; }
    
    // ===== ESTATÍSTICAS E DEBUG =====
    void printStatistics();
    void printRuleStatus();
    String getSystemStateJSON();
    String getRuleExecutionLog();
    void resetStatistics();
    
    // ===== VALIDAÇÃO =====
    bool validateRule(const DecisionRule& rule, String& error_message);
    bool validateJSON(const String& json_str);
    
    // ===== CALLBACKS =====
    typedef std::function<void(int relay, bool state, unsigned long duration)> RelayControlCallback;
    typedef std::function<void(const String& message, bool is_critical)> AlertCallback;
    typedef std::function<void(const String& event, const String& data)> LogCallback;
    
    void setRelayControlCallback(RelayControlCallback callback) { relay_control_callback = callback; }
    void setAlertCallback(AlertCallback callback) { alert_callback = callback; }
    void setLogCallback(LogCallback callback) { log_callback = callback; }

private:
    // ===== CALLBACKS INTERNOS =====
    RelayControlCallback relay_control_callback;
    AlertCallback alert_callback;
    LogCallback log_callback;
    
    // ===== MÉTODOS INTERNOS =====
    bool parseRuleFromJSON(const JsonObject& json_rule, DecisionRule& rule);
    JsonObject ruleToJSON(const DecisionRule& rule, JsonDocument& doc);
    bool parseConditionFromJSON(const JsonObject& json_cond, RuleCondition& condition);
    JsonObject conditionToJSON(const RuleCondition& condition, JsonDocument& doc);
    bool parseActionFromJSON(const JsonObject& json_action, RuleAction& action);
    JsonObject actionToJSON(const RuleAction& action, JsonDocument& doc);
    
    float getSensorValue(const String& sensor_name, const SystemState& state);
    bool compareValues(float sensor_value, CompareOperator op, float target_min, float target_max);
    
    void logRuleExecution(const String& rule_id, const String& action, bool success);
    void updateExecutionCounts(DecisionRule& rule);
    bool isInCooldown(const DecisionRule& rule);
    bool hasExceededHourlyLimit(const DecisionRule& rule);
    
    void executeRelayAction(const RuleAction& action, const String& rule_id);
    void executeSystemAlert(const RuleAction& action, const String& rule_id);
    void executeLogEvent(const RuleAction& action, const String& rule_id);
    
    /**
     * @brief Cria regras padrão para demonstração
     */
    void createDefaultRules();
};

#endif // DECISION_ENGINE_H
