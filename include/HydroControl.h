#ifndef HYDRO_CONTROL_H
#define HYDRO_CONTROL_H

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "Config.h"
#if HIDRO_ENABLE_DS18B20_FALLBACK
#include <OneWire.h>
#include <DallasTemperature.h>
#endif
#include <PCF8574.h>
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
#include "WaterFlowSensor.h"
#include "FlowSensorBank.h"
#include "AdaptivePHController.h"
#include <ArduinoJson.h>  // ✅ Para JsonArray en executeWebDosage
#include "PreferencesManager.h"  // ✅ Para persistência em NVS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ===== SISTEMA SEQUENCIAL SIMPLES =====
enum SequentialState {
    IDLE,           // Parado - nenhuma dosagem ativa
    DOSING,         // Pulso ON do nutriente atual
    PULSE_GAP,      // Gap entre pulsos do mesmo nutriente
    WAITING,        // Pausa curta entre nutrientes (~3s) — publicado como "dosing" na UI
    RECIRCULATING   // Aguardando tempo_recirculacao após secuencia completa
};

enum DilutionState {
    DILUTION_IDLE,
    DILUTION_WAIT_DRAIN_ACK,
    DILUTION_DRAINING,
    DILUTION_WAIT_FILL_ACK,
    DILUTION_FILLING,
    DILUTION_RECIRCULATING
};

struct SimpleNutrient {
    String name;        // Nome do nutriente (ex: "Grow", "Micro")
    int relay;          // Índice do relé (0-15 para ESP-HIDROWAVE)
    float dosageML;     // Quantidade em ml
    int durationMs;     // Duração total estimada (legado / telemetria)
    float flowRateMlPerS; // Caudal da bomba deste nutriente
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
typedef void (*EcGainLearnedCallback)(void* userData);

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
typedef uint32_t (*DilutionSlaveRelayCallback)(const uint8_t* mac, int relayIndex, bool on, void* userData);

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
    bool toggleRelay(int relay, int seconds = 0);
    bool setRelay(int relay, bool state, int seconds = 0);
    bool isPcf1Ok() const { return pcf1_ok; }
    bool isPcf2Ok() const { return pcf2_ok; }
    void updateSensorData(float temp, float humidity, float ph, float ecUsCm);
    void updateRelayTimers();
    bool* getRelayStates() { return relayStates; }
    bool areSensorsWorking() { return sensorsOk; }
    bool isWaterLevelOk() { return tankLevelOk; }
    /** normal = ≠vazio; carrera = solo alto (4/4). Persistido NVS. */
    enum LevelInterlockMode : uint8_t { LEVEL_INTERLOCK_NORMAL = 0, LEVEL_INTERLOCK_CARRERA = 1 };
    bool setLevelInterlockMode(LevelInterlockMode mode);
    LevelInterlockMode getLevelInterlockMode() const { return levelInterlockMode; }
    const char* getLevelInterlockModeName() const;
    /** true si nivel bajo o procedimiento de tanque/dilución activo — pausa Auto EC/pH. */
    bool isAutoDosingPausedByInterlock() const;
    /**
     * Gate P1: pausa Auto EC/pH mientras un procedimiento secuencial de tanque está activo.
     * Sin timers — liberar con setTankProcedureActive(false) al terminar el script.
     */
    void setTankProcedureActive(bool active);
    bool isTankProcedureActive() const { return tankProcedureHoldCount > 0; }
    bool isLevelWet(int levelIndex) const;
    const char* getWaterLevelAggregate() const;
    bool isDiscreteLevelBankActive() const { return levelBank.isAvailable(); }
    
    // Getters para leituras dos sensores
    float& getTemperature() { return temperature; }
    const float& getTemperature() const { return temperature; }
    float& getpH() { return pH; }
    const float& getpH() const { return pH; }
    /** @deprecated Use getEC() — retorna o mesmo µS/cm (alias para regras legacy "tds"). */
    float& getTDS() { return ec; }
    const float& getTDS() const { return ec; }
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
    void setAutoECInterval(int intervalSeconds, bool saveToNVS = true);
    int getAutoECInterval() const { return autoECIntervalSeconds; }
    void setMaxStepEcFraction(float fraction);
    float getMaxStepEcFraction() const { return ecAggressiveness; }
    void setConsumoEc24hEnabled(bool enabled);
    bool isConsumoEc24hEnabled() const { return consumoEc24hEnabled; }
    void setConsumoPh24hEnabled(bool enabled);
    bool isConsumoPh24hEnabled() const { return consumoPh24hEnabled; }
    void setTempoRecirculacaoSeconds(unsigned long seconds);
    unsigned long getTempoRecirculacaoSeconds() const { return tempoRecirculacaoSeconds; }
    void setEcPulseDosing(float pulseMl, float pulseGapSec);
    float getEcPulseMl() const { return ecPulseMl; }
    float getEcPulseGapSec() const { return ecPulseGapSec; }
    void setPhPulseDosing(float pulseMl, float pulseGapSec);
    float getPhPulseMl() const { return phPulseMl; }
    float getPhPulseGapSec() const { return phPulseGapSec; }
    void setNutrientDoseCallback(NutrientDoseCallback cb, void* userData);
    void setEcOperationSyncCallback(EcOperationSyncCallback cb, void* userData);
    void setPhysicalRecircCallback(PhysicalRecircCallback cb, void* userData);
    void setEcMetricCallback(EcMetricCallback cb, void* userData);
    void setPhMetricCallback(PhMetricCallback cb, void* userData);

