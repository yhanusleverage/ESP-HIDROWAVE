#include "HydroControl.h"
#include "PreferencesManager.h"  // ✅ Para persistência em NVS
#include "StatePersistenceManager.h"
#include "SensorSanitize.h"
#include <Preferences.h>
#include <cmath>
#include <cstring>

HydroControl::HydroControl()
    : lcd(0x27, 16, 2)
    , oneWire(TEMP_PIN)
    , sensors(&oneWire)
    , pcf1(0x20)  // Primeiro PCF8574
    , pcf2(0x24)  // Segundo PCF8574
    , pcf1_ok(false)
    , pcf2_ok(false)
    , ecController()  // ✅ Inicializar Controller KP
{
    // Inicializa os estados dos relés
    for(int i = 0; i < NUM_RELAYS; i++) {
        relayStates[i] = false;
        startTimes[i] = 0;
        timerSeconds[i] = 0;
    }
#if !HIDRO_SIMULATE_WATER_LEVELS
    tankSensor = new LevelSensor(TANK_LOW_PIN, TANK_HIGH_PIN);
#else
    tankSensor = nullptr;
    tankLevelOk = true;
#endif
    
    // ✅ Inicializar controle automático de EC
    ecSetpoint = 0.0;
    ecTolerance = 50.0f;
    autoECEnabled = false;
    lastECCheck = 0;
    lastECCheckAtMs = 0;
    autoECIntervalSeconds = 30;  // Padrão: 30 segundos
    tempoRecirculacaoSeconds = 60;
    ecAtLastSequenceStart = 0.0f;
    ecSetpointAtLastSequence = 0.0f;
    currentDoseSource = "auto_ec";
    nutrientDoseCallback = nullptr;
    nutrientDoseCallbackUserData = nullptr;
    ecOperationSyncCallback = nullptr;
    ecOperationSyncCallbackUserData = nullptr;
    physicalRecircCallback = nullptr;
    physicalRecircCallbackUserData = nullptr;
    ecMetricCallback = nullptr;
    ecMetricCallbackUserData = nullptr;

    phSetpoint = 6.0f;
    phTolerance = 0.2f;
    autoPHEnabled = false;
    lastPHCheck = 0;
    lastPHCheckAtMs = 0;
    autoPHIntervalSeconds = 300;
    relayPhUp = 1;
    relayPhDown = 0;
    flowRatePhUp = 1.0f;
    flowRatePhDown = 1.0f;
    mlPerPhUnitAcid = 2.0f;
    mlPerPhUnitBase = 2.0f;
    phAggressiveness = 0.5f;
    phGainAlpha = 0.2f;
    phMaxDoseMl = 50.0f;
    phMaxPulseSec = 120;
    phMaxConsecutive = 5;
    phConsecutiveCorrections = 0;
    phRecircSeconds = 60;
    phAutoState = PH_IDLE;
    phStateStartMs = 0;
    phActiveRelay = -1;
    phActivePath = PH_PATH_NONE;
    phCycleHBefore = 0.0f;
    phCyclePhBefore = 0.0f;
    phCycleMlApplied = 0.0f;
    phCycleDurationSec = 0.0f;
    phCycleDurationMs = 0;
    phOperationSyncCallback = nullptr;
    phOperationSyncCallbackUserData = nullptr;
    phDoseCallback = nullptr;
    phDoseCallbackUserData = nullptr;
    phMetricCallback = nullptr;
    phMetricCallbackUserData = nullptr;
    phGainLearnedCallback = nullptr;
    phGainLearnedCallbackUserData = nullptr;
    tankScriptHoldUntilMs = 0;
#if USE_PH_MODBUS_SENSOR
    phModbusSensor = nullptr;
#else
    pHSensor = nullptr;
#endif
    ecSensor = nullptr;
    tankSensor = nullptr;
    temperature = NAN;
    pH = NAN;
    tds = NAN;
    ec = NAN;
    ecValid = false;
    phValid = false;
    tempValid = false;
    lastPhValidMs = 0;
    lastEcValidMs = 0;
    lastTempValidMs = 0;
    
    // ✅ Inicializar sistema sequencial de dosagem
    currentState = IDLE;
    totalNutrients = 0;
    currentNutrientIndex = 0;
    stateStartTime = 0;
    intervalSeconds = 3;  // Padrão: 3 segundos entre nutrientes
    
    // ✅ Inicializar proporções dinâmicas
    activeNutrientsCount = 0;
    totalMlPerLiter = 0.0;
    for (int i = 0; i < 16; i++) {
        dynamicProportions[i].name = "";
        dynamicProportions[i].relay = i;
        dynamicProportions[i].mlPerLiter = 0.0;
        dynamicProportions[i].proportion = 0.0;
        dynamicProportions[i].active = false;
    }

    flowmeterSensor = nullptr;
    dilutionState = DILUTION_IDLE;
    dilutionAutoEnabled = false;
    dilutionDrainRelay = DILUTION_DRAIN_RELAY_DEFAULT;
    dilutionFillRelay = DILUTION_FILL_RELAY_DEFAULT;
    dilutionMaxVolumeL = DILUTION_MAX_VOLUME_L_DEFAULT;
    dilutionFillFlowLps = DILUTION_FILL_FLOW_LPS;
    dilutionTargetL = 0.0f;
    dilutionProgressL = 0.0f;
    dilutionDrainMeasuredL = 0.0f;
    dilutionStateStartMs = 0;
    dilutionDrainStartMs = 0;
    dilutionFillStartMs = 0;
    dilutionFillDurationMs = 0;
    dilutionLastPulseMs = 0;
    dilutionLastPulseCount = 0;
    dilutionSource = "manual";
    dilutionEcBefore = NAN;
    ecDilutionCallback = nullptr;
    ecDilutionCallbackUserData = nullptr;
}

/**
 * @brief Inicializa o HydroControl e todos os seus componentes
 * 
 * ⚠️ ORDEM PROCEDURAL CRÍTICA (NÃO PODE SER ALTERADA):
 * 
 * 1. Wire.begin() - OBRIGATÓRIO PRIMEIRO (inicializa barramento I2C)
 * 2. Escanear dispositivos I2C (debug)
 * 3. lcd.begin(16, 2) - LCD I2C
 * 4. oneWire.begin(TEMP_PIN) - Barramento OneWire para sensores de temperatura
 * 5. sensors.begin() - Sensores DS18B20
 * 6. pHSensor = new phSensor() - Sensor de pH
 * 7. ecSensor = new EcAnalogSensor() - Sensor EC analógico
 * 8. tankSensor = new LevelSensor() - Sensor de nível do tanque
 * 9. delay(100) - Estabilizar barramento I2C
 * 10. pcf1.begin(false) - PCF8574 #1 (Relés 1-7) - false = não reiniciar I2C
 * 11. pcf2.begin(false) - PCF8574 #2 (Relé 8) - false = não reiniciar I2C
 * 
 * ⚠️ DEPENDÊNCIAS CRÍTICAS:
 * - Wire.begin() DEVE ser chamado PRIMEIRO
 * - pcf1.begin(false) e pcf2.begin(false) usam false para não reiniciar I2C
 * - Se PCF8574 falhar, relés não funcionarão
 * 
 * @return true se inicialização bem-sucedida, false caso contrário
 */
bool HydroControl::begin() {
    // ⚠️ PASSO 1: Inicializar I2C uma única vez - OBRIGATÓRIO PRIMEIRO
    // Por quê: Todos os dispositivos I2C (LCD, PCF8574) dependem do barramento I2C
    // ✅ CORREÇÃO CRÍTICA: Verificar se I2C já está iniciado antes de chamar Wire.begin()
    // Isso evita o erro "Bus already started in Master Mode"
    // ✅ PADRÃO ESP-NOW MASTERTASK: Usar static bool para garantir inicialização única
    static bool i2cInitialized = false;
    
    if (!i2cInitialized) {
        Wire.begin();
        i2cInitialized = true;
        Serial.println("🔌 I2C inicializado (primeira vez)");
        delay(50); // Pequena pausa para estabilizar
    } else {
        Serial.println("ℹ️ I2C já estava inicializado (reutilizando)");
    }
    
    Serial.println("🔍 Escaneando dispositivos I2C...");
    
    // Scan I2C antes de inicializar os dispositivos
    for(byte i = 8; i < 120; i++) {
        Wire.beginTransmission(i);
        if (Wire.endTransmission() == 0) {
            Serial.printf("✅ Dispositivo I2C encontrado no endereço 0x%02X\n", i);
        }
    }

    // Inicializar LCD sem reiniciar I2C
    // ✅ CORREÇÃO: LCD pode tentar inicializar I2C internamente, mas já está iniciado
    lcd.begin(16, 2);  // Removido lcd.init() para evitar dupla inicialização
    lcd.backlight();
    lcd.print("Iniciando...");

    // Inicializar sensores
    oneWire.begin(TEMP_PIN);
    sensors.begin();

#if USE_PH_MODBUS_SENSOR
    phModbusSensor = new PhModbusSensor(PH_RS485_RX_PIN,
                                        PH_RS485_TX_PIN,
                                        PH_RS485_DE_RE_PIN,
                                        PH_MODBUS_BAUD,
                                        PH_MODBUS_ADDR,
                                        PH_MODBUS_REG,
                                        PH_MODBUS_SCALE);
    phModbusSensor->begin();
    Serial.println("[pH] Modbus RS485 iniciado (RO=34 DI=23 DE/RE=32)");
#else
    pHSensor = new phSensor();
    pHSensor->calibrate(2.56, 3.3, 2.05, false);
    pinMode(PH_PIN, INPUT);
    ESP32_ADC_CONFIGURE_PIN(PH_PIN);
#endif

    // Inicializar sensor EC
    ESP32_ADC_CONFIGURE_PIN(EC_SENSOR_ANALOG_PIN);
    ecSensor = new EcAnalogSensor(EC_SENSOR_ANALOG_PIN, TDS_VREF, TDS_CALIBRATION_FACTOR);
    ecSensor->begin();
    loadEcCalibrationFromNVS();

    flowmeterSensor = new FlowmeterSensor(FLOWMETER_PULSE_PIN, FLOWMETER_PULSES_PER_LITER);
    flowmeterSensor->begin();
    Serial.printf("[DILUTION] Fluxometro GPIO%d %.0f pulsos/L\n",
                  FLOWMETER_PULSE_PIN, FLOWMETER_PULSES_PER_LITER);

#if !HIDRO_SIMULATE_WATER_LEVELS
    if (tankSensor == nullptr) {
        tankSensor = new LevelSensor(TANK_LOW_PIN, TANK_HIGH_PIN);
    }
    tankSensor->begin();
#else
    Serial.println("[LEVEL] HIDRO_SIMULATE_WATER_LEVELS=1 — L1-L4 simulados ON");
#endif
    levelBank.begin();

    // Pequena pausa para estabilizar o barramento I2C
    delay(100);

    // Inicializar PCF8574s com tratamento de erro
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║   🔧 INICIALIZANDO PCF8574                          ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    
    // ✅ CORREÇÃO: Verificar se PCF8574 está presente antes de inicializar
    Wire.beginTransmission(0x20);
    bool pcf1Present = (Wire.endTransmission() == 0);
    
    Wire.beginTransmission(0x24);
    bool pcf2Present = (Wire.endTransmission() == 0);
    
    // ✅ PCF1 (0x20) - Sensores capacitivos (ENTRADAS)
    if (pcf1Present) {
        pcf1_ok = pcf1.begin(false);  // false para não reiniciar I2C
        if (!pcf1_ok) {
            Serial.println("❌ [PCF1] Falha ao inicializar PCF8574 #1 (0x20)");
            Serial.println("   ⚠️ Verifique conexões I2C e endereço");
        } else {
            Serial.println("✅ [PCF1] PCF8574 #1 inicializado (0x20) - Sensores capacitivos");
            // Configurar pinos como entrada (ao ler, configura automaticamente)
            for (int i = 0; i < 8; i++) {
                pcf1.read(i);  // Configurar como entrada
            }
            Serial.println("   📥 P0-P7 configurados como ENTRADAS (sensores)");
        }
    } else {
        Serial.println("⚠️ [PCF1] PCF8574 #1 (0x20) não detectado no barramento I2C");
        pcf1_ok = false;
    }
    
    // ✅ PCF2 (0x24) - Relés peristálticos (SAÍDAS) - TODOS os relés (0-7)
    if (pcf2Present) {
        pcf2_ok = pcf2.begin(false);  // false para não reiniciar I2C
        if (!pcf2_ok) {
            Serial.println("❌ [PCF2] Falha ao inicializar PCF8574 #2 (0x24)");
            Serial.println("   ⚠️ Verifique conexões I2C e endereço");
        } else {
            Serial.println("✅ [PCF2] PCF8574 #2 inicializado (0x24) - Relés peristálticos");
            // Inicializar todos os relés em HIGH (desligados)
            for (int i = 0; i < 8; i++) {
                relayStates[i] = false;
                startTimes[i] = 0;
                timerSeconds[i] = 0;
                pcf2.write(i, HIGH);  // HIGH = relé desligado
            }
            Serial.println("   📤 P0-P7 configurados como SAÍDAS (relés) - Todos DESLIGADOS");
        }
    } else {
        Serial.println("⚠️ [PCF2] PCF8574 #2 (0x24) não detectado no barramento I2C");
        pcf2_ok = false;
    }
    
    Serial.println("✅ [PCF8574] Inicialização completa");
    Serial.println("╚════════════════════════════════════════════════════╝\n");

    // Resetar estados dos relés
    for (int i = 0; i < NUM_RELAYS; i++) {
        relayStates[i] = false;
        startTimes[i] = 0;
        timerSeconds[i] = 0;
    }

    Serial.println("\n🚀 Sistema iniciado" + 
                  String(!pcf1_ok || !pcf2_ok ? " com avisos" : " sem erros"));
    
    // ✅ Carregar configuração persistida do Controller KP
    loadECControllerConfig();
    loadNutrientProportions();
    loadPHControllerConfig();
    adaptivePhController.loadFromNVS();
    if (adaptivePhController.getValidLearningCycles() == 0) {
        adaptivePhController.setSeedFromMlPerPhUnit(phSetpoint, mlPerPhUnitAcid, mlPerPhUnitBase);
    }
    
    // Return true if basic initialization succeeded (even with PCF errors)
    return true;
}

