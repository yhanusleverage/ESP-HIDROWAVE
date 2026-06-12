#ifndef HYDRO_CONTROL_H
#define HYDRO_CONTROL_H

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PCF8574.h>
#include "Config.h"
#include "PHSensor.h"
#include "TDSReaderSerial.h"
#include "LevelSensor.h"
#include "Controller.h"  // ✅ Controller KP para controle automático de EC
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
    
    // Getters para leituras dos sensores
    float& getTemperature() { return temperature; }
    const float& getTemperature() const { return temperature; }
    float& getpH() { return pH; }
    const float& getpH() const { return pH; }
    float& getTDS() { return tds; }
    const float& getTDS() const { return tds; }
    float& getEC() { return ec; }
    const float& getEC() const { return ec; }
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

    /** Estado operacional Auto EC para UI (relay_master.ec_operation_*). */
    const char* getEcOperationStateName() const;
    int getEcOperationRemainingSec() const;
    int getEcNextCheckInSec() const;

    // ✅ Auto pH
    PHController& getPHController() { return phController; }
    void setPHSetpoint(float setpoint, bool saveToNVS = true);
    float getPHSetpoint() const { return phSetpoint; }
    void setPHTolerance(float tolerance) { phTolerance = tolerance; }
    float getPHTolerance() const { return phTolerance; }
    void setAutoPHEnabled(bool enabled, bool saveToNVS = true);
    bool isAutoPHEnabled() const { return autoPHEnabled; }
    void setAutoPHInterval(int intervalSeconds, bool saveToNVS = true);
    int getAutoPHInterval() const { return autoPHIntervalSeconds; }
    void setPhPumpConfig(int relayUp, int relayDown, float flowUp, float flowDown, float mlPerUnit);
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
    void saveTDSCalibration();  // Salvar calibração TDS no NVS
    
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
    phSensor* pHSensor;
    TDSReaderSerial* tdsSensor;
    LevelSensor* tankSensor;
    
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
    bool ecValid; // ✅ Flag para indicar se EC é confiável (buffer TDS cheio)
    
    // Estado dos relés
    bool relayStates[NUM_RELAYS];
    unsigned long startTimes[NUM_RELAYS];
    int timerSeconds[NUM_RELAYS];
    
    // ✅ Controller KP e controle automático de EC
    ECController ecController;
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

    // ✅ Auto pH
    PHController phController;
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
    float mlPerPhUnit;
    unsigned long phRecircSeconds;
    enum PhAutoState { PH_IDLE, PH_DOSING, PH_RECIRCULATING };
    PhAutoState phAutoState;
    unsigned long phStateStartMs;
    int phActiveRelay;
    PhOperationSyncCallback phOperationSyncCallback;
    void* phOperationSyncCallbackUserData;
    
    // ✅ TEMPO MORTO (recirculação) - Aguardar após dosagem antes de medir EC novamente
    unsigned long lastDosageCompleteTime;  // Timestamp da última dosagem completa
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
    void checkAutoEC();  // ✅ Verificar e ajustar EC automaticamente
    void checkAutoPH();  // ✅ Verificar e ajustar pH automaticamente
    void processPhAutoState();
    void processSimpleSequential();  // ✅ Máquina de estados para dosagem sequencial
    void emitNutrientDoseEvent(const SimpleNutrient& nutrient);
    void notifyEcOperationChanged();
    void notifyPhOperationChanged();
    void startPhAutoDosage(int relay, float durationSec);
    int computeEcOperationRemainingSec() const;
    
    // ✅ Persistência em NVS (privadas - carregamento automático)
    void loadECControllerConfig();  // Carregar configuração do Controller ao iniciar
    void loadNutrientProportions();  // Carregar proporções nutricionais ao iniciar
};

#endif