    /** Estado operacional Auto EC para UI (relay_master.ec_operation_*). */
    const char* getEcOperationStateName() const;
    int getEcOperationRemainingSec() const;
    int getEcNextCheckInSec() const;
    float getEcDilutionTargetL() const {
        // Em filling a UI não usa litros como stop — target só faz sentido no dreno.
        if (dilutionState == DILUTION_FILLING || dilutionState == DILUTION_WAIT_FILL_ACK) {
            return 0.0f;
        }
        return dilutionTargetL;
    }
    float getEcDilutionProgressL() const { return dilutionProgressL; }
    bool isDilutionActive() const { return dilutionState != DILUTION_IDLE; }
    bool isDilutionAwaitingValve() const {
        return dilutionState == DILUTION_WAIT_DRAIN_ACK || dilutionState == DILUTION_WAIT_FILL_ACK;
    }
    /** true si este relé es dreno/fill de la dilución en curso (bloquea Manual). */
    bool holdsDilutionValve(bool isLocal, const uint8_t* mac, int relay) const;
    void notifyDilutionRelayAck(uint32_t commandId, bool success, bool actualOn);
    void notifyDilutionObservedRelay(const uint8_t* mac, int relay, bool state, bool online);
    /** idle|drain|fill|recirc — telemetría [RES]/[FLOW]. */
    const char* getDilutionPhaseName() const {
        switch (dilutionState) {
            case DILUTION_WAIT_DRAIN_ACK:
            case DILUTION_DRAINING: return "drain";
            case DILUTION_WAIT_FILL_ACK:
            case DILUTION_FILLING: return "fill";
            case DILUTION_RECIRCULATING: return "recirc";
            default: return "idle";
        }
    }

    void setDilutionAutoEnabled(bool enabled, bool saveToNVS = true);
    bool isDilutionAutoEnabled() const { return dilutionAutoEnabled; }
    void setDilutionRelays(int drainRelay, int fillRelay);
    void setDilutionSlaveRelays(const String& drainMac, int drainRelay,
                                const String& fillMac, int fillRelay);
    String getDilutionDrainSlaveMac() const { return dilutionDrainSlaveMac; }
    String getDilutionFillSlaveMac() const { return dilutionFillSlaveMac; }
    int getDilutionDrainRelay() const { return dilutionDrainRelay; }
    int getDilutionFillRelay() const { return dilutionFillRelay; }
    /** YFB5 sesión A→B para ScriptRunner (`wait_liters`). */
    void resetFlowSession();
    float getFlowSessionLiters();
    /** Recirc física para opcode `recirc` (scripts de tanque). */
    void setProcedureRecircActive(bool active);
    void setDilutionSlaveRelayCallback(DilutionSlaveRelayCallback cb, void* userData);
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
    void setPhAdaptiveConfig(float aggressiveness, float gainAlpha);
    void resetPhLearnedGains();
    void setPhDoseCallback(PhDoseCallback cb, void* userData);
    void setPhGainLearnedCallback(PhGainLearnedCallback cb, void* userData);
    void setEcGainLearnedCallback(EcGainLearnedCallback cb, void* userData);
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
    bool abortAutoOperationsOnBoot();
    
    // ✅ Proporções Dinâmicas da Tabela Nutricional
    void updateNutrientProportions(JsonArray nutrients);  // Receber do frontend
    void calculateProportionsFromMlPerLiter(JsonArray nutrients);  // Calcular proporções

private:
    // Hardware
    LiquidCrystal_I2C lcd;
#if HIDRO_ENABLE_DS18B20_FALLBACK
    OneWire oneWire;
    DallasTemperature sensors;
#endif
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
    bool writeLocalPumpPin(int relay, bool logicalOn);
    