void HydroControl::loop() {
    // Call the existing update method
    update();
}

void HydroControl::update() {
    updateSensors();
    if (flowmeterSensor) {
        flowmeterSensor->tick();
    }
    updateDisplay();
    checkRelayTimers();
    processPhAutoState();
    processDilution();
    checkAutoEC();
    checkAutoPH();
    processSimpleSequential();
}

bool HydroControl::isPhValidForTelemetry() const {
    if (!phValid || !isfinite(pH)) {
        return false;
    }
    return (millis() - lastPhValidMs) <= SENSOR_READING_STALE_MS;
}

bool HydroControl::isEcValidForTelemetry() const {
    if (!ecValid || !isfinite(ec)) {
        return false;
    }
    return (millis() - lastEcValidMs) <= SENSOR_READING_STALE_MS;
}

bool HydroControl::isTempValidForTelemetry() const {
    if (!tempValid || !isfinite(temperature) || temperature <= 0.0f) {
        return false;
    }
    return (millis() - lastTempValidMs) <= SENSOR_READING_STALE_MS;
}

void HydroControl::updateSensors() {
    static unsigned long lastSensorDebugLog = 0;
    const unsigned long nowMs = millis();
    sensorsOk = true;

    auto readDs18b20Fallback = [this, nowMs]() {
        sensors.requestTemperatures();
        const float tempReading = sensors.getTempCByIndex(0);
        if (tempReading != -127.0f && isValidWaterTempReading(tempReading) && tempReading > 0.0f) {
            temperature = tempReading;
            tempValid = true;
            lastTempValidMs = nowMs;
        } else if (!tempValid) {
            tempValid = false;
            temperature = NAN;
            sensorsOk = false;
        }
    };

#if !USE_PH_MODBUS_SENSOR
    readDs18b20Fallback();
    float phReading = pHSensor->readPH(PH_PIN);
    const uint32_t phPinMv = analogReadMilliVolts(PH_PIN);
    if (isfinite(phReading) && isValidPhReading(phReading)) {
        pH = phReading;
        phValid = true;
        lastPhValidMs = nowMs;
    } else {
        phValid = false;
        pH = NAN;
        sensorsOk = false;
    }
#else
    const uint32_t phPinMv = 0;
    if (!tempValid) {
        readDs18b20Fallback();
    }
#endif

    if (ecSensor) {
        const float tempForEc = isfinite(temperature) ? temperature : 25.0f;
        ecSensor->updateLiquidTemperatureC(tempForEc);
        ecSensor->tick();

        if (ecSensor->consumeWindowReady()) {
#if USE_PH_MODBUS_SENSOR
            if (phModbusSensor) {
                const float phReading = phModbusSensor->readPH();
                const float modbusTempC = phModbusSensor->lastTempC();
                if (isfinite(phReading) && isValidPhReading(phReading)) {
                    pH = phReading;
                    phValid = true;
                    lastPhValidMs = nowMs;
                } else {
                    phValid = false;
                    pH = NAN;
                    sensorsOk = false;
                }
                if (isfinite(modbusTempC) && isValidWaterTempReading(modbusTempC) && modbusTempC > 0.0f) {
                    temperature = modbusTempC;
                    tempValid = true;
                    lastTempValidMs = nowMs;
                } else {
                    readDs18b20Fallback();
                }
            }
#endif
            const float ecReading = ecSensor->ecMeanMicrosiemensPerCm();
            const float pinMv = ecSensor->lastPinVolts() * 1000.0f;
            bool windowEcValid = false;
            if (isfinite(ecReading) && ecReading >= MIN_EC && ecReading <= MAX_EC &&
                isValidEcMicroSiemens(ecReading)) {
                ec = ecReading;
                ecValid = true;
                windowEcValid = true;
                lastEcValidMs = nowMs;
            } else {
                ecValid = false;
                ec = NAN;
                sensorsOk = false;
            }

#if USE_PH_MODBUS_SENSOR && HIDRO_EC_REQUIRES_PH_MODBUS
            if (!phValid) {
                if (windowEcValid) {
                    static unsigned long lastEcCouplingLogMs = 0;
                    if (nowMs - lastEcCouplingLogMs >= 30000UL) {
                        lastEcCouplingLogMs = nowMs;
                        Serial.println("[EC] omitida — pH Modbus inválido (HIDRO_EC_REQUIRES_PH_MODBUS)");
                    }
                }
                ecValid = false;
                ec = NAN;
                windowEcValid = false;
                sensorsOk = false;
            }
#endif

            static float lastLogMv = -1.0f;
            static float lastLogEc = -1.0f;
            static unsigned long lastEcWindowLogMs = 0;
            const bool mvChanged = lastLogMv < 0.0f ||
                fabsf(pinMv - lastLogMv) > fmaxf(50.0f, lastLogMv * 0.05f);
            const bool ecChanged = lastLogEc < 0.0f ||
                (isfinite(ecReading) && lastLogEc >= 0.0f &&
                 fabsf(ecReading - lastLogEc) > fmaxf(50.0f, lastLogEc * 0.05f));
            const bool timeDue = (nowMs - lastEcWindowLogMs) >= 30000UL;
            if (mvChanged || ecChanged || timeDue) {
                lastEcWindowLogMs = nowMs;
                lastLogMv = pinMv;
                lastLogEc = isfinite(ecReading) ? ecReading : -1.0f;
                Serial.printf("[EC WINDOW] mV=%.0f ec=%.0f valid=%d\n",
                    pinMv,
                    isfinite(ecReading) ? ecReading : -1.0f,
                    windowEcValid ? 1 : 0);
            }
        }
    } else {
        ecValid = false;
        ec = NAN;
        sensorsOk = false;
    }

    if (nowMs - lastSensorDebugLog >= 60000UL) {
        lastSensorDebugLog = nowMs;
        Serial.print("[SENSORES] pH=");
        if (isfinite(pH)) {
            Serial.printf("%.2f", pH);
        } else {
            Serial.print("--");
        }
#if USE_PH_MODBUS_SENSOR
        Serial.printf(" valid=%d | EC=", phValid ? 1 : 0);
#else
        Serial.printf(" valid=%d mV=%u | EC=", phValid ? 1 : 0, static_cast<unsigned>(phPinMv));
#endif
        if (isfinite(ec)) {
            Serial.printf("%.0f", ec);
        } else {
            Serial.print("--");
        }
        Serial.printf(" valid=%d | Temp=", ecValid ? 1 : 0);
        if (isfinite(temperature)) {
            Serial.printf("%.1f", temperature);
        } else {
            Serial.print("--");
        }
        Serial.printf(" valid=%d\n", tempValid ? 1 : 0);
    }

#if HIDRO_SIMULATE_WATER_LEVELS
    tankLevelOk = true;
#else
    if (levelBank.poll(pcf1, pcf1_ok)) {
        tankLevelOk = levelBank.isLevelOk();
        static unsigned long lastLevelLog = 0;
        if (millis() - lastLevelLog >= 10000) {
            lastLevelLog = millis();
            Serial.printf("LEVEL L1=%s L2=%s L3=%s L4=%s → %s\n",
                levelBank.isWet(1) ? "MOJADO" : "SECO",
                levelBank.isWet(2) ? "MOJADO" : "SECO",
                levelBank.isWet(3) ? "MOJADO" : "SECO",
                levelBank.isWet(4) ? "MOJADO" : "SECO",
                levelBank.getWaterLevel());
        }
    } else if (tankSensor) {
        tankLevelOk = tankSensor->checkWaterLevel();
    } else {
        tankLevelOk = false;
    }
#endif
}

bool HydroControl::isLevelWet(int levelIndex) const {
#if HIDRO_SIMULATE_WATER_LEVELS
    if (levelIndex >= 1 && levelIndex <= 4) {
        return true;
    }
    return false;
#else
    if (levelBank.isAvailable()) {
        return levelBank.isWet(levelIndex);
    }
    if (!tankSensor || levelIndex < 1 || levelIndex > 4) {
        return false;
    }
    const String status = tankSensor->getStatus();
    if (levelIndex == 4) {
        return status != "BAIXO" && status != "ERRO";
    }
    if (levelIndex == 1) {
        return status == "CHEIO";
    }
    return status == "MÉDIO" || status == "CHEIO";
#endif
}

const char* HydroControl::getWaterLevelAggregate() const {
#if HIDRO_SIMULATE_WATER_LEVELS
    return "alto";
#else
    if (levelBank.isAvailable()) {
        return levelBank.getWaterLevel();
    }
    if (!tankSensor) {
        return "vazio";
    }
    const String status = tankSensor->getStatus();
    if (status == "CHEIO") return "alto";
    if (status == "MÉDIO") return "medio";
    if (status == "BAIXO") return "baixo";
    return "vazio";
#endif
}

void HydroControl::updateDisplay() {
    lcd.clear();

    String tempText = "Temp:";
    if (tempValid && isfinite(temperature)) {
        tempText += String(temperature, 1) + char(223) + "C";
    } else {
        tempText += "--";
    }
    lcd.setCursor((16 - tempText.length()) / 2, 0);
    lcd.print(tempText);

    lcd.setCursor(0, 1);
    lcd.print("pH:");
    if (phValid && isfinite(pH)) {
        lcd.print(pH, 2);
    } else {
        lcd.print("--");
    }

    String ecText = "EC:";
    if (ecValid && isfinite(ec)) {
        ecText += String(ec, 0);
    } else {
        ecText += "--";
    }
    lcd.setCursor(16 - ecText.length(), 1);
    lcd.print(ecText);
}

void HydroControl::showMessage(String msg) {
    lcd.clear();
    lcd.print(msg);
}

// ✅ NOVO: Define o estado do relé diretamente
void HydroControl::setRelay(int relay, bool desiredState, int seconds) {
    // ✅ Validação: Relés 0-7 (todos no PCF2)
    if (relay < 0 || relay >= 8) {
        Serial.printf("❌ [RELAY] Índice inválido: %d (deve ser 0-7)\n", relay);
        return;
    }
    
    // ✅ Verificar se PCF2 está disponível
    if (!pcf2_ok) {
        Serial.printf("❌ [RELAY] PCF8574 #2 (0x24) não conectado!\n");
        return;
    }
    
    // Se já está no estado desejado, não fazer nada (exceto se tiver timer)
    if (relayStates[relay] == desiredState && seconds == 0) {
        Serial.printf("ℹ️ [RELAY] Relé %d já está %s\n", relay + 1, desiredState ? "ON" : "OFF");
        return;
    }
    
    // Atualizar estado do relé
    relayStates[relay] = desiredState;
    bool physicalState = !desiredState;  // Invertido: LOW = ligado, HIGH = desligado
    
    // ✅ TODOS os relés estão no PCF2 (0x24) - mapeamento direto
    pcf2.write(relay, physicalState);
    
    const char* relayNames[] = {
        "Bomba pH-", "Bomba pH+", "Bomba A (Grow)", "Bomba B (Micro)",
        "Bomba C (Bloom)", "Bomba CalMag", "Luz UV", "Aerador"
    };
    
    Serial.printf("🔌 [RELAY %d] %s → %s", 
        relay + 1,
        relayNames[relay],
        desiredState ? "LIGADO" : "DESLIGADO"
    );
    
    // Configurar timer se necessário
    if (seconds > 0 && relayStates[relay]) {
        startTimes[relay] = millis();
        timerSeconds[relay] = seconds;
        Serial.printf(" (timer: %d segundos)", seconds);
    }
    
    Serial.println();
}

void HydroControl::toggleRelay(int relay, int seconds) {
    // ✅ Validação: Relés 0-7 (todos no PCF2)
    if (relay < 0 || relay >= 8) {
        Serial.printf("❌ [RELAY] Índice inválido: %d (deve ser 0-7)\n", relay);
        return;
    }
    
    // ✅ Verificar se PCF2 está disponível
    if (!pcf2_ok) {
        Serial.printf("❌ [RELAY] PCF8574 #2 (0x24) não conectado!\n");
        return;
    }
    
    // Inverter estado
    relayStates[relay] = !relayStates[relay];
    
    // Lógica invertida: LOW = ligado, HIGH = desligado
    bool pcfState = relayStates[relay] ? LOW : HIGH;
    
    // ✅ TODOS os relés estão no PCF2 (0x24) - mapeamento direto
    pcf2.write(relay, pcfState);
    
    const char* relayNames[] = {
        "Bomba pH-", "Bomba pH+", "Bomba A (Grow)", "Bomba B (Micro)",
        "Bomba C (Bloom)", "Bomba CalMag", "Luz UV", "Aerador"
    };
    
    Serial.printf("🔌 [RELAY %d] %s → %s", 
        relay + 1,
        relayNames[relay],
        relayStates[relay] ? "LIGADO" : "DESLIGADO"
    );
    
    // Configurar timer se necessário
    if (seconds > 0 && relayStates[relay]) {
        startTimes[relay] = millis();
        timerSeconds[relay] = seconds;
        Serial.printf(" (timer: %d segundos)", timerSeconds[relay]);
    } else {
        startTimes[relay] = 0;
        timerSeconds[relay] = 0;
    }
    
    Serial.println();
}

