#ifndef DECISION_ENGINE_INTEGRATION_H
#define DECISION_ENGINE_INTEGRATION_H

#include "DecisionEngine.h"
#include "HydroControl.h"
#include "SupabaseClient.h"
#include "DataTypes.h"

/**
 * @brief Classe de integração entre DecisionEngine e sistema ESP-HIDROWAVE
 * 
 * Esta classe serve como ponte entre o motor de decisões e os componentes
 * existentes do sistema, fornecendo callbacks e adaptadores necessários.
 */
class DecisionEngineIntegration {
private:
    DecisionEngine* engine;
    HydroControl* hydroControl;
    SupabaseClient* supabase;
    
    // Estados e configurações
    bool emergency_mode;
    bool manual_override_active;
    std::vector<int> locked_relays;
    
    // Estatísticas de integração
    unsigned long total_relay_commands;
    unsigned long total_alerts_sent;
    unsigned long total_supabase_updates;
    
    // Buffers para logs
    static const size_t LOG_BUFFER_SIZE = 10;
    String execution_log[LOG_BUFFER_SIZE];
    size_t log_index;
    
public:
    DecisionEngineIntegration(DecisionEngine* engine, HydroControl* hydro, SupabaseClient* supa);
    ~DecisionEngineIntegration();
    
    // ===== CONTROLE PRINCIPAL =====
    bool begin();
    void loop();
    void end();
    
    // ===== CONTROLE DE MODO =====
    void setEmergencyMode(bool enabled);
    bool isEmergencyMode() const { return emergency_mode; }
    void setManualOverride(bool enabled);
    bool isManualOverrideActive() const { return manual_override_active; }
    
    // ===== CONTROLE DE RELÉS =====
    void lockRelay(int relay_id);
    void unlockRelay(int relay_id);
    void unlockAllRelays();
    bool isRelayLocked(int relay_id);
    
    // ===== ATUALIZAÇÃO DE ESTADO =====
    void updateSystemStateFromSensors();
    SystemState getCurrentSystemState();
    
    // ===== LOGS E TELEMETRIA =====
    void sendTelemetryToSupabase();
    String getExecutionLogJSON();
    void printIntegrationStatistics();
    
    // ===== VALIDAÇÃO E SEGURANÇA =====
    bool validateRelayCommand(int relay_id, bool state, unsigned long duration);
    void performSafetyChecks();
    void emergencyShutdown(const String& reason);
    
private:
    // ===== CALLBACKS PARA DECISION ENGINE =====
    static void relayControlCallback(int relay, bool state, unsigned long duration, void* context);
    static void alertCallback(const String& message, bool is_critical, void* context);
    static void logCallback(const String& event, const String& data, void* context);
    
    // ===== MÉTODOS INTERNOS =====
    void handleRelayControl(int relay, bool state, unsigned long duration);
    void handleAlert(const String& message, bool is_critical);
    void handleLogEvent(const String& event, const String& data);
    
    void addToExecutionLog(const String& log_entry);
    bool isSystemHealthy();
    void updateSupabaseWithRuleExecution(const String& rule_id, const String& action, bool success);
    
    // ===== INTERLOCKS DE SEGURANÇA =====
    bool checkWaterLevelInterlock();
    bool checkTemperatureInterlock();
    bool checkPowerSupplyInterlock();
    bool checkMemoryInterlock();
    
    // ===== MAPEAMENTO DE SENSORES =====
    float mapSensorValue(const String& sensor_name);
    bool mapSystemStatus(const String& status_name);
};

/**
 * @brief Estrutura para configuração de interlocks de segurança
 */
struct SafetyInterlock {
    String name;
    String description;
    bool is_active;
    bool is_critical;
    std::function<bool()> check_function;
    String last_error;
    unsigned long last_check_time;
    
    SafetyInterlock() : is_active(true), is_critical(false), last_check_time(0) {}
};

/**
 * @brief Manager de interlocks de segurança
 */
class SafetyInterlockManager {
private:
    std::vector<SafetyInterlock> interlocks;
    bool global_safety_enabled;
    unsigned long last_safety_check;
    static const unsigned long SAFETY_CHECK_INTERVAL = 5000; // 5 segundos
    
public:
    SafetyInterlockManager();
    ~SafetyInterlockManager();
    
    // ===== CONTROLE =====
    bool begin();
    void loop();
    void enableGlobalSafety(bool enabled) { global_safety_enabled = enabled; }
    bool isGlobalSafetyEnabled() const { return global_safety_enabled; }
    
    // ===== GERENCIAMENTO DE INTERLOCKS =====
    void addInterlock(const SafetyInterlock& interlock);
    void removeInterlock(const String& name);
    void enableInterlock(const String& name, bool enabled);
    bool isInterlockActive(const String& name);
    
    // ===== VERIFICAÇÕES =====
    bool checkAllInterlocks();
    bool checkCriticalInterlocks();
    std::vector<String> getFailedInterlocks();
    std::vector<String> getCriticalFailures();
    
    // ===== STATUS E DEBUG =====
    void printInterlockStatus();
    String getInterlockStatusJSON();
    size_t getInterlockCount() const { return interlocks.size(); }
    
private:
    SafetyInterlock* findInterlock(const String& name);
    void performSafetyCheck();
};

// ===== INTERLOCKS PADRÃO =====
namespace DefaultInterlocks {
    SafetyInterlock createWaterLevelInterlock(LevelSensor* sensor);
    SafetyInterlock createTemperatureInterlock(float* water_temp, float* env_temp);
    SafetyInterlock createPHInterlock(float* ph_value);
    SafetyInterlock createTDSInterlock(float* tds_value);
    SafetyInterlock createMemoryInterlock();
    SafetyInterlock createWiFiInterlock();
    SafetyInterlock createPowerSupplyInterlock();
}

#endif // DECISION_ENGINE_INTEGRATION_H