    // Status dos sensores
    bool sensorsOk;
    bool tankLevelOk;
    LevelInterlockMode levelInterlockMode;
    
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
    FlowSensorBank flowSensorBank;
    WaterFlowSensor* waterFlowSensor;  // alias slot dilución (bank)
    float ecSetpoint;
    float ecTolerance;
    bool autoECEnabled;
    unsigned long lastECCheck;
    static const unsigned long EC_CHECK_INTERVAL = 30000; // 30 segundos
    static const unsigned long CONSUMO_24H_MS = 24UL * 60UL * 60UL * 1000UL;
    int autoECIntervalSeconds;
    unsigned long tempoRecirculacaoSeconds;
    float ecAggressiveness;
    float ecPulseMl;
    float ecPulseGapSec;
    float pulseRemainingMl;
    float pulseChunkMl;
    unsigned long pulseOnDurationMs;
    int pulseIndex;
    bool consumoEc24hEnabled;
    bool consumoEc24hWindowOpen;
    float consumoEc24hT0;
    unsigned long consumoEc24hStartMs;
    bool consumoEc24hHungerPending;
    bool consumoEc24hDilutePending;
    unsigned long lastECCheckAtMs;
    float ecAtLastSequenceStart;
    float ecSetpointAtLastSequence;
    float lastSequenceTotalMl;
    bool ecGainLearnPending;
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
    float phPulseMl;
    float phPulseGapSec;
    float phPulseRemainingMl;
    float phPulseChunkMl;
    unsigned long phPulseOnDurationMs;
    int phPulseIndex;
    float phGainAlpha;
    int phConsecutiveCorrections;
    unsigned long phRecircSeconds;
    bool consumoPh24hEnabled;
    bool consumoPh24hWindowOpen;
    float consumoPh24hT0;
    unsigned long consumoPh24hStartMs;
    bool consumoPh24hForcePending;
    enum PhAutoState { PH_IDLE, PH_DOSING, PH_PULSE_GAP, PH_RECIRCULATING };
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
    EcGainLearnedCallback ecGainLearnedCallback;
    void* ecGainLearnedCallbackUserData;

    // Diluição EC modo A
    DilutionState dilutionState;
    bool dilutionAutoEnabled;
    int dilutionDrainRelay;
    int dilutionFillRelay;
    String dilutionDrainSlaveMac;
    String dilutionFillSlaveMac;
    float dilutionMaxVolumeL;
    float dilutionFillFlowLps;
    float dilutionTargetL;
    float dilutionProgressL;
    float dilutionDrainMeasuredL;
    float dilutionProgressPublishedL;  // último progress enviado a telemetría
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
    DilutionSlaveRelayCallback dilutionSlaveRelayCallback;
    void* dilutionSlaveRelayCallbackUserData;
    volatile uint32_t dilutionPendingCommandId;
    volatile int8_t dilutionAckResult;  // 0=wait, 1=ok, -1=fail
    int dilutionPendingRelay;
    bool dilutionPendingExpectOn;
    unsigned long dilutionAckDeadlineMs;
    
    // ✅ TEMPO MORTO (recirculação) - Aguardar após dosagem antes de medir EC novamente
    unsigned long lastDosageCompleteTime;  // Timestamp da última dosagem completa
    int tankProcedureHoldCount;  // P1: nº de procedimientos tanque activos (sin timer)
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
        float flowRate;    // Vazão desta bomba (ml/s); 0 = não calibrada
        float proportion;  // Proporção calculada (0.0 - 1.0)
        bool active;   // Se está ativo
    };
    NutrientProportion dynamicProportions[16];  // Máximo 16 nutrientes (um por relé)
    int activeNutrientsCount;
    float totalMlPerLiter;  // Soma total de mlPerLiter
    
    // Funções internas
    void updateSensors();
    /** Poll L1-L4 cada LEVEL_POLL_MS (antes de Modbus/EC). */
    void pollDiscreteLevels();
    void refreshTankLevelOkFromAggregate();
    void loadLevelInterlockModeFromNVS();
    void updateDisplay();
    void checkRelayTimers();
    void checkAutoEC();
    void checkAutoPH();  // ✅ Verificar e ajustar pH automaticamente
    void tickConsumoEcWindow();
    void tickConsumoPhWindow();
    void processPhAutoState();
    void processSimpleSequential();  // ✅ Máquina de estados para dosagem sequencial
    void beginEcNutrientPulses();
    bool startEcPulseOn();
    void advanceAfterEcNutrientComplete(unsigned long now);
    void beginPhDosePulses(float flowMlPerS);
    bool startPhPulseOn(float flowMlPerS);
    void processDilution();
    uint32_t setDilutionRelay(int relayIndex, bool on);
    void beginDilutionValveWait(uint32_t commandId, int relay, bool expectOn);
    void clearDilutionValveWait();
    void confirmDilutionValveAck();
    bool processDilutionValveWait(unsigned long now);
    bool dilutionUsesSlaveDrain() const { return dilutionDrainSlaveMac.length() > 0; }
    bool dilutionUsesSlaveFill() const { return dilutionFillSlaveMac.length() > 0; }
    bool dilutionMacMatches(const uint8_t* mac, const String& configured) const;
    void finishDilutionDrainPhase();
    void finishDilutionSequence(bool success);
    void emitEcDilutionEvent();
    bool isTankHighCapacitive() const;
    void applyFlowCalibrationFromPpl(float ppl);
    void emitNutrientDoseEvent(const SimpleNutrient& nutrient);
    void notifyEcOperationChanged();
    void notifyPhOperationChanged();
    void notifyPhysicalRecirc(bool starting, const char* domain);
    void startPhAutoDosage(int relay, float durationSec, PhCorrectionPath path,
                           float mlApplied, float hBefore, float phBefore);
    void finishPhRecirculation();
    void emitPhDoseEvent();
    void learnEcGainAfterSequence();
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
    float nutrientFlowRateMlPerSec(int idx) const;  // Vazão da bomba (0 = sem calibragem)
    void loadEcCalibrationFromNVS();
    void saveEcCalibrationToNVS();
};

#endif