void HydroControl::checkRelayTimers() {
    unsigned long currentMillis = millis();
    
    for(int i = 0; i < 8; i++) {  // ✅ Apenas 8 relés (0-7) no PCF2
        if(relayStates[i] && timerSeconds[i] > 0) {
            if((currentMillis - startTimes[i]) / 1000 >= timerSeconds[i]) {
                relayStates[i] = false;
                bool state = !relayStates[i];  // Invertido: LOW = ligado, HIGH = desligado
                
                // ✅ TODOS os relés estão no PCF2 (0x24) - mapeamento direto
                pcf2.write(i, state);
                Serial.printf("⏰ [RELAY %d] Timer expirado - desligando automaticamente\n", i + 1);
                
                timerSeconds[i] = 0;
                startTimes[i] = 0;
            }
        }
    }
}

void HydroControl::updateSensorData(float temp, float humidity, float ph, float ecUsCm) {
    temperature = temp;
    pH = ph;
    ec = ecUsCm;
    ecValid = isfinite(ecUsCm) && isValidEcMicroSiemens(ecUsCm);
    
    updateDisplay();
}

void HydroControl::updateRelayTimers() {
    checkRelayTimers();
}

String HydroControl::getTankStatus() {
    if (!tankSensor) {
#if HIDRO_SIMULATE_WATER_LEVELS
        return "CHEIO";
#else
        return "ERRO";
#endif
    }
    return tankSensor->getStatus();
}

float HydroControl::getWaterTemp() {
    return temperature;
}

bool HydroControl::isAutoDosingPausedByInterlock() const {
#if !HIDRO_SIMULATE_WATER_LEVELS
    if (!tankLevelOk) {
        return true;
    }
#endif
    const unsigned long now = millis();
    return tankScriptHoldUntilMs > 0 && now < tankScriptHoldUntilMs;
}

void HydroControl::holdAutoDosingForTankScript(unsigned long durationMs) {
    if (durationMs < TANK_SCRIPT_HOLD_MIN_MS) {
        durationMs = TANK_SCRIPT_HOLD_MIN_MS;
    }
    const unsigned long until = millis() + durationMs;
    if (until > tankScriptHoldUntilMs) {
        tankScriptHoldUntilMs = until;
        Serial.printf(
            "🔒 [INTERLOCK P1] Auto EC/pH pausados ~%lu s (script tanque priority >= %d)\n",
            durationMs / 1000UL,
            TANK_SCRIPT_PRIORITY_THRESHOLD
        );
    }
}

// ✅ Função para verificar e ajustar EC automaticamente
void HydroControl::checkAutoEC() {
    if (!autoECEnabled && !dilutionAutoEnabled) {
        static unsigned long lastDebugPrint = 0;
        unsigned long now = millis();
        if (now - lastDebugPrint >= 30000) {
            lastDebugPrint = now;
            Serial.println("⚠️ [AUTO EC] auto_enabled e dilution_auto desativados");
        }
        return;
    }

    if (!autoECEnabled) {
        static unsigned long lastDebugPrint = 0;
        const unsigned long now = millis();
        if (now - lastDebugPrint >= 60000UL) {
            lastDebugPrint = now;
            Serial.println("ℹ️ [AUTO EC] nutrientes off — só diluição se dilution_auto=1");
        }
    } else {
        static unsigned long lastDebugPrint = 0;
        unsigned long now = millis();
        if (now - lastDebugPrint >= 30000 && !dilutionAutoEnabled) {
            lastDebugPrint = now;
            Serial.printf("   💡 Valores: setpoint=%.0f ec=%.0f base_dose=%.2f\n",
                ecSetpoint, ec, ecController.getBaseDose());
        }
    }

    if (isAutoDosingPausedByInterlock()) {
        static unsigned long lastEcInterlockLog = 0;
        const unsigned long nowMs = millis();
        if (nowMs - lastEcInterlockLog >= 60000UL) {
            lastEcInterlockLog = nowMs;
            if (!tankLevelOk) {
                Serial.println("⚠️ [AUTO EC] Pausado — nível de água baixo (water_level_ok=false)");
            } else {
                Serial.println("⚠️ [AUTO EC] Pausado — script tanque P1 activo");
            }
        }
        return;
    }

    if (currentState != IDLE) {
        return;
    }

    if (dilutionState != DILUTION_IDLE) {
        return;
    }

    if (!ecValid) {
        static unsigned long lastEcNotReadyLog = 0;
        const unsigned long nowMs = millis();
        if (nowMs - lastEcNotReadyLog >= 60000UL) {
            lastEcNotReadyLog = nowMs;
            Serial.println("⚠️ [AUTO EC] EC no listo (ventana pendiente o fuera de rango plausible)");
        }
        return;
    }

    // Verificar intervalo de verificação
    unsigned long currentMillis = millis();
    unsigned long checkInterval = autoECIntervalSeconds > 0 ? 
        (autoECIntervalSeconds * 1000UL) : EC_CHECK_INTERVAL;
    
    if (currentMillis - lastECCheck < checkInterval) {
        return;  // Ainda não é hora de verificar
    }
    
    lastECCheck = currentMillis;
    lastECCheckAtMs = currentMillis;

    float ecForControl = ec;
#if !HIDRO_DEV_RELAX_SENSORS
    if (!isValidEcMicroSiemens(ec)) {
        static unsigned long lastInvalidEcLog = 0;
        if (currentMillis - lastInvalidEcLog >= 60000) {
            lastInvalidEcLog = currentMillis;
            Serial.printf("⚠️ [AUTO EC] Lectura EC inválida (%.0f µS/cm) — omitiendo dosaje\n", ec);
        }
        return;
    }
#endif

    if (dilutionAutoEnabled &&
        ecDilutionController.needsDilution(ecSetpoint, ecForControl, ecTolerance)) {
        float volumeL = ecDilutionController.calculateDrainVolumeLiters(
            ecSetpoint, ecForControl, ecController.getVolume());
        if (volumeL >= 0.1f) {
            if (volumeL > dilutionMaxVolumeL) {
                volumeL = dilutionMaxVolumeL;
            }
            Serial.printf("\n🤖 [AUTO DILUTION] overshoot → %.2f L (dreno+reposição)\n", volumeL);
            startEcDilution(volumeL, "auto");
        }
        return;
    }

    if (!autoECEnabled) {
        return;
    }
    
    // Verificar se precisa de ajuste (tolerância configurável — default 50 µS/cm)
    const bool needsAdj = ecController.needsAdjustment(ecSetpoint, ecForControl, ecTolerance);
    const float ecError = ecSetpoint - ec;
    float dosageML = 0.0f;
    float dosageTime = 0.0f;
    bool applied = false;
    String seqForMetric;

    if (needsAdj) {
        dosageML = ecController.calculateDosage(ecSetpoint, ecForControl);

        if (dosageML > 0.1f) {
            dosageTime = ecController.calculateDosageTime(dosageML);

            Serial.println("\n🤖 === CONTROLE AUTOMÁTICO EC ===");
            Serial.printf("📊 EC Atual: %.0f µS/cm\n", ec);
            Serial.printf("🎯 EC Setpoint: %.0f µS/cm\n", ecSetpoint);
            Serial.printf("⚡ Erro: %.0f µS/cm\n", ecError);
            Serial.printf("💧 u(t) calculado: %.3f ml (proporção milimétrica)\n", dosageML);
            Serial.printf("⏱️ Tempo de dosagem: %.2f segundos\n", dosageTime);
            Serial.printf("⏱️  Tempo de dosagem: %.1f segundos\n", dosageTime);
            Serial.println("================================\n");

            if (currentState == IDLE) {
                startSimpleSequentialDosage(dosageML, ecSetpoint, ec);
                applied = true;
                seqForMetric = currentSequenceId;
            } else {
                Serial.println("⚠️  Auto EC: Sistema sequencial já ativo - aguardando conclusão");
            }
        } else {
            Serial.printf("ℹ️  Auto EC: Dosagem muito pequena (%.3f ml) - ignorada\n", dosageML);
        }
    } else {
        static unsigned long lastNoAdjustLog = 0;
        if (currentMillis - lastNoAdjustLog > 60000) {
            lastNoAdjustLog = currentMillis;
            float error = abs(ecSetpoint - ec);
            Serial.printf("✅ Auto EC: Sem ajuste necessário (Erro: %.0f µS/cm, Tolerância: %.0f µS/cm)\n", error, ecTolerance);
        }
    }

    emitEcControllerMetric(needsAdj, applied, dosageML, dosageTime, ecError, seqForMetric);
}

// ✅ Máquina de estados para dosagem sequencial
void HydroControl::processSimpleSequential() {
    if (currentState == IDLE) {
        return;
    }
    
    unsigned long currentTime = millis();
    
    if (currentState == RECIRCULATING) {
        unsigned long elapsedSec = (currentTime - stateStartTime) / 1000UL;
        if (elapsedSec >= tempoRecirculacaoSeconds) {
            notifyPhysicalRecirc(false, "ec");
            Serial.println("✅ [RECIRC] Tempo de recirculação concluído");
            currentState = IDLE;
            totalNutrients = 0;
            currentNutrientIndex = 0;
            currentSequenceId = "";
            showMessage("Recirc OK");
            notifyEcOperationChanged();
        }
        return;
    }
    
    if (currentState == DOSING) {
        // ===== DOSANDO NUTRIENTE ATUAL =====
        SimpleNutrient& current = nutrients[currentNutrientIndex];
        
        // Verificar tempo decorrido
        unsigned long elapsedTime = currentTime - stateStartTime;
        
        // Verificar se terminou a dosagem
        if (elapsedTime >= current.durationMs) {
            // ===== DESLIGAR RELÉ IMEDIATAMENTE =====
            relayStates[current.relay] = false;
            bool state = !relayStates[current.relay];  // Invertido: LOW = ligado, HIGH = desligado
            
            // ✅ TODOS os relés estão no PCF2 (0x24) - mapeamento direto
            if (current.relay >= 0 && current.relay < 8) {
                pcf2.write(current.relay, state);
                Serial.printf("🔴 [DOSAGEM] Relé %d DESLIGADO após %.3fs\n", current.relay + 1, current.durationMs / 1000.0);
            }

            emitNutrientDoseEvent(current);
            
            // ===== PRÓXIMO NUTRIENTE OU INTERVALO =====
            currentNutrientIndex++;
            
            if (currentNutrientIndex >= totalNutrients) {
                Serial.println("✅ SEQUÊNCIA COMPLETA - TODOS OS NUTRIENTES DOSADOS!");
                if (tempoRecirculacaoSeconds > 0) {
                    currentState = RECIRCULATING;
                    stateStartTime = currentTime;
                    Serial.printf("⏳ [RECIRC] Aguardando %lu s (tempo_recirculacao)...\n",
                        tempoRecirculacaoSeconds);
                    showMessage("Recirculando...");
                    notifyPhysicalRecirc(true, "ec");
                    notifyEcOperationChanged();
                } else {
                    currentState = IDLE;
                    totalNutrients = 0;
                    currentNutrientIndex = 0;
                    currentSequenceId = "";
                    showMessage("Sequencia OK!");
                    notifyEcOperationChanged();
                }
            } else {
                // ===== AGUARDAR INTERVALO ANTES DO PRÓXIMO =====
                currentState = WAITING;
                stateStartTime = currentTime;
                Serial.printf("⏳ Aguardando %ds antes do próximo nutriente...\n", intervalSeconds);
                showMessage("Aguardando...");
                notifyEcOperationChanged();
            }
        }
        
    } else if (currentState == WAITING) {
        // ===== AGUARDANDO INTERVALO CONFIGURADO =====
        if (currentTime - stateStartTime >= (intervalSeconds * 1000)) {
            // ===== INICIAR PRÓXIMO NUTRIENTE =====
            SimpleNutrient& next = nutrients[currentNutrientIndex];
            
            Serial.printf("🚀 [DOSAGEM] Iniciando: %s - %.3fml por %.3fs - Relé %d\n", 
                next.name.c_str(), next.dosageML, next.durationMs / 1000.0, next.relay + 1);
            
            // ===== LIGAR RELÉ =====
            relayStates[next.relay] = true;
            bool state = !relayStates[next.relay];  // Invertido: LOW = ligado, HIGH = desligado
            
            // ✅ TODOS os relés estão no PCF2 (0x24) - mapeamento direto
            if (next.relay >= 0 && next.relay < 8) {
                pcf2.write(next.relay, state);
            }
            
            // ===== MUDAR PARA DOSING =====
            currentState = DOSING;
            stateStartTime = currentTime;
            
            String displayMsg = next.name + ": " + String(next.dosageML, 2) + "ml";
            showMessage(displayMsg);
            notifyEcOperationChanged();
        }
    }
}

