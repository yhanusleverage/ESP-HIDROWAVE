#ifndef HYDRO_CONTROL_H
#define HYDRO_CONTROL_H

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PCF8574.h>
#include "Config.h"
#if USE_PH_MODBUS_SENSOR
#include "PhModbusSensor.h"
#else
#include "PHSensor.h"
#endif
#include "EcAnalogSensor.h"
#include "LevelSensor.h"
#include "DiscreteLevelBank.h"
#include "Controller.h"  // ✅ Controller KP para controle automático de EC
#include "EcDilutionController.h"
#include "FlowmeterSensor.h"
#include "AdaptivePHController.h"
#include <ArduinoJson.h>  // ✅ Para JsonArray en executeWebDosage
#include "PreferencesManager.h"  // ✅ Para persistência em NVS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ===== SISTEMA SEQUENCIAL SIMPLES =====
enum SequentialState {
    IDLE,           // Parado - nenhuma dosagem ativa
    DOSING,         // Dosando nutriente atual
    WAITING,        // Pausa curta entre nutrientes (~3s) — publicado como "dosing" na UI
    RECIRCULATING   // Aguardando tempo_recirculacao após secuencia completa
};

enum DilutionState {
    DILUTION_IDLE,
    DILUTION_DRAINING,
    DILUTION_FILLING,
    DILUTION_RECIRCULATING
};

struct SimpleNutrient {
    String name;        // Nome do nutriente (ex: "Grow", "Micro")
    int relay;          // Índice do relé (0-15 para ESP-HIDROWAVE)
    float dosageML;     // Quantidade em ml
    int durationMs;     // Duração em milissegundos
};

/** Evento emitido ao completar dosagem de um nutriente (para Supabase nutrient_dosages). */
struct NutrientDoseEvent {
    char sequenceId[24];
    char nutrientName[32];
    int relayNumber;
    float dosageMl;
    float dosageTimeSeconds;
    float ecBefore;
    float ecSetpoint;
    const char* source;
};

typedef void (*NutrientDoseCallback)(const NutrientDoseEvent* event, void* userData);
typedef void (*EcOperationSyncCallback)(void* userData);
typedef void (*PhOperationSyncCallback)(void* userData);
typedef void (*PhysicalRecircCallback)(bool starting, const char* domain, void* userData);

/** Evento emitido ao completar dosagem pH (para Supabase ph_dosages). */
struct PhDoseEvent {
    char sequenceId[24];
    char direction[8];
    int relayNumber;
    float dosageMl;
    float dosageTimeSeconds;
    float phBefore;
    float phSetpoint;
    float kAcid;
    float kBase;
    float errorH;
    const char* source;
};

typedef void (*PhDoseCallback)(const PhDoseEvent* event, void* userData);
typedef void (*PhGainLearnedCallback)(void* userData);

/** Métricas de ciclo Auto EC (cada checkAutoEC con PV válido). */
struct EcControllerMetricEvent {
    float ecSetpoint;
    float ecActual;
    float ecError;
    float kValue;
    float dosageMl;
    float dosageTimeSeconds;
    float baseDose;
    float flowRate;
    float volume;
    float totalMl;
    float kp;
    bool autoEnabled;
    bool adjustmentNeeded;
    bool adjustmentApplied;
    char sequenceId[24];
};

/** Métricas de ciclo Auto pH (cada checkAutoPH con PV válido). */
struct PhControllerMetricEvent {
    float phSetpoint;
    float phBefore;
    float errorH;
    char direction[8];
    float kAcid;
    float kBase;
    float kUsed;
    float doseIdealMl;
    float doseRealMl;
    float dosageTimeSeconds;
    float aggressiveness;
    bool autoEnabled;
    bool adjustmentNeeded;
    bool adjustmentApplied;
    char sequenceId[24];
};

typedef void (*EcMetricCallback)(const EcControllerMetricEvent* event, void* userData);
typedef void (*PhMetricCallback)(const PhControllerMetricEvent* event, void* userData);

/** Evento ao completar diluição EC (dreno + reposição). */
struct EcDilutionEvent {
    char sequenceId[24];
    float ecBefore;
    float ecSetpoint;
    float volumeTargetL;
    float volumeMeasuredL;
    float drainDurationSec;
    float fillDurationSec;
    const char* source;
};

typedef void (*EcDilutionCallback)(const EcDilutionEvent* event, void* userData);

class HydroControl {
public:
    #ifndef NUM_RELAYS
        #define NUM_RELAYS 16
    #endif
    // La macro NUM_RELAYS se usa para arrays (líneas 110-112)
    // Para acceso como constante estática, usar directamente el valor o la macro
    static constexpr int NUM_RELAYS_VALUE = 16;  // Valor debe coincidir con NUM_RELAYS
    
    HydroControl();
    bool begin();
    void loop();
    void update();
    void showMessage(String msg);
    void toggleRelay(int relay, int seconds = 0);
    void setRelay(int relay, bool state, int seconds = 0);  // ✅ NOVO: Define estado direto
    void updateSensorData(float temp, float humidity, float ph, float tds);
    void updateRelayTimers();
    bool* getRelayStates() { return relayStates; }
    bool areSensorsWorking() { return sensorsOk; }
    bool isWaterLevelOk() { return tankLevelOk; }
    /** true si nivel bajo o hold P1 activo — pausa checkAutoEC/checkAutoPH. */
    bool isAutoDosingPausedByInterlock() const;
    /** Extiende pausa de dosaje químico durante script tanque (P1). */
    void holdAutoDosingForTankScript(unsigned long durationMs);
    bool isLevelWet(int levelIndex) const;
    const char* getWaterLevelAggregate() const;
    bool isDiscreteLevelBankActive() const { return levelBank.isAvailable(); }
    
    // Getters para leituras dos sensores
    float& getTemperature() { return temperature; }
    const float& getTemperature() const { return temperature; }
    float& getpH() { return pH; }
    const float& getpH() const { return pH; }
    float& getTDS() { return tds; }
    const float& getTDS() const { return tds; }
    float& getEC() { return ec; }
    const float& getEC() const { return ec; }
    bool isEcValid() const { return ecValid; }
    bool isPhValidForTelemetry() const;
    bool isEcValidForTelemetry() const;
    bool isTempValidForTelemetry() const;
    String getTankStatus();
    float getWaterTemp();
    
    // ✅ Controller KP - Controle automático de EC
    ECController& getECController() { return ecController; }
    const ECController& getECController() const { return ecController; }
    void setECSetpoint(float setpoint, bool saveToNVS = true);  // ✅ saveToNVS: false para evitar guardar múltiples veces
    float getECSetpoint() const { return ecSetpoint; }
    void setECTolerance(float tolerance, bool saveToNVS = true);
    float getECTolerance() const { return ecTolerance; }
    void setAutoECEnabled(bool enabled, bool saveToNVS = true);  // ✅ saveToNVS: false para evitar guardar múltiples veces
    bool isAutoECEnabled() const { return autoECEnabled; }
    void setAutoECInterval(int intervalSeconds, bool saveToNVS = true);  // ✅ saveToNVS: false para evitar guardar múltiples veces
    int getAutoECInterval() const { return autoECIntervalSeconds; }
    void setTempoRecirculacaoSeconds(unsigned long seconds);
    unsigned long getTempoRecirculacaoSeconds() const { return tempoRecirculacaoSeconds; }
    void setNutrientDoseCallback(NutrientDoseCallback cb, void* userData);
    void setEcOperationSyncCallback(EcOperationSyncCallback cb, void* userData);
    void setPhysicalRecircCallback(PhysicalRecircCallback cb, void* userData);
    void setEcMetricCallback(EcMetricCallback cb, void* userData);
    void setPhMetricCallback(PhMetricCallback cb, void* userData);

    /** Estado operacional Auto EC para UI (relay_master.ec_operation_*). */
    const char* getEcOperationStateName() const;
    int getEcOperationRemainingSec() const;
    int getEcNextCheckInSec() const;
    float getEcDilutionTargetL() const { return dilutionTargetL; }
    float getEcDilutionProgressL() const { return dilutionProgressL; }
    bool isDilutionActive() const { return dilutionState != DILUTION_IDLE; }