// ✅ Iniciar dosagem sequencial automática
void HydroControl::startSimpleSequentialDosage(float totalML, float ecSetpoint, float ecActual) {
    // Se já há uma dosagem ativa, não iniciar nova
    if (currentState != IDLE) {
        Serial.println("⚠️  Sistema já ativo - ignorando nova dosagem");
        return;
    }
    
    Serial.println("\n🔄 INICIANDO DOSAGEM SEQUENCIAL AUTOMÁTICA...");
    Serial.printf("💧 Total u(t): %.3f ml\n", totalML);
    Serial.printf("🎯 EC Setpoint: %.0f µS/cm\n", ecSetpoint);
    Serial.printf("📊 EC Atual: %.0f µS/cm\n", ecActual);

    currentSequenceId = String(millis());
    currentDoseSource = "auto_ec";
    ecAtLastSequenceStart = ecActual;
    ecSetpointAtLastSequence = ecSetpoint;
    
    // ===== DISTRIBUIR u(t) PROPORCIONALMENTE BASEADO EM mlPerLiter =====
    // ✅ LÓGICA: u(t) é o esforço de controle total (ml calculados)
    // ✅ Cada nutriente recebe: u(t) × (mlPerLiter / totalMlPerLiter)
    
    totalNutrients = 0;
    intervalSeconds = 3;  // Padrão: 3 segundos entre nutrientes
    
    // Verificar se temos proporções dinâmicas configuradas
    bool useDynamicProportions = (activeNutrientsCount > 0 && totalMlPerLiter > 0.0);
    
    if (useDynamicProportions) {
        // ===== USAR PROPORÇÕES DINÂMICAS (da tabela nutricional) =====
        Serial.println("📊 Distribuindo u(t) usando proporções da tabela nutricional");
        Serial.printf("   💧 u(t) total: %.3f ml\n", totalML);
        Serial.printf("   📊 Total ml/L: %.2f\n", totalMlPerLiter);
        Serial.printf("   🔢 Nutrientes ativos: %d\n", activeNutrientsCount);
        
        // Distribuir u(t) proporcionalmente para cada nutriente (só slots ativos)
        for (int i = 0; i < activeNutrientsCount && totalNutrients < 8; i++) {
            if (!dynamicProportions[i].active || dynamicProportions[i].mlPerLiter <= 0.0) {
                continue;
            }
            
            // ✅ CALCULAR DOSAGEM PROPORCIONAL
            // dosagemNutriente = u(t) × (mlPerLiter / totalMlPerLiter)
            float proportion = dynamicProportions[i].proportion;
            float nutDosage = totalML * proportion;
            float nutTime = nutDosage / ecController.getFlowRate();
            int durationMs = (int)(nutTime * 1000);
            
            if (durationMs < 100) durationMs = 100; // Mínimo 100ms
            
            if (nutDosage > 0.001) {
                // Unificar mesmo relé na mesma secuencia (evita dosagem duplicada)
                int existingIdx = -1;
                for (int j = 0; j < totalNutrients; j++) {
                    if (nutrients[j].relay == dynamicProportions[i].relay) {
                        existingIdx = j;
                        break;
                    }
                }

                if (existingIdx >= 0) {
                    nutrients[existingIdx].dosageML += nutDosage;
                    nutrients[existingIdx].durationMs += durationMs;
                    Serial.printf("⚠️  %s: +%.3fml unificado no relé %d (slot duplicado ignorado)\n",
                        dynamicProportions[i].name.c_str(), nutDosage, dynamicProportions[i].relay + 1);
                } else {
                    nutrients[totalNutrients].name = dynamicProportions[i].name;
                    nutrients[totalNutrients].relay = dynamicProportions[i].relay;
                    nutrients[totalNutrients].dosageML = nutDosage;
                    nutrients[totalNutrients].durationMs = durationMs;

                    Serial.printf("📝 %s: %.3fml (%.1f%%) [%.2f ml/L] → %dms → Relé %d\n",
                        dynamicProportions[i].name.c_str(),
                        nutDosage,
                        proportion * 100,
                        dynamicProportions[i].mlPerLiter,
                        durationMs,
                        dynamicProportions[i].relay + 1);

                    totalNutrients++;
                }
            }
        }
    } else {
        // ===== FALLBACK: Proporções padrão (se não há tabela nutricional) =====
        Serial.println("⚠️  Tabela nutricional não configurada - usando proporções padrão");
        
        struct NutrientInfo {
            String name;
            int relay;
            float ratio;
        };
        
        NutrientInfo nutrientList[] = {
            {"Grow", 2, 0.35},
            {"Micro", 3, 0.35},
            {"Bloom", 4, 0.25},
            {"CalMag", 5, 0.05}
        };
        
        for (int i = 0; i < 4; i++) {
            float nutDosage = totalML * nutrientList[i].ratio;
            float nutTime = nutDosage / ecController.getFlowRate();
            int durationMs = (int)(nutTime * 1000);
            
            if (durationMs < 100) durationMs = 100;
            
            if (nutDosage > 0.001) {
                nutrients[totalNutrients].name = nutrientList[i].name;
                nutrients[totalNutrients].relay = nutrientList[i].relay;
                nutrients[totalNutrients].dosageML = nutDosage;
                nutrients[totalNutrients].durationMs = durationMs;
                
                Serial.printf("📝 %s: %.3fml (%.0f%%) → %dms → Relé %d\n", 
                    nutrientList[i].name.c_str(), nutDosage, nutrientList[i].ratio * 100, durationMs, nutrientList[i].relay + 1);
                
                totalNutrients++;
            }
        }
    }
    
    if (totalNutrients > 0) {
        // ===== INICIAR PRIMEIRO NUTRIENTE IMEDIATAMENTE =====
        currentNutrientIndex = 0;
        currentState = DOSING;
        stateStartTime = millis();
        
        SimpleNutrient& first = nutrients[0];
        Serial.printf("🚀 [DOSAGEM] Iniciando PRIMEIRO: %s - %.3fml por %.3fs - Relé %d\n", 
            first.name.c_str(), first.dosageML, first.durationMs / 1000.0, first.relay + 1);
        
        // ===== LIGAR PRIMEIRO RELÉ =====
        relayStates[first.relay] = true;
        bool state = !relayStates[first.relay];  // Invertido: LOW = ligado, HIGH = desligado
        
        // ✅ TODOS os relés estão no PCF2 (0x24) - mapeamento direto
        if (first.relay >= 0 && first.relay < 8) {
            pcf2.write(first.relay, state);
        }
        
        String displayMsg = first.name + ": " + String(first.dosageML, 2) + "ml";
        showMessage(displayMsg);
        
        Serial.printf("✅ [DOSAGEM] SISTEMA SEQUENCIAL INICIADO: %d nutrientes, intervalo %ds\n", totalNutrients, intervalSeconds);
        notifyEcOperationChanged();
    } else {
        Serial.println("❌ Nenhuma dosagem significativa para executar");
        currentState = IDLE;
    }
}

// ✅ Executar dosagem recebida do frontend
void HydroControl::executeWebDosage(JsonArray distribution, int intervalo) {
    // Se já há uma dosagem ativa, não iniciar nova
    if (currentState != IDLE) {
        Serial.println("⚠️  Sistema já ativo - ignorando nova dosagem web");
        return;
    }
    
    Serial.println("\n🌐 INICIANDO DOSAGEM VIA WEB...");
    currentSequenceId = String(millis());
    currentDoseSource = "web";
    ecAtLastSequenceStart = ec;
    ecSetpointAtLastSequence = ecSetpoint;
    
    totalNutrients = 0;
    intervalSeconds = intervalo;
    
    // ===== PROCESSAR DADOS DA WEB =====
    for (JsonVariant nutrient : distribution) {
        if (totalNutrients >= 8) break;  // Máximo 8 nutrientes
        
        String name = nutrient["name"].as<String>();
        int relay = nutrient["relay"].as<int>() - 1;  // Converter para índice (1-16 → 0-15)
        float dosageML = nutrient["dosage"].as<float>();
        float durationSec = nutrient["duration"].as<float>();
        int durationMs = (int)(durationSec * 1000);
        
        // ✅ Validar relé (0-7 para PCF2)
        if (relay < 0 || relay >= 8) {
            Serial.printf("⚠️ [DOSAGEM] Relé inválido: %d (deve ser 1-8)\n", relay + 1);
            continue;
        }
        
        if (durationMs < 100) durationMs = 100;  // Mínimo 100ms
        
        Serial.printf("📦 Web: %s → %.3fml → %dms → Relé %d\n", 
            name.c_str(), dosageML, durationMs, relay + 1);
        
        nutrients[totalNutrients].name = name;
        nutrients[totalNutrients].relay = relay;
        nutrients[totalNutrients].dosageML = dosageML;
        nutrients[totalNutrients].durationMs = durationMs;
        totalNutrients++;
    }
    
    if (totalNutrients > 0) {
        // ===== INICIAR PRIMEIRO NUTRIENTE =====
        currentNutrientIndex = 0;
        currentState = DOSING;
        stateStartTime = millis();
        
        SimpleNutrient& first = nutrients[0];
        Serial.printf("🚀 [DOSAGEM] Iniciando PRIMEIRO (Web): %s - %.3fml por %.3fs - Relé %d\n", 
            first.name.c_str(), first.dosageML, first.durationMs / 1000.0, first.relay + 1);
        
        // ===== LIGAR PRIMEIRO RELÉ =====
        relayStates[first.relay] = true;
        bool state = !relayStates[first.relay];  // Invertido: LOW = ligado, HIGH = desligado
        
        // ✅ TODOS os relés estão no PCF2 (0x24) - mapeamento direto
        if (first.relay >= 0 && first.relay < 8) {
            pcf2.write(first.relay, state);
        }
        
        String displayMsg = first.name + ": " + String(first.dosageML, 2) + "ml";
        showMessage(displayMsg);
        
        Serial.printf("✅ [DOSAGEM] SISTEMA WEB INICIADO: %d nutrientes, intervalo %ds\n", totalNutrients, intervalSeconds);
        notifyEcOperationChanged();
    } else {
        Serial.println("❌ Nenhum nutriente válido recebido da web");
        currentState = IDLE;
    }
}

// ✅ Cancelar dosagem em andamento
void HydroControl::cancelCurrentDosage() {
    if (dilutionState != DILUTION_IDLE) {
        Serial.println("\n🛑 CANCELANDO DILUIÇÃO EC...");
        finishDilutionSequence(false);
    }
    if (currentState != IDLE) {
        Serial.println("\n🛑 CANCELANDO DOSAGEM SEQUENCIAL EM ANDAMENTO...");

        if (currentState == RECIRCULATING) {
            notifyPhysicalRecirc(false, "ec");
        }
        
        if (currentState == DOSING) {
            // Desligar relé atual imediatamente
            SimpleNutrient& current = nutrients[currentNutrientIndex];
            relayStates[current.relay] = false;
            bool state = !relayStates[current.relay];  // Invertido: LOW = ligado, HIGH = desligado
            
            // ✅ TODOS os relés estão no PCF2 (0x24) - mapeamento direto
            if (current.relay >= 0 && current.relay < 8) {
                pcf2.write(current.relay, state);
            }
            
            Serial.printf("🔴 Relé %d CANCELADO (era %s)\n", current.relay + 1, current.name.c_str());
        }
        
        // Resetar sistema sequencial
        currentState = IDLE;
        totalNutrients = 0;
        currentNutrientIndex = 0;
        stateStartTime = 0;
        
        Serial.println("✅ DOSAGEM CANCELADA - Sistema resetado para IDLE");
        notifyEcOperationChanged();
    } else {
        Serial.println("ℹ️  Nenhuma dosagem ativa para cancelar");
    }
}

bool HydroControl::abortAutoOperationsOnBoot() {
    EcPhBootSnapshot snapshot = {};
    bool hadSnapshot = StatePersistenceManager::loadEcPhBootSnapshot(snapshot);
    bool wasActive = hadSnapshot &&
        ((snapshot.lastEcState[0] != '\0' && strcmp(snapshot.lastEcState, "idle") != 0) ||
         (snapshot.lastPhState[0] != '\0' && strcmp(snapshot.lastPhState, "idle") != 0));

    cancelCurrentDosage();

    if (phAutoState != PH_IDLE) {
        if (phActiveRelay >= 0 && phActiveRelay < 8) {
            setRelay(phActiveRelay, false);
        }
        phAutoState = PH_IDLE;
        phActiveRelay = -1;
        notifyPhOperationChanged();
        wasActive = true;
    }

    StatePersistenceManager::saveEcPhBootSnapshot("idle", "idle", wasActive);
    if (wasActive) {
        Serial.println("⚠️ BOOT: ciclo Auto EC/pH interrompido — fail-safe idle");
    }
    return wasActive;
}

// ✅ Atualizar proporções dinâmicas da tabela nutricional (recebido do frontend)
void HydroControl::updateNutrientProportions(JsonArray nutrients) {
    activeNutrientsCount = 0;
    totalMlPerLiter = 0.0;

    for (int i = 0; i < 16; i++) {
        dynamicProportions[i].name = "";
        dynamicProportions[i].relay = i;
        dynamicProportions[i].mlPerLiter = 0.0;
        dynamicProportions[i].proportion = 0.0;
        dynamicProportions[i].active = false;
    }
    
    // Primeiro passo: calcular totalMlPerLiter (soma de todos os mlPerLiter)
    for (JsonVariant nutrient : nutrients) {
        float mlPerLiter = nutrient["mlPerLiter"].as<float>();
        if (mlPerLiter > 0.0) {
            totalMlPerLiter += mlPerLiter;
        }
    }
    
    Serial.println("\n📊 Atualizando proporções da tabela nutricional...");
    Serial.printf("   Total ml/L: %.2f\n", totalMlPerLiter);
    
    if (totalMlPerLiter <= 0.0) {
        Serial.println("⚠️  Total ml/L é zero - proporções não podem ser calculadas");
        saveNutrientProportions();
        return;
    }
    
    // Segundo passo: calcular proporções e armazenar
    for (JsonVariant nutrient : nutrients) {
        if (activeNutrientsCount >= 16) break;  // Máximo 16 nutrientes
        
        String name = nutrient["name"].as<String>();
        int relayNumber = nutrient["relayNumber"].as<int>();  // 1-16 do frontend
        int relay = relayNumber - 1;  // Converter para índice 0-15
        float mlPerLiter = nutrient["mlPerLiter"].as<float>();
        
        // Validar relé
        if (relay < 0 || relay >= NUM_RELAYS) {
            Serial.printf("⚠️  Relé inválido: %d (deve ser 1-16)\n", relayNumber);
            continue;
        }
        
        if (mlPerLiter > 0.0) {
            // ✅ CALCULAR PROPORÇÃO: mlPerLiter / totalMlPerLiter
            float proportion = mlPerLiter / totalMlPerLiter;
            
            dynamicProportions[activeNutrientsCount].name = name;
            dynamicProportions[activeNutrientsCount].relay = relay;
            dynamicProportions[activeNutrientsCount].mlPerLiter = mlPerLiter;
            dynamicProportions[activeNutrientsCount].proportion = proportion;
            dynamicProportions[activeNutrientsCount].active = true;
            
            Serial.printf("   ✅ %s: %.2f ml/L (%.1f%%) → Relé %d\n", 
                name.c_str(), mlPerLiter, proportion * 100, relayNumber);
            
            activeNutrientsCount++;
        }
    }
    
    Serial.printf("✅ %d nutrientes ativos configurados\n", activeNutrientsCount);
    Serial.println("   💡 Proporções serão usadas na próxima dosagem automática");
    
    // ✅ Salvar proporções no NVS para persistência
    saveNutrientProportions();
}