    void setDilutionAutoEnabled(bool enabled, bool saveToNVS = true);
    bool isDilutionAutoEnabled() const { return dilutionAutoEnabled; }
    void setDilutionRelays(int drainRelay, int fillRelay);
    void setDilutionMaxVolumeL(float maxL);
    void setDilutionFillFlowLps(float lps);
    void setFlowmeterPulsesPerLiter(float ppl);
    bool startEcDilution(float volumeLiters, const char* source);
    void setEcDilutionCallback(EcDilutionCallback cb, void* userData);
    EcDilutionController& getEcDilutionController() { return ecDilutionController; }

    // ✅ Auto pH adaptativo
    AdaptivePHController& getAdaptivePHController() { return adaptivePhController; }
    const AdaptivePHController& getAdaptivePHController() const { return adaptivePhController; }
    void setPHSetpoint(float setpoint, bool saveToNVS = true);
    float getPHSetpoint() const { return phSetpoint; }
    void setPHTolerance(float tolerance) { phTolerance = tolerance; }
    float getPHTolerance() const { return phTolerance; }
    void setAutoPHEnabled(bool enabled, bool saveToNVS = true);
    bool isAutoPHEnabled() const { return autoPHEnabled; }
    void setAutoPHInterval(int intervalSeconds, bool saveToNVS = true);
    int getAutoPHInterval() const { return autoPHIntervalSeconds; }
    void setPhPumpConfig(int relayUp, int relayDown, float flowUp, float flowDown,
                         float mlPerUnitAcid, float mlPerUnitBase);
    void setPhAdaptiveConfig(float aggressiveness, float gainAlpha,
                             float maxDoseMl, int maxPulseSec, int maxConsecutive);
    void resetPhLearnedGains();
    void setPhDoseCallback(PhDoseCallback cb, void* userData);
    void setPhGainLearnedCallback(PhGainLearnedCallback cb, void* userData);
    float getPhErrorH() const;
    void setPhRecirculacaoSeconds(unsigned long seconds) { phRecircSeconds = seconds > 0 ? seconds : 60; }
    void setPhOperationSyncCallback(PhOperationSyncCallback cb, void* userData);
    const char* getPhOperationStateName() const;
    int getPhOperationRemainingSec() const;
    int getPhNextCheckInSec() const;
    
    // ✅ TEMPO MORTO (recirculação)
    void setTempoRecirculacao(unsigned long segundos) { tempoRecirculacao = segundos; }
    unsigned long getTempoRecirculacao() const { return tempoRecirculacao; }
    
    // ✅ Persistência pública
    void saveECControllerConfig();  // Salvar configuração do Controller
    void saveNutrientProportions();  // Salvar proporções nutricionais
    
    // ✅ Calibração TDS/EC
    bool calibrateTDS(float standardValue, float measuredValue);  // Calibrar com valores padrão e medido
    bool calibrateTDSWithSolution1413();  // Calibrar automaticamente com solução 1413 µS/cm
    void setTDSCalibrationFactor(float factor);  // Definir fator de calibração manualmente
    void setTDSVRef(float vref);  // Atualizar tensão de referência
    float getTDSCalibrationFactor() const;  // Obter fator de calibração atual
    float getTDSVRef() const;  // Obter tensão de referência atual
    void saveTDSCalibration();  // Salvar calibração EC no NVS
    bool processEcSerialCommand(const String& command);
    
    // ✅ Sistema Sequencial de Dosagem
    void startSimpleSequentialDosage(float totalML, float ecSetpoint, float ecActual);
    void executeWebDosage(JsonArray distribution, int intervalo);
    bool isDosageActive() const { return (currentState != IDLE); }
    void cancelCurrentDosage();
    
    // ✅ Proporções Dinâmicas da Tabela Nutricional
    void updateNutrientProportions(JsonArray nutrients);  // Receber do frontend
    void calculateProportionsFromMlPerLiter(JsonArray nutrients);  // Calcular proporções

private:
    // Hardware
    LiquidCrystal_I2C lcd;
    OneWire oneWire;
    DallasTemperature sensors;
    PCF8574 pcf1, pcf2;
#if USE_PH_MODBUS_SENSOR
    PhModbusSensor* phModbusSensor;
#else
    phSensor* pHSensor;
#endif
    EcAnalogSensor* ecSensor;
    LevelSensor* tankSensor;
    DiscreteLevelBank levelBank;
    
    // Status dos PCF8574
    bool pcf1_ok;
    bool pcf2_ok;
    
    // Status dos sensores
    bool sensorsOk;
    bool tankLevelOk;
    
    // Leituras dos sensores
    float temperature;
    float pH;
    float tds;
    float ec;
    bool ecValid;
    bool phValid;
    bool tempValid;
    unsigned long lastPhValidMs;
    unsigned long lastEcValidMs;
    unsigned long lastTempValidMs;
    
    // Estado dos relés
    bool relayStates[NUM_RELAYS];
    unsigned long startTimes[NUM_RELAYS];
    int timerSeconds[NUM_RELAYS];
    
    // ✅ Controller KP e controle automático de EC
    ECController ecController;
    EcDilutionController ecDilutionController;
    FlowmeterSensor* flowmeterSensor;
    float ecSetpoint;
    float ecTolerance;
    bool autoECEnabled;
    unsigned long lastECCheck;
    static const unsigned long EC_CHECK_INTERVAL = 30000; // 30 segundos
    int autoECIntervalSeconds;
    unsigned long tempoRecirculacaoSeconds;
    unsigned long lastECCheckAtMs;
    float ecAtLastSequenceStart;
    float ecSetpointAtLastSequence;
    String currentSequenceId;
    const char* currentDoseSource;
    NutrientDoseCallback nutrientDoseCallback;
    void* nutrientDoseCallbackUserData;
    EcOperationSyncCallback ecOperationSyncCallback;
    void* ecOperationSyncCallbackUserData;
    PhysicalRecircCallback physicalRecircCallback;
    void* physicalRecircCallbackUserData;

    // ✅ Auto pH adaptativo
    AdaptivePHController adaptivePhController;
    float phSetpoint;
    float phTolerance;
    bool autoPHEnabled;
    unsigned long lastPHCheck;
    unsigned long lastPHCheckAtMs;
    int autoPHIntervalSeconds;
    int relayPhUp;
    int relayPhDown;
    float flowRatePhUp;
    float flowRatePhDown;
    float mlPerPhUnitAcid;
    float mlPerPhUnitBase;
    float phAggressiveness;
    float phGainAlpha;
    float phMaxDoseMl;
    int phMaxPulseSec;
    int phMaxConsecutive;
    int phConsecutiveCorrections;
    unsigned long phRecircSeconds;
    enum PhAutoState { PH_IDLE, PH_DOSING, PH_RECIRCULATING };
    PhAutoState phAutoState;
    unsigned long phStateStartMs;
    int phActiveRelay;
    PhCorrectionPath phActivePath;
    float phCycleHBefore;
    float phCyclePhBefore;
    float phCycleMlApplied;
    float phCycleDurationSec;
    unsigned long phCycleDurationMs;
    String phCurrentSequenceId;
    PhOperationSyncCallback phOperationSyncCallback;
    void* phOperationSyncCallbackUserData;
    PhDoseCallback phDoseCallback;
    void* phDoseCallbackUserData;
    EcMetricCallback ecMetricCallback;
    void* ecMetricCallbackUserData;
    PhMetricCallback phMetricCallback;
    void* phMetricCallbackUserData;
    PhGainLearnedCallback phGainLearnedCallback;
    void* phGainLearnedCallbackUserData;