// ✅ Calcular proporções a partir de mlPerLiter (função auxiliar)
void HydroControl::calculateProportionsFromMlPerLiter(JsonArray nutrients) {
    // Esta função é um alias para updateNutrientProportions
    updateNutrientProportions(nutrients);
}

// ===== PERSISTÊNCIA EM NVS =====

// ✅ Carregar configuração do Controller KP do NVS
void HydroControl::loadECControllerConfig() {
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║   📂 CARREGANDO EC_CONFIG DO NVS                    ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    
    // Carregar parâmetros do Controller
    float baseDose = 0.0;
    float flowRate = 0.0;
    float volume = 0.0;
    float totalMl = 0.0;
    float kp = 1.0;
    float setpoint = 0.0;
    float tolerance = 50.0f;
    bool autoEnabled = false;
    int intervalSeconds = 30;
    int32_t tempoRecircSec = 60;
    
    PreferencesManager::loadConfigFloat("ec_baseDose", baseDose);
    PreferencesManager::loadConfigFloat("ec_flowRate", flowRate);
    PreferencesManager::loadConfigFloat("ec_volume", volume);
    PreferencesManager::loadConfigFloat("ec_totalMl", totalMl);
    PreferencesManager::loadConfigFloat("ec_kp", kp);
    PreferencesManager::loadConfigFloat("ec_setpoint", setpoint);
    PreferencesManager::loadConfigFloat("ec_tolerance", tolerance);
    PreferencesManager::loadConfigInt("ec_autoEnabled", (int32_t&)autoEnabled);
    PreferencesManager::loadConfigInt("ec_interval", (int32_t&)intervalSeconds);
    PreferencesManager::loadConfigInt("ec_tempoRecirc", tempoRecircSec);
    
    // Mostrar valores carregados
    Serial.println("📊 Valores carregados do NVS:");
    Serial.printf("   • base_dose:        %.2f µS/cm\n", baseDose);
    Serial.printf("   • flow_rate:        %.3f ml/s\n", flowRate);
    Serial.printf("   • volume:           %.2f L\n", volume);
    Serial.printf("   • total_ml:         %.2f ml/L\n", totalMl);
    Serial.printf("   • kp:               %.2f\n", kp);
    Serial.printf("   • ec_setpoint:      %.0f µS/cm\n", setpoint);
    Serial.printf("   • tolerance:        %.0f µS/cm\n", tolerance);
    Serial.printf("   • auto_enabled:     %s\n", autoEnabled ? "true" : "false");
    Serial.printf("   • intervalo_auto_ec: %d segundos\n", intervalSeconds);
    Serial.printf("   • tempo_recirculacao: %ld segundos\n", (long)tempoRecircSec);
    
    // Aplicar valores carregados
    if (baseDose > 0.0) ecController.setBaseDose(baseDose);
    if (flowRate > 0.0) ecController.setFlowRate(flowRate);
    if (volume > 0.0) ecController.setVolume(volume);
    if (totalMl > 0.0) ecController.setTotalMl(totalMl);
    if (kp > 0.0) ecController.setKp(kp);
    if (setpoint > 0.0) ecSetpoint = setpoint;
    if (tolerance > 0.0) ecTolerance = tolerance;
    autoECEnabled = autoEnabled;
    if (intervalSeconds > 0) autoECIntervalSeconds = intervalSeconds;
    if (tempoRecircSec > 0) tempoRecirculacaoSeconds = (unsigned long)tempoRecircSec;

    int32_t dilAuto = 0;
    int32_t drainRelay = DILUTION_DRAIN_RELAY_DEFAULT;
    int32_t fillRelay = DILUTION_FILL_RELAY_DEFAULT;
    float maxVol = DILUTION_MAX_VOLUME_L_DEFAULT;
    float fillLps = DILUTION_FILL_FLOW_LPS;
    float ppl = FLOWMETER_PULSES_PER_LITER;
    PreferencesManager::loadConfigInt("dil_auto", dilAuto);
    PreferencesManager::loadConfigInt("dil_drainRelay", drainRelay);
    PreferencesManager::loadConfigInt("dil_fillRelay", fillRelay);
    PreferencesManager::loadConfigFloat("dil_maxVol", maxVol);
    PreferencesManager::loadConfigFloat("dil_fillLps", fillLps);
    PreferencesManager::loadConfigFloat("dil_ppl", ppl);
    dilutionAutoEnabled = dilAuto != 0;
    if (drainRelay >= 0 && drainRelay < 8) dilutionDrainRelay = (int)drainRelay;
    if (fillRelay >= 0 && fillRelay < 8) dilutionFillRelay = (int)fillRelay;
    if (maxVol > 0.0f) dilutionMaxVolumeL = maxVol;
    if (fillLps > 0.0f) dilutionFillFlowLps = fillLps;
    if (flowmeterSensor && ppl > 0.0f) flowmeterSensor->setPulsesPerLiter(ppl);
    Serial.printf("   • dilution_auto:    %s\n", dilutionAutoEnabled ? "true" : "false");
    Serial.printf("   • dil_drain_relay:  %d\n", dilutionDrainRelay);
    Serial.printf("   • dil_fill_relay:   %d\n", dilutionFillRelay);
    
    Serial.println("✅ EC_CONFIG carregado e aplicado com sucesso");
    Serial.println("╚════════════════════════════════════════════════════╝\n");
}

void HydroControl::loadPHControllerConfig() {
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║   📂 CARREGANDO PH_CONFIG DO NVS                    ║");
    Serial.println("╚════════════════════════════════════════════════════╝");

    float setpoint = 0.0f;
    bool autoEnabled = false;
    int32_t intervalSeconds = 300;

    PreferencesManager::loadConfigFloat("ph_setpoint", setpoint);
    PreferencesManager::loadConfigInt("ph_autoEnabled", (int32_t&)autoEnabled);
    PreferencesManager::loadConfigInt("ph_interval", intervalSeconds);

    Serial.println("📊 Valores carregados do NVS (PH):");
    Serial.printf("   • ph_setpoint:       %.2f\n", setpoint);
    Serial.printf("   • auto_enabled:      %s\n", autoEnabled ? "true" : "false");
    Serial.printf("   • intervalo_auto_ph: %ld segundos\n", (long)intervalSeconds);

    if (setpoint > 0.0f) phSetpoint = setpoint;
    autoPHEnabled = autoEnabled;
    if (intervalSeconds > 0) autoPHIntervalSeconds = (int)intervalSeconds;

    Serial.println("✅ PH_CONFIG carregado e aplicado com sucesso");
    Serial.println("╚════════════════════════════════════════════════════╝\n");
}

// ✅ Salvar configuração do Controller KP no NVS
void HydroControl::saveECControllerConfig() {
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║   💾 ATUALIZANDO EC_CONFIG NO NVS                   ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    
    // Obtener valores actuales
    float baseDose = ecController.getBaseDose();
    float flowRate = ecController.getFlowRate();
    float volume = ecController.getVolume();
    float totalMl = ecController.getTotalMl();
    float kp = ecController.getKp();
    float setpoint = ecSetpoint;
    bool autoEnabled = autoECEnabled;
    int interval = autoECIntervalSeconds;
    
    // Mostrar valores ANTES de guardar
    Serial.println("📊 Valores a serem salvos:");
    Serial.printf("   • base_dose:        %.2f µS/cm\n", baseDose);
    Serial.printf("   • flow_rate:        %.3f ml/s\n", flowRate);
    Serial.printf("   • volume:           %.2f L\n", volume);
    Serial.printf("   • total_ml:         %.2f ml/L\n", totalMl);
    Serial.printf("   • kp:               %.2f\n", kp);
    Serial.printf("   • ec_setpoint:      %.0f µS/cm\n", setpoint);
    Serial.printf("   • tolerance:        %.0f µS/cm\n", ecTolerance);
    Serial.printf("   • auto_enabled:     %s\n", autoEnabled ? "true" : "false");
    Serial.printf("   • intervalo_auto_ec: %d segundos\n", interval);
    
    // Guardar en NVS
    bool success = true;
    success &= PreferencesManager::saveConfigFloat("ec_baseDose", baseDose);
    success &= PreferencesManager::saveConfigFloat("ec_flowRate", flowRate);
    success &= PreferencesManager::saveConfigFloat("ec_volume", volume);
    success &= PreferencesManager::saveConfigFloat("ec_totalMl", totalMl);
    success &= PreferencesManager::saveConfigFloat("ec_kp", kp);
    success &= PreferencesManager::saveConfigFloat("ec_setpoint", setpoint);
    success &= PreferencesManager::saveConfigFloat("ec_tolerance", ecTolerance);
    success &= PreferencesManager::saveConfigInt("ec_autoEnabled", autoEnabled ? 1 : 0);
    success &= PreferencesManager::saveConfigInt("ec_interval", interval);
    
    if (success) {
        Serial.println("✅ EC_CONFIG salvo no NVS com sucesso");
        Serial.println("╚════════════════════════════════════════════════════╝\n");
    } else {
        Serial.println("❌ Erro ao salvar EC_CONFIG no NVS");
        Serial.println("╚════════════════════════════════════════════════════╝\n");
    }
}

// ✅ Carregar proporções nutricionais do NVS
void HydroControl::loadNutrientProportions() {
    Serial.println("\n📂 Carregando proporções nutricionais do NVS...");
    
    // Carregar número de nutrientes ativos
    int32_t count = 0;
    PreferencesManager::loadConfigInt("nut_count", count);
    
    if (count <= 0 || count > 16) {
        Serial.println("   ℹ️  Nenhuma proporção nutricional salva no NVS");
        return;
    }
    
    activeNutrientsCount = 0;
    totalMlPerLiter = 0.0;
    
    // Primeiro passo: calcular totalMlPerLiter
    for (int i = 0; i < count; i++) {
        String key = "nut_" + String(i) + "_ml";
        float mlPerLiter = 0.0;
        if (PreferencesManager::loadConfigFloat(key, mlPerLiter)) {
            if (mlPerLiter > 0.0) {
                totalMlPerLiter += mlPerLiter;
            }
        }
    }
    
    Serial.printf("   Total ml/L: %.2f\n", totalMlPerLiter);
    
    if (totalMlPerLiter <= 0.0) {
        Serial.println("   ⚠️  Total ml/L é zero - proporções não podem ser carregadas");
        return;
    }
    
    // Segundo passo: carregar cada nutriente e calcular proporções
    for (int i = 0; i < count && activeNutrientsCount < 16; i++) {
        String nameKey = "nut_" + String(i) + "_name";
        String relayKey = "nut_" + String(i) + "_relay";
        String mlKey = "nut_" + String(i) + "_ml";
        
        String name;
        int32_t relayNumber = 0;
        float mlPerLiter = 0.0;
        
        if (!PreferencesManager::loadConfig(nameKey, name)) continue;
        if (!PreferencesManager::loadConfigInt(relayKey, relayNumber)) continue;
        if (!PreferencesManager::loadConfigFloat(mlKey, mlPerLiter)) continue;
        
        if (mlPerLiter <= 0.0) continue;
        
        int relay = relayNumber - 1;  // Converter para índice 0-15
        if (relay < 0 || relay >= NUM_RELAYS) continue;
        
        // Calcular proporção
        float proportion = mlPerLiter / totalMlPerLiter;
        
        dynamicProportions[activeNutrientsCount].name = name;
        dynamicProportions[activeNutrientsCount].relay = relay;
        dynamicProportions[activeNutrientsCount].mlPerLiter = mlPerLiter;
        dynamicProportions[activeNutrientsCount].proportion = proportion;
        dynamicProportions[activeNutrientsCount].active = true;
        
        Serial.printf("   ✅ %s: %.2f ml/L (%.1f%%) → Relé %d\n", 
            name.c_str(), mlPerLiter, proportion * 100, relayNumber);
        
        activeNutrientsCount++;
    }
    
    Serial.printf("✅ %d proporções nutricionais carregadas do NVS\n", activeNutrientsCount);
}

// ✅ Implementação dos setters com persistência automática
void HydroControl::setECSetpoint(float setpoint, bool saveToNVS) {
    ecSetpoint = setpoint;
    if (saveToNVS) {
        saveECControllerConfig();
    }
}

void HydroControl::setECTolerance(float tolerance, bool saveToNVS) {
    ecTolerance = tolerance > 0 ? tolerance : 50.0f;
    if (saveToNVS) {
        PreferencesManager::saveConfigFloat("ec_tolerance", ecTolerance);
    }
}