    // Diluição EC modo A
    DilutionState dilutionState;
    bool dilutionAutoEnabled;
    int dilutionDrainRelay;
    int dilutionFillRelay;
    float dilutionMaxVolumeL;
    float dilutionFillFlowLps;
    float dilutionTargetL;
    float dilutionProgressL;
    float dilutionDrainMeasuredL;
    unsigned long dilutionStateStartMs;
    unsigned long dilutionDrainStartMs;
    unsigned long dilutionFillStartMs;
    unsigned long dilutionFillDurationMs;
    unsigned long dilutionLastPulseMs;
    uint32_t dilutionLastPulseCount;
    String dilutionSequenceId;
    const char* dilutionSource;
    float dilutionEcBefore;
    EcDilutionCallback ecDilutionCallback;
    void* ecDilutionCallbackUserData;
    
    // ✅ TEMPO MORTO (recirculação) - Aguardar após dosagem antes de medir EC novamente
    unsigned long lastDosageCompleteTime;  // Timestamp da última dosagem completa
    unsigned long tankScriptHoldUntilMs;   // P1 interlock: pausa Auto EC/pH até este millis
    unsigned long tempoRecirculacao;       // Tempo de espera em SEGUNDOS
    
    // ✅ Sistema Sequencial de Dosagem (variáveis privadas)
    SequentialState currentState;
    SimpleNutrient nutrients[8];  // Máximo 8 nutrientes (ajustado para 16 relés)
    int totalNutrients;
    int currentNutrientIndex;
    unsigned long stateStartTime;
    int intervalSeconds;
    
    // ✅ TASK DEDICADA para timing preciso de dosagem
    TaskHandle_t dosingTaskHandle;
    SemaphoreHandle_t dosingMutex;
    volatile bool dosingTaskRunning;
    static void dosingTaskFunction(void* parameter);
    void processDosingTask();  // Função interna da task
    
    // ✅ Proporções Dinâmicas da Tabela Nutricional
    struct NutrientProportion {
        String name;
        int relay;      // Índice do relé (0-15)
        float mlPerLiter;  // ml/L do nutriente
        float proportion;  // Proporção calculada (0.0 - 1.0)
        bool active;   // Se está ativo
    };
    NutrientProportion dynamicProportions[16];  // Máximo 16 nutrientes (um por relé)
    int activeNutrientsCount;
    float totalMlPerLiter;  // Soma total de mlPerLiter
    
    // Funções internas
    void updateSensors();
    void updateDisplay();
    void checkRelayTimers();
    void checkAutoEC();
    void checkAutoPH();  // ✅ Verificar e ajustar pH automaticamente
    void processPhAutoState();
    void processSimpleSequential();  // ✅ Máquina de estados para dosagem sequencial
    void processDilution();
    void setDilutionRelay(int relayIndex, bool on);
    void finishDilutionDrainPhase();
    void finishDilutionSequence(bool success);
    void emitEcDilutionEvent();
    void emitNutrientDoseEvent(const SimpleNutrient& nutrient);
    void notifyEcOperationChanged();
    void notifyPhOperationChanged();
    void notifyPhysicalRecirc(bool starting, const char* domain);
    void startPhAutoDosage(int relay, float durationSec, PhCorrectionPath path,
                           float mlApplied, float hBefore, float phBefore);
    void finishPhRecirculation();
    void emitPhDoseEvent();
    void emitEcControllerMetric(bool adjustmentNeeded, bool adjustmentApplied,
                                float dosageMl, float dosageTimeSec, float ecError,
                                const String& sequenceId);
    void emitPhControllerMetric(bool adjustmentNeeded, bool adjustmentApplied,
                                PhCorrectionPath path, const PhDosePlan* plan,
                                const String& sequenceId);
    int computeEcOperationRemainingSec() const;
    int computePhOperationRemainingSec() const;
    
    // ✅ Persistência em NVS (privadas - carregamento automático)
    void loadECControllerConfig();  // Carregar configuração do Controller ao iniciar
    void loadPHControllerConfig();  // Carregar SP/auto/interval pH do NVS ao iniciar
    void loadNutrientProportions();  // Carregar proporções nutricionais ao iniciar
    void loadEcCalibrationFromNVS();
    void saveEcCalibrationToNVS();
};

#endif