void HydroControl::setAutoECEnabled(bool enabled, bool saveToNVS) {
    if (!enabled && autoECEnabled) {
        cancelCurrentDosage();
    }
    autoECEnabled = enabled;
    if (saveToNVS) {
        saveECControllerConfig();
    }
}

void HydroControl::setAutoECInterval(int intervalSeconds, bool saveToNVS) {
    if (intervalSeconds > 0 && intervalSeconds != autoECIntervalSeconds) {
        lastECCheckAtMs = millis();
    }
    autoECIntervalSeconds = intervalSeconds;
    if (saveToNVS) {
        saveECControllerConfig();
    }
}

void HydroControl::setTempoRecirculacaoSeconds(unsigned long seconds) {
    tempoRecirculacaoSeconds = seconds > 0 ? seconds : 60;
    PreferencesManager::saveConfigInt("ec_tempoRecirc", (int32_t)tempoRecirculacaoSeconds);
}

void HydroControl::setNutrientDoseCallback(NutrientDoseCallback cb, void* userData) {
    nutrientDoseCallback = cb;
    nutrientDoseCallbackUserData = userData;
}

void HydroControl::setEcOperationSyncCallback(EcOperationSyncCallback cb, void* userData) {
    ecOperationSyncCallback = cb;
    ecOperationSyncCallbackUserData = userData;
}

void HydroControl::setPhysicalRecircCallback(PhysicalRecircCallback cb, void* userData) {
    physicalRecircCallback = cb;
    physicalRecircCallbackUserData = userData;
}

void HydroControl::setEcMetricCallback(EcMetricCallback cb, void* userData) {
    ecMetricCallback = cb;
    ecMetricCallbackUserData = userData;
}

void HydroControl::setPhMetricCallback(PhMetricCallback cb, void* userData) {
    phMetricCallback = cb;
    phMetricCallbackUserData = userData;
}

void HydroControl::emitEcControllerMetric(bool adjustmentNeeded, bool adjustmentApplied,
                                            float dosageMl, float dosageTimeSec, float ecError,
                                            const String& sequenceId) {
    if (!ecMetricCallback) {
        return;
    }
    EcControllerMetricEvent event = {};
    event.ecSetpoint = ecSetpoint;
    event.ecActual = ec;
    event.ecError = ecError;
    event.kValue = ecController.getKValue();
    event.dosageMl = dosageMl;
    event.dosageTimeSeconds = dosageTimeSec;
    event.baseDose = ecController.getBaseDose();
    event.flowRate = ecController.getFlowRate();
    event.volume = ecController.getVolume();
    event.totalMl = ecController.getTotalMl();
    event.kp = ecController.getKp();
    event.autoEnabled = autoECEnabled;
    event.adjustmentNeeded = adjustmentNeeded;
    event.adjustmentApplied = adjustmentApplied;
    sequenceId.toCharArray(event.sequenceId, sizeof(event.sequenceId));
    ecMetricCallback(&event, ecMetricCallbackUserData);
}

void HydroControl::emitPhControllerMetric(bool adjustmentNeeded, bool adjustmentApplied,
                                            PhCorrectionPath path, const PhDosePlan* plan,
                                            const String& sequenceId) {
    if (!phMetricCallback) {
        return;
    }
    PhControllerMetricEvent event = {};
    event.phSetpoint = phSetpoint;
    event.phBefore = pH;
    event.errorH = AdaptivePHController::errorH(phSetpoint, pH);
    if (!isfinite(event.errorH)) {
        const float linearErr = phSetpoint - pH;
        event.errorH = isfinite(linearErr) ? linearErr * 1e-6f : 0.0f;
    }
    event.kAcid = adaptivePhController.getKAcid();
    event.kBase = adaptivePhController.getKBase();
    event.aggressiveness = phAggressiveness;
    event.autoEnabled = autoPHEnabled;
    event.adjustmentNeeded = adjustmentNeeded;
    event.adjustmentApplied = adjustmentApplied;
    if (path == PH_PATH_BASE) {
        strncpy(event.direction, "up", sizeof(event.direction) - 1);
    } else if (path == PH_PATH_ACID) {
        strncpy(event.direction, "down", sizeof(event.direction) - 1);
    }
    if (plan != nullptr) {
        event.kUsed = plan->kUsed;
        event.doseIdealMl = plan->doseIdealMl;
        event.doseRealMl = plan->doseRealMl;
        event.dosageTimeSeconds = plan->durationSec;
        const float doseCap = 9999999.0f;
        if (event.doseIdealMl > doseCap) event.doseIdealMl = doseCap;
        if (event.doseRealMl > doseCap) event.doseRealMl = doseCap;
    }
    sequenceId.toCharArray(event.sequenceId, sizeof(event.sequenceId));
    phMetricCallback(&event, phMetricCallbackUserData);
}

void HydroControl::notifyEcOperationChanged() {
    StatePersistenceManager::saveEcPhBootSnapshot(
        getEcOperationStateName(), getPhOperationStateName(), false);
    if (ecOperationSyncCallback) {
        ecOperationSyncCallback(ecOperationSyncCallbackUserData);
    }
}

void HydroControl::notifyPhysicalRecirc(bool starting, const char* domain) {
    if (physicalRecircCallback) {
        physicalRecircCallback(starting, domain, physicalRecircCallbackUserData);
    }
}

void HydroControl::emitNutrientDoseEvent(const SimpleNutrient& nutrient) {
    if (!nutrientDoseCallback) {
        return;
    }
    NutrientDoseEvent event = {};
    currentSequenceId.toCharArray(event.sequenceId, sizeof(event.sequenceId));
    nutrient.name.toCharArray(event.nutrientName, sizeof(event.nutrientName));
    event.relayNumber = nutrient.relay;
    event.dosageMl = nutrient.dosageML;
    event.dosageTimeSeconds = nutrient.durationMs / 1000.0f;
    event.ecBefore = ecAtLastSequenceStart;
    event.ecSetpoint = ecSetpointAtLastSequence;
    event.source = currentDoseSource ? currentDoseSource : "auto_ec";
    nutrientDoseCallback(&event, nutrientDoseCallbackUserData);
}

int HydroControl::computeEcOperationRemainingSec() const {
    if (dilutionState != DILUTION_IDLE) {
        const unsigned long elapsedMs = millis() - dilutionStateStartMs;
        if (dilutionState == DILUTION_DRAINING) {
            if (dilutionFillFlowLps > 0.0f && dilutionTargetL > dilutionProgressL) {
                const float remL = dilutionTargetL - dilutionProgressL;
                return (int)ceilf(remL / dilutionFillFlowLps);
            }
            return 0;
        }
        if (dilutionState == DILUTION_FILLING) {
            if (dilutionFillDurationMs > elapsedMs) {
                return (int)((dilutionFillDurationMs - elapsedMs) / 1000UL);
            }
            return 0;
        }
        if (dilutionState == DILUTION_RECIRCULATING) {
            const long rem = (long)tempoRecirculacaoSeconds - (long)(elapsedMs / 1000UL);
            return rem > 0 ? (int)rem : 0;
        }
    }
    if (currentState == IDLE) {
        return 0;
    }
    unsigned long elapsedMs = millis() - stateStartTime;
    if (currentState == WAITING) {
        long rem = (long)intervalSeconds - (long)(elapsedMs / 1000UL);
        return rem > 0 ? (int)rem : 0;
    }
    if (currentState == RECIRCULATING) {
        long rem = (long)tempoRecirculacaoSeconds - (long)(elapsedMs / 1000UL);
        return rem > 0 ? (int)rem : 0;
    }
    if (currentState == DOSING && currentNutrientIndex < totalNutrients) {
        const SimpleNutrient& current = nutrients[currentNutrientIndex];
        long rem = (long)(current.durationMs - elapsedMs) / 1000L;
        return rem > 0 ? (int)rem : 0;
    }
    return 0;
}

const char* HydroControl::getEcOperationStateName() const {
    if (dilutionState == DILUTION_DRAINING) {
        return "diluting_draining";
    }
    if (dilutionState == DILUTION_FILLING) {
        return "diluting_filling";
    }
    if (dilutionState == DILUTION_RECIRCULATING) {
        return "recirculating";
    }
    if (!autoECEnabled && currentState != IDLE) {
        return "idle";
    }
    switch (currentState) {
        case DOSING:
        case WAITING:
            // WAITING (~3s entre nutrientes) — mesma secuencia; UI só mostra "Dosando"
            return "dosing";
        case RECIRCULATING:
            return "recirculating";
        case IDLE:
        default: {
            if (!autoECEnabled) {
                return "idle";
            }
            if (getEcNextCheckInSec() > 0) {
                return "ec_check_pending";
            }
            return "idle";
        }
    }
}

int HydroControl::getEcOperationRemainingSec() const {
    return computeEcOperationRemainingSec();
}

int HydroControl::getEcNextCheckInSec() const {
    if (dilutionState != DILUTION_IDLE || currentState != IDLE) {
        return 0;
    }
    if (!autoECEnabled && !dilutionAutoEnabled) {
        return 0;
    }
    unsigned long checkInterval = autoECIntervalSeconds > 0 ?
        (autoECIntervalSeconds * 1000UL) : EC_CHECK_INTERVAL;
    if (lastECCheckAtMs == 0) {
        return (int)(checkInterval / 1000UL);
    }
    unsigned long elapsed = millis() - lastECCheckAtMs;
    if (elapsed >= checkInterval) {
        return 0;
    }
    return (int)((checkInterval - elapsed) / 1000UL);
}

// ✅ Salvar proporções nutricionais no NVS
void HydroControl::saveNutrientProportions() {
    Serial.println("\n💾 Salvando proporções nutricionais no NVS...");
    
    // Salvar número de nutrientes ativos
    bool success = PreferencesManager::saveConfigInt("nut_count", activeNutrientsCount);
    
    if (!success) {
        Serial.println("❌ Erro ao salvar contador de nutrientes");
        return;
    }
    
    // Salvar cada nutriente
    for (int i = 0; i < activeNutrientsCount; i++) {
        String nameKey = "nut_" + String(i) + "_name";
        String relayKey = "nut_" + String(i) + "_relay";
        String mlKey = "nut_" + String(i) + "_ml";
        
        success &= PreferencesManager::saveConfig(nameKey, dynamicProportions[i].name);
        success &= PreferencesManager::saveConfigInt(relayKey, dynamicProportions[i].relay + 1);  // Salvar como 1-16
        success &= PreferencesManager::saveConfigFloat(mlKey, dynamicProportions[i].mlPerLiter);
    }
    
    // Salvar totalMlPerLiter também
    success &= PreferencesManager::saveConfigFloat("nut_totalMl", totalMlPerLiter);
    
    if (success) {
        Serial.printf("✅ %d proporções nutricionais salvas no NVS\n", activeNutrientsCount);
    } else {
        Serial.println("❌ Erro ao salvar proporções nutricionais no NVS");
    }
}

// ===== Auto pH =====

void HydroControl::setPHSetpoint(float setpoint, bool saveToNVS) {
    phSetpoint = setpoint;
    if (saveToNVS) {
        PreferencesManager::saveConfigFloat("ph_setpoint", setpoint);
    }
}

void HydroControl::setAutoPHEnabled(bool enabled, bool saveToNVS) {
    autoPHEnabled = enabled;
    if (!enabled && phAutoState != PH_IDLE) {
        phAutoState = PH_IDLE;
        phActiveRelay = -1;
        notifyPhOperationChanged();
    }
    if (saveToNVS) {
        PreferencesManager::saveConfigInt("ph_autoEnabled", enabled ? 1 : 0);
    }
}

void HydroControl::setAutoPHInterval(int intervalSeconds, bool saveToNVS) {
    autoPHIntervalSeconds = intervalSeconds > 0 ? intervalSeconds : 300;
    if (saveToNVS) {
        PreferencesManager::saveConfigInt("ph_interval", autoPHIntervalSeconds);
    }
}

void HydroControl::setPhPumpConfig(int relayUp, int relayDown, float flowUp, float flowDown,
                                     float mlPerUnitAcid, float mlPerUnitBase) {
    relayPhUp = relayUp;
    relayPhDown = relayDown;
    flowRatePhUp = flowUp > 0 ? flowUp : 1.0f;
    flowRatePhDown = flowDown > 0 ? flowDown : 1.0f;
    mlPerPhUnitAcid = mlPerUnitAcid > 0 ? mlPerUnitAcid : 2.0f;
    mlPerPhUnitBase = mlPerUnitBase > 0 ? mlPerUnitBase : 2.0f;
    if (adaptivePhController.getValidLearningCycles() == 0) {
        adaptivePhController.setSeedFromMlPerPhUnit(phSetpoint, mlPerPhUnitAcid, mlPerPhUnitBase);
    }
}

void HydroControl::setPhAdaptiveConfig(float aggressiveness, float gainAlpha,
                                       float maxDoseMl, int maxPulseSec, int maxConsecutive) {
    phAggressiveness = aggressiveness >= 0.05f ? aggressiveness : 0.5f;
    if (phAggressiveness > 1.0f) phAggressiveness = 1.0f;
    phGainAlpha = gainAlpha >= 0.05f ? gainAlpha : 0.2f;
    if (phGainAlpha > 0.5f) phGainAlpha = 0.5f;
    phMaxDoseMl = maxDoseMl > 0 ? maxDoseMl : 50.0f;
    phMaxPulseSec = maxPulseSec > 0 ? maxPulseSec : 120;
    phMaxConsecutive = maxConsecutive > 0 ? maxConsecutive : 5;
}

void HydroControl::resetPhLearnedGains() {
    adaptivePhController.setSeedFromMlPerPhUnit(phSetpoint, mlPerPhUnitAcid, mlPerPhUnitBase);
    adaptivePhController.saveToNVS();
    phConsecutiveCorrections = 0;
}

void HydroControl::setPhDoseCallback(PhDoseCallback cb, void* userData) {
    phDoseCallback = cb;
    phDoseCallbackUserData = userData;
}

void HydroControl::setPhGainLearnedCallback(PhGainLearnedCallback cb, void* userData) {
    phGainLearnedCallback = cb;
    phGainLearnedCallbackUserData = userData;
}

float HydroControl::getPhErrorH() const {
    return AdaptivePHController::errorH(phSetpoint, pH);
}

void HydroControl::setPhOperationSyncCallback(PhOperationSyncCallback cb, void* userData) {
    phOperationSyncCallback = cb;
    phOperationSyncCallbackUserData = userData;
}

void HydroControl::notifyPhOperationChanged() {
    StatePersistenceManager::saveEcPhBootSnapshot(
        getEcOperationStateName(), getPhOperationStateName(), false);
    if (phOperationSyncCallback) {
        phOperationSyncCallback(phOperationSyncCallbackUserData);
    }
}

void HydroControl::startPhAutoDosage(int relay, float durationSec, PhCorrectionPath path,
                                     float mlApplied, float hBefore, float phBefore) {
#if PH_PROTOTYPE_RELAX_GUARDS
    if (relay < 0 || relay >= 8 || durationSec < 0.1f) return;
#else
    if (relay < 0 || relay >= 8 || durationSec < 0.5f) return;
    if (currentState != IDLE) {
        Serial.println("⚠️ [AUTO PH] EC sequencial ativo — adiando dosagem pH");
        return;
    }
#endif
    const char* pathLabel = (path == PH_PATH_BASE) ? "pH+" : (path == PH_PATH_ACID) ? "pH-" : "pH";

    phAutoState = PH_DOSING;
    phActiveRelay = relay;
    phActivePath = path;
    phCycleHBefore = hBefore;
    phCyclePhBefore = phBefore;
    phCycleMlApplied = mlApplied;
    phCycleDurationSec = durationSec;
    phCycleDurationMs = (unsigned long)(durationSec * 1000.0f);
    if (phCycleDurationMs < 1000UL) {
        phCycleDurationMs = 1000UL;
    }
    phCurrentSequenceId = String(millis());
    phConsecutiveCorrections++;
    phStateStartMs = millis();

    Serial.printf("🚀 [DOSAGEM pH] Iniciando: %s - %.3fml por %.3fs - Relé %d\n",
        pathLabel, mlApplied, phCycleDurationMs / 1000.0, relay + 1);

    relayStates[relay] = true;
    bool state = !relayStates[relay];
    if (relay >= 0 && relay < 8) {
        if (pcf2_ok) {
            pcf2.write(relay, state);
        } else {
            Serial.println("❌ [RELAY] PCF8574 #2 (0x24) não conectado!");
        }
    }

    Serial.printf("✅ [DOSAGEM pH] Ciclo iniciado: %.3f ml, relé %d\n", mlApplied, relay + 1);
    notifyPhOperationChanged();
}

void HydroControl::finishPhRecirculation() {
    notifyPhysicalRecirc(false, "ph");
    const float hAfter = AdaptivePHController::toH(pH);
    const bool kLearned = adaptivePhController.updateGainAfterDose(
        phActivePath, phCycleHBefore, hAfter, phCycleMlApplied, phGainAlpha);

    if (kLearned && phGainLearnedCallback) {
        phGainLearnedCallback(phGainLearnedCallbackUserData);
    }

    if (adaptivePhController.needsAdjustment(phSetpoint, pH, phTolerance)) {
        // ainda fora da banda — mantém contador consecutivo
    } else {
        phConsecutiveCorrections = 0;
    }

    phAutoState = PH_IDLE;
    phActiveRelay = -1;
    phActivePath = PH_PATH_NONE;
    phCycleDurationMs = 0;
    lastPHCheck = millis();
    lastPHCheckAtMs = millis();
    Serial.println("✅ SEQUÊNCIA pH COMPLETA");
    notifyPhOperationChanged();
}

void HydroControl::emitPhDoseEvent() {
    if (!phDoseCallback) return;

    PhDoseEvent event = {};
    phCurrentSequenceId.toCharArray(event.sequenceId, sizeof(event.sequenceId));
    if (phActivePath == PH_PATH_BASE) {
        strncpy(event.direction, "up", sizeof(event.direction) - 1);
    } else if (phActivePath == PH_PATH_ACID) {
        strncpy(event.direction, "down", sizeof(event.direction) - 1);
    } else {
        strncpy(event.direction, "none", sizeof(event.direction) - 1);
    }
    event.relayNumber = phActiveRelay;
    event.dosageMl = phCycleMlApplied;
    event.dosageTimeSeconds = phCycleDurationSec;
    event.phBefore = phCyclePhBefore;
    event.phSetpoint = phSetpoint;
    event.kAcid = adaptivePhController.getKAcid();
    event.kBase = adaptivePhController.getKBase();
    event.errorH = AdaptivePHController::errorH(phSetpoint, phCyclePhBefore);
    event.source = "auto_ph";
    phDoseCallback(&event, phDoseCallbackUserData);
}

void HydroControl::processPhAutoState() {
    if (phAutoState == PH_IDLE) return;

    const unsigned long now = millis();
    const unsigned long elapsedMs = now - phStateStartMs;

    if (phAutoState == PH_DOSING) {
        if (elapsedMs >= phCycleDurationMs) {
            if (phActiveRelay >= 0 && phActiveRelay < 8) {
                relayStates[phActiveRelay] = false;
                bool state = !relayStates[phActiveRelay];
                if (pcf2_ok) {
                    pcf2.write(phActiveRelay, state);
                }
                Serial.printf("🔴 [DOSAGEM pH] Relé %d DESLIGADO após %.3fs\n",
                    phActiveRelay + 1, phCycleDurationMs / 1000.0);
            }

            emitPhDoseEvent();

            phAutoState = PH_RECIRCULATING;
            phStateStartMs = now;
            Serial.printf("⏳ [RECIRC] Aguardando %lu s (tempo_recirculacao)...\n", phRecircSeconds);
            notifyPhysicalRecirc(true, "ph");
            notifyPhOperationChanged();
        }
        return;
    }

    if (phAutoState == PH_RECIRCULATING) {
        if (elapsedMs >= phRecircSeconds * 1000UL) {
            Serial.println("✅ [RECIRC] Tempo de recirculação concluído");
            finishPhRecirculation();
        }
    }
}

void HydroControl::checkAutoPH() {
    if (!autoPHEnabled) {
        static unsigned long lastPhDebugPrint = 0;
        const unsigned long now = millis();
        if (now - lastPhDebugPrint >= 30000) {
            Serial.println("⚠️ [AUTO pH] auto_enabled = false - ative no frontend primeiro!");
            Serial.printf("   💡 Valores atuais: setpoint=%.2f, ph=%.2f, tolerance=%.2f\n",
                phSetpoint, pH, phTolerance);
            lastPhDebugPrint = now;
        }
        return;
    }
    if (phAutoState != PH_IDLE) return;

    if (isAutoDosingPausedByInterlock()) {
        static unsigned long lastPhInterlockLog = 0;
        const unsigned long nowMs = millis();
        if (nowMs - lastPhInterlockLog >= 60000UL) {
            lastPhInterlockLog = nowMs;
            if (!tankLevelOk) {
                Serial.println("⚠️ [AUTO PH] Pausado — nível de água baixo (water_level_ok=false)");
            } else {
                Serial.println("⚠️ [AUTO PH] Pausado — script tanque P1 activo");
            }
        }
        return;
    }

#if !PH_PROTOTYPE_RELAX_GUARDS
    if (currentState != IDLE) return;
#endif

    unsigned long now = millis();
    unsigned long checkInterval = autoPHIntervalSeconds > 0
        ? (autoPHIntervalSeconds * 1000UL) : 300000UL;
    if (now - lastPHCheck < checkInterval) return;

    lastPHCheck = now;
    lastPHCheckAtMs = now;

    float phForControl = pH;
#if !HIDRO_DEV_RELAX_SENSORS
    if (!isPhValidForTelemetry()) {
        static unsigned long lastInvalidPhLog = 0;
        const unsigned long t = millis();
        if (t - lastInvalidPhLog >= 60000) {
            lastInvalidPhLog = t;
            Serial.printf("⚠️ [AUTO PH] Lectura pH inválida o stale — omitiendo ciclo\n");
        }
        return;
    }
#else
    if (!isPhValidForTelemetry()) {
        return;
    }
#endif

    if (!adaptivePhController.needsAdjustment(phSetpoint, phForControl, phTolerance)) {
        phConsecutiveCorrections = 0;
        emitPhControllerMetric(false, false, PH_PATH_NONE, nullptr, "");
        return;
    }

#if !PH_PROTOTYPE_RELAX_GUARDS
    if (phConsecutiveCorrections >= phMaxConsecutive) {
        Serial.printf("🛑 [AUTO PH] Limite consecutivo (%d) — pausa\n", phMaxConsecutive);
        return;
    }
#endif

    const PhCorrectionPath path = adaptivePhController.selectPath(phSetpoint, phForControl, phTolerance);
    if (path == PH_PATH_NONE) {
        emitPhControllerMetric(true, false, PH_PATH_NONE, nullptr, "");
        return;
    }

    const float flow = path == PH_PATH_BASE ? flowRatePhUp : flowRatePhDown;
    const bool commissioning = adaptivePhController.getValidLearningCycles() < 3;
    const PhDosePlan plan = adaptivePhController.planDose(
        phSetpoint, phForControl, phTolerance, phAggressiveness, flow,
        phMaxDoseMl, (float)phMaxPulseSec, commissioning);

    if (!plan.valid) {
        emitPhControllerMetric(true, false, path, &plan, "");
        return;
    }

    const int relay = path == PH_PATH_BASE ? relayPhUp : relayPhDown;
    const float hBefore = AdaptivePHController::toH(phForControl);
    const float phError = phSetpoint - pH;

    Serial.println("\n🤖 === CONTROLE AUTOMÁTICO pH ===");
    Serial.printf("📊 pH Atual: %.2f\n", pH);
    Serial.printf("🎯 pH Setpoint: %.2f\n", phSetpoint);
    Serial.printf("⚡ Erro: %.2f\n", phError);
    Serial.printf("💧 u(t) calculado: %.3f ml (domínio H, K=%.3e)\n", plan.doseRealMl, plan.kUsed);
    Serial.printf("⏱️ Tempo de dosagem: %.2f segundos\n", plan.durationSec);
    Serial.printf("⏱️  Tempo de dosagem: %.1f segundos\n", plan.durationSec);
    Serial.println("================================\n");

    startPhAutoDosage(relay, plan.durationSec, path, plan.doseRealMl, hBefore, pH);
    emitPhControllerMetric(true, true, path, &plan, phCurrentSequenceId);
}

const char* HydroControl::getPhOperationStateName() const {
    if (!autoPHEnabled && phAutoState == PH_IDLE) return "idle";
    switch (phAutoState) {
        case PH_DOSING: return "dosing";
        case PH_RECIRCULATING: return "recirculating";
        case PH_IDLE:
        default:
            if (autoPHEnabled && getPhNextCheckInSec() > 0) return "ph_check_pending";
            return "idle";
    }
}

int HydroControl::computePhOperationRemainingSec() const {
    if (phAutoState == PH_IDLE) {
        return 0;
    }
    const unsigned long elapsedMs = millis() - phStateStartMs;
    if (phAutoState == PH_DOSING) {
        long rem = (long)(phCycleDurationMs - elapsedMs) / 1000L;
        return rem > 0 ? (int)rem : 0;
    }
    if (phAutoState == PH_RECIRCULATING) {
        long rem = (long)phRecircSeconds - (long)(elapsedMs / 1000UL);
        return rem > 0 ? (int)rem : 0;
    }
    return 0;
}

int HydroControl::getPhOperationRemainingSec() const {
    return computePhOperationRemainingSec();
}

int HydroControl::getPhNextCheckInSec() const {
    if (!autoPHEnabled || phAutoState != PH_IDLE) return 0;
#if !PH_PROTOTYPE_RELAX_GUARDS
    if (currentState != IDLE) return 0;
#endif
    unsigned long checkInterval = autoPHIntervalSeconds > 0
        ? (autoPHIntervalSeconds * 1000UL) : 300000UL;
    if (lastPHCheckAtMs == 0) return (int)(checkInterval / 1000UL);
    unsigned long elapsed = millis() - lastPHCheckAtMs;
    if (elapsed >= checkInterval) return 0;
    return (int)((checkInterval - elapsed) / 1000UL);
}

namespace {
constexpr const char* EC_SENSOR_NVS_NS = "ec_sensor";
constexpr const char* EC_SENSOR_CAL_K_KEY = "cal_k";
}  // namespace

void HydroControl::loadEcCalibrationFromNVS() {
    if (!ecSensor) {
        return;
    }
    Preferences prefs;
    if (!prefs.begin(EC_SENSOR_NVS_NS, true)) {
        return;
    }
    const float k = prefs.getFloat(EC_SENSOR_CAL_K_KEY, TDS_CALIBRATION_FACTOR);
    prefs.end();
    if (k > 0.0f && k <= 10.0f) {
        ecSensor->setCalibrationFactor(k);
        Serial.printf("[EC] K cargado de NVS: %.4f\n", k);
    }
}

void HydroControl::saveEcCalibrationToNVS() {
    if (!ecSensor) {
        return;
    }
    Preferences prefs;
    if (!prefs.begin(EC_SENSOR_NVS_NS, false)) {
        Serial.println("[EC] Error abriendo NVS para guardar K");
        return;
    }
    const float k = ecSensor->calibrationFactor();
    const bool ok = prefs.putFloat(EC_SENSOR_CAL_K_KEY, k);
    prefs.end();
    if (ok) {
        Serial.printf("[EC] K guardado en NVS: %.4f\n", k);
    } else {
        Serial.println("[EC] Error guardando K en NVS");
    }
}

void HydroControl::saveTDSCalibration() {
    saveEcCalibrationToNVS();
}

bool HydroControl::calibrateTDS(float standardValue, float measuredValue) {
    if (!ecSensor) {
        return false;
    }
    const bool ok = ecSensor->calibrate(standardValue, measuredValue);
    if (ok) {
        saveEcCalibrationToNVS();
    }
    return ok;
}

bool HydroControl::calibrateTDSWithSolution1413() {
    if (!ecSensor) {
        return false;
    }
    const bool ok = ecSensor->calibrateWithSolution1413();
    if (ok) {
        saveEcCalibrationToNVS();
    }
    return ok;
}

void HydroControl::setTDSCalibrationFactor(float factor) {
    if (!ecSensor) {
        return;
    }
    ecSensor->setCalibrationFactor(factor);
    saveEcCalibrationToNVS();
}

void HydroControl::setTDSVRef(float vref) {
    if (!ecSensor) {
        return;
    }
    ecSensor->setFullScaleVolts(vref);
}

float HydroControl::getTDSCalibrationFactor() const {
    return ecSensor ? ecSensor->calibrationFactor() : TDS_CALIBRATION_FACTOR;
}

float HydroControl::getTDSVRef() const {
    return ecSensor ? ecSensor->fullScaleVolts() : TDS_VREF;
}

// ===== Diluição EC modo A =====

void HydroControl::setDilutionAutoEnabled(bool enabled, bool saveToNVS) {
    dilutionAutoEnabled = enabled;
    if (!enabled && dilutionState != DILUTION_IDLE) {
        finishDilutionSequence(false);
    }
    if (saveToNVS) {
        PreferencesManager::saveConfigInt("dil_auto", enabled ? 1 : 0);
    }
}

void HydroControl::setDilutionRelays(int drainRelay, int fillRelay) {
    if (drainRelay >= 0 && drainRelay < 8) {
        dilutionDrainRelay = drainRelay;
        PreferencesManager::saveConfigInt("dil_drainRelay", drainRelay);
    }
    if (fillRelay >= 0 && fillRelay < 8) {
        dilutionFillRelay = fillRelay;
        PreferencesManager::saveConfigInt("dil_fillRelay", fillRelay);
    }
}

void HydroControl::setDilutionMaxVolumeL(float maxL) {
    if (maxL > 0.0f) {
        dilutionMaxVolumeL = maxL;
        PreferencesManager::saveConfigFloat("dil_maxVol", maxL);
    }
}

void HydroControl::setDilutionFillFlowLps(float lps) {
    if (lps > 0.0f) {
        dilutionFillFlowLps = lps;
        PreferencesManager::saveConfigFloat("dil_fillLps", lps);
    }
}

void HydroControl::setFlowmeterPulsesPerLiter(float ppl) {
    if (ppl > 0.0f && flowmeterSensor) {
        flowmeterSensor->setPulsesPerLiter(ppl);
        PreferencesManager::saveConfigFloat("dil_ppl", ppl);
    }
}

void HydroControl::setEcDilutionCallback(EcDilutionCallback cb, void* userData) {
    ecDilutionCallback = cb;
    ecDilutionCallbackUserData = userData;
}

void HydroControl::setDilutionRelay(int relayIndex, bool on) {
    if (relayIndex < 0 || relayIndex >= 8 || !pcf2_ok) {
        return;
    }
    relayStates[relayIndex] = on;
    pcf2.write(relayIndex, !on);
}

static String makeDilutionSequenceId() {
    return String("dil") + String(millis());
}

bool HydroControl::startEcDilution(float volumeLiters, const char* source) {
    if (volumeLiters < 0.1f) {
        Serial.println("[DILUTION] volume < 0.1 L — ignorado");
        return false;
    }
    if (volumeLiters > dilutionMaxVolumeL) {
        Serial.printf("[DILUTION] volume %.2f L > max %.2f L\n", volumeLiters, dilutionMaxVolumeL);
        return false;
    }
    if (dilutionDrainRelay < 0 || dilutionFillRelay < 0) {
        Serial.println("[DILUTION] relés dreno/llenado no configurados");
        return false;
    }
    if (currentState != IDLE || dilutionState != DILUTION_IDLE) {
        Serial.println("[DILUTION] sistema ocupado");
        return false;
    }
    if (!tankLevelOk) {
        Serial.println("[DILUTION] nivel bajo — abortado");
        return false;
    }
    if (!ecValid) {
        Serial.println("[DILUTION] EC inválida");
        return false;
    }
#if USE_PH_MODBUS_SENSOR && HIDRO_EC_REQUIRES_PH_MODBUS
    if (!phValid) {
        Serial.println("[DILUTION] pH Modbus inválido — abortado");
        return false;
    }
#endif

    dilutionTargetL = volumeLiters;
    dilutionProgressL = 0.0f;
    dilutionDrainMeasuredL = 0.0f;
    dilutionEcBefore = ec;
    dilutionSource = source ? source : "manual";
    dilutionSequenceId = makeDilutionSequenceId();
    dilutionState = DILUTION_DRAINING;
    dilutionStateStartMs = millis();
    dilutionDrainStartMs = dilutionStateStartMs;
    dilutionLastPulseMs = dilutionDrainStartMs;
    dilutionLastPulseCount = flowmeterSensor ? flowmeterSensor->pulseCount() : 0;
    if (flowmeterSensor) {
        flowmeterSensor->reset();
    }

    setDilutionRelay(dilutionDrainRelay, true);
    setDilutionRelay(dilutionFillRelay, false);

    Serial.println("\n💧 === DILUIÇÃO EC (modo A) ===");
    Serial.printf("📊 EC: %.0f µS/cm | SP: %.0f\n", ec, ecSetpoint);
    Serial.printf("🎯 Volume dreno+reposição: %.2f L | fonte: %s\n", volumeLiters, dilutionSource);
    Serial.println("================================\n");

    showMessage("Drenando...");
    notifyEcOperationChanged();
    return true;
}

void HydroControl::finishDilutionDrainPhase() {
    setDilutionRelay(dilutionDrainRelay, false);
    dilutionDrainMeasuredL = flowmeterSensor ? flowmeterSensor->consumedLiters() : dilutionProgressL;
    if (dilutionDrainMeasuredL < 0.05f) {
        dilutionDrainMeasuredL = dilutionTargetL;
    }
    dilutionProgressL = 0.0f;
    dilutionState = DILUTION_FILLING;
    dilutionStateStartMs = millis();
    dilutionFillStartMs = dilutionStateStartMs;
    if (flowmeterSensor) {
        flowmeterSensor->reset();
    }
    if (dilutionFillFlowLps > 0.0f) {
        dilutionFillDurationMs = (unsigned long)((dilutionDrainMeasuredL / dilutionFillFlowLps) * 1000.0f);
        if (dilutionFillDurationMs < 1000UL) {
            dilutionFillDurationMs = 1000UL;
        }
    } else {
        dilutionFillDurationMs = 60000UL;
    }
    setDilutionRelay(dilutionFillRelay, true);
    Serial.printf("💧 [DILUTION] Dreno OK %.2f L — repondo ~%.0f s\n",
                  dilutionDrainMeasuredL,
                  dilutionFillDurationMs / 1000UL);
    showMessage("Repondo agua");
    notifyEcOperationChanged();
}

void HydroControl::emitEcDilutionEvent() {
    if (!ecDilutionCallback) {
        return;
    }
    EcDilutionEvent event = {};
    dilutionSequenceId.toCharArray(event.sequenceId, sizeof(event.sequenceId));
    event.ecBefore = dilutionEcBefore;
    event.ecSetpoint = ecSetpoint;
    event.volumeTargetL = dilutionTargetL;
    event.volumeMeasuredL = dilutionDrainMeasuredL;
    const unsigned long drainMs = dilutionFillStartMs > dilutionDrainStartMs
        ? (dilutionFillStartMs - dilutionDrainStartMs) : 0;
    const unsigned long fillMs = dilutionStateStartMs > dilutionFillStartMs
        ? (millis() - dilutionFillStartMs) : 0;
    event.drainDurationSec = drainMs / 1000.0f;
    event.fillDurationSec = fillMs / 1000.0f;
    event.source = dilutionSource ? dilutionSource : "manual";
    ecDilutionCallback(&event, ecDilutionCallbackUserData);
}

void HydroControl::finishDilutionSequence(bool success) {
    setDilutionRelay(dilutionDrainRelay, false);
    setDilutionRelay(dilutionFillRelay, false);
    if (success) {
        emitEcDilutionEvent();
        if (tempoRecirculacaoSeconds > 0) {
            dilutionState = DILUTION_RECIRCULATING;
            dilutionStateStartMs = millis();
            notifyPhysicalRecirc(true, "ec");
            Serial.printf("⏳ [DILUTION] Recirc %lu s pós-diluição\n", tempoRecirculacaoSeconds);
            showMessage("Recirc dil");
        } else {
            dilutionState = DILUTION_IDLE;
            dilutionTargetL = 0.0f;
            dilutionProgressL = 0.0f;
            dilutionSequenceId = "";
            showMessage("Diluicao OK");
        }
    } else {
        dilutionState = DILUTION_IDLE;
        dilutionTargetL = 0.0f;
        dilutionProgressL = 0.0f;
        dilutionSequenceId = "";
        showMessage("Dil cancel");
        Serial.println("🛑 [DILUTION] Sequência abortada");
    }
    notifyEcOperationChanged();
}

void HydroControl::processDilution() {
    if (dilutionState == DILUTION_IDLE) {
        return;
    }

    const unsigned long now = millis();

    if (dilutionState == DILUTION_DRAINING) {
        if (flowmeterSensor) {
            dilutionProgressL = flowmeterSensor->consumedLiters();
            const uint32_t pulses = flowmeterSensor->pulseCount();
            if (pulses != dilutionLastPulseCount) {
                dilutionLastPulseCount = pulses;
                dilutionLastPulseMs = now;
            }
        }
        const bool targetReached = dilutionProgressL >= (dilutionTargetL - 0.05f);
        const bool stall = (now - dilutionLastPulseMs) > DILUTION_FLOWMETER_STALL_MS &&
                           dilutionProgressL < 0.05f;
        const unsigned long maxDrainMs = dilutionFillFlowLps > 0.0f
            ? (unsigned long)((dilutionTargetL / dilutionFillFlowLps) * 2000.0f)
            : 120000UL;
        const bool timeFallback = (now - dilutionDrainStartMs) > maxDrainMs;

        if (targetReached) {
            finishDilutionDrainPhase();
        } else if (stall && !timeFallback) {
            Serial.println("⚠️ [DILUTION] Fluxometro sem pulsos — abort");
            finishDilutionSequence(false);
        } else if (timeFallback && dilutionProgressL < 0.05f) {
            Serial.println("⚠️ [DILUTION] Timeout dreno — estimativa por tempo");
            dilutionProgressL = dilutionTargetL;
            finishDilutionDrainPhase();
        }
        return;
    }

    if (dilutionState == DILUTION_FILLING) {
        const unsigned long elapsed = now - dilutionFillStartMs;
        dilutionProgressL = dilutionFillFlowLps > 0.0f
            ? (elapsed / 1000.0f) * dilutionFillFlowLps
            : 0.0f;
        if (elapsed >= dilutionFillDurationMs) {
            setDilutionRelay(dilutionFillRelay, false);
            dilutionProgressL = dilutionDrainMeasuredL;
            Serial.println("✅ [DILUTION] Reposição concluída");
            finishDilutionSequence(true);
        }
        return;
    }

    if (dilutionState == DILUTION_RECIRCULATING) {
        const unsigned long elapsedSec = (now - dilutionStateStartMs) / 1000UL;
        if (elapsedSec >= tempoRecirculacaoSeconds) {
            notifyPhysicalRecirc(false, "ec");
            Serial.println("✅ [DILUTION] Recirculação pós-diluição OK");
            dilutionState = DILUTION_IDLE;
            dilutionTargetL = 0.0f;
            dilutionProgressL = 0.0f;
            dilutionSequenceId = "";
            lastECCheck = now;
            lastECCheckAtMs = now;
            showMessage("Diluicao OK");
            notifyEcOperationChanged();
        }
    }
}

bool HydroControl::processEcSerialCommand(const String& command) {
    if (!ecSensor) {
        return false;
    }
    if (command == "EC CAL 1413") {
        Serial.println("[EC] Iniciando calibracion con solucion 1413 uS/cm...");
        const bool ok = calibrateTDSWithSolution1413();
        Serial.println(ok ? "[EC] Calibracion OK" : "[EC] Calibracion fallida");
        return true;
    }
    if (command.startsWith("EC K ")) {
        const float k = command.substring(5).toFloat();
        setTDSCalibrationFactor(k);
        Serial.printf("[EC] Factor K aplicado: %.4f\n", getTDSCalibrationFactor());
        return true;
    }
    if (command == "EC STATUS") {
        Serial.printf("[EC] GPIO=%u K=%.4f Vmax=%.2f EC=%.0f uS/cm valid=%s\n",
                        EC_SENSOR_ANALOG_PIN,
                        getTDSCalibrationFactor(),
                        getTDSVRef(),
                        ec,
                        ecValid ? "yes" : "no");
        return true;
    }
    return false;
}
