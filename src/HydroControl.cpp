#include "HydroControl.h"
#include "PreferencesManager.h"  // ✅ Para persistência em NVS
#include <cmath>

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
    tankSensor = new LevelSensor(23, 32);
    
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
    mlPerPhUnit = 2.0f;
    phRecircSeconds = 60;
    phAutoState = PH_IDLE;
    phStateStartMs = 0;
    phActiveRelay = -1;
    phOperationSyncCallback = nullptr;
    phOperationSyncCallbackUserData = nullptr;
    
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
 * 7. tdsSensor = new TDSReaderSerial() - Sensor TDS
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
    
    // Inicializar sensor de pH
    pHSensor = new phSensor();
    pHSensor->calibrate(2.56, 3.3, 2.05, false);

    // Inicializar sensor TDS
    tdsSensor = new TDSReaderSerial(TDS_PIN, 3.3, 1.0);
    tdsSensor->begin();

    // Inicializar sensor de nível
    tankSensor = new LevelSensor(TANK_LOW_PIN, TANK_HIGH_PIN);
    tankSensor->begin();

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
    
    // Return true if basic initialization succeeded (even with PCF errors)
    return true;
}

void HydroControl::loop() {
    // Call the existing update method
    update();
}

void HydroControl::update() {
    updateSensors();
    updateDisplay();
    checkRelayTimers();
    processPhAutoState();
    checkAutoEC();  // ✅ Verificar e ajustar EC automaticamente
    checkAutoPH();  // ✅ Verificar e ajustar pH automaticamente
    processSimpleSequential();  // ✅ Processar máquina de estados sequencial
}

void HydroControl::updateSensors() {
    sensors.requestTemperatures();
    float tempReading = sensors.getTempCByIndex(0);
    if (tempReading != -127.0 && tempReading >= MIN_TEMP && tempReading <= MAX_TEMP) {
        temperature = tempReading;
        sensorsOk = true;
    } else {
        sensorsOk = false;
    }

    float phReading = pHSensor->readPH(PH_PIN);
    if (phReading >= MIN_PH && phReading <= MAX_PH) {
        pH = phReading;
        sensorsOk &= true;
    } else {
        sensorsOk = false;
    }

    tdsSensor->updateTemperature(temperature);
    tdsSensor->readTDS();
    float tdsReading = tdsSensor->getTDSValue();
    if (tdsReading >= MIN_TDS && tdsReading <= MAX_TDS) {
        tds = tdsReading;
        ec = tdsSensor->getECValue();
        sensorsOk &= true;
    } else {
        sensorsOk = false;
    }

    tankLevelOk = tankSensor->checkWaterLevel();
}

void HydroControl::updateDisplay() {
    lcd.clear();
    
    // Linha 1: Temperatura centralizada
    String tempText = "Temp:" + String(temperature, 1) + char(223) + "C";
    lcd.setCursor((16 - tempText.length()) / 2, 0);
    lcd.print(tempText);
    
    // Linha 2: pH e EC
    lcd.setCursor(0, 1);
    lcd.print("pH:");
    lcd.print(pH, 2);
    
    String ecText = "EC:" + String(ec, 0);
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
        timerSeconds[relay] = seconds / 1000;  // Converter ms para segundos
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

void HydroControl::updateSensorData(float temp, float humidity, float ph, float tds) {
    temperature = temp;
    // humidity não é armazenada na classe atual, mas poderia ser adicionada se necessário
    pH = ph;
    this->tds = tds;
    ec = tds * 2;  // EC = TDS * 2 (aproximação)
    
    // Atualizar display com os novos dados
    updateDisplay();
}

void HydroControl::updateRelayTimers() {
    checkRelayTimers();
}

String HydroControl::getTankStatus() {
    return tankSensor->getStatus();
}

// ✅ Função para verificar e ajustar EC automaticamente
void HydroControl::checkAutoEC() {
    // Se controle automático não está habilitado, não fazer nada
    if (!autoECEnabled) {
        static unsigned long lastDebugPrint = 0;
        unsigned long now = millis();
        if (now - lastDebugPrint >= 30000) {  // Debug a cada 30 segundos
            Serial.println("⚠️ [AUTO EC] auto_enabled = false - ative no frontend primeiro!");
            Serial.printf("   💡 Valores atuais: setpoint=%.0f, ec=%.0f, base_dose=%.2f\n", 
                ecSetpoint, ec, ecController.getBaseDose());
            lastDebugPrint = now;
        }
        return;
    }

    if (currentState != IDLE) {
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
    
    // Verificar se precisa de ajuste (tolerância configurável — default 50 µS/cm)
    if (ecController.needsAdjustment(ecSetpoint, ec, ecTolerance)) {
        // Calcular dosagem necessária
        float dosageML = ecController.calculateDosage(ecSetpoint, ec);
        
        if (dosageML > 0.1) {  // Só dosar se for significativo (> 0.1 ml)
            float dosageTime = ecController.calculateDosageTime(dosageML);
            
            Serial.println("\n🤖 === CONTROLE AUTOMÁTICO EC ===");
            Serial.printf("📊 EC Atual: %.0f µS/cm\n", ec);
            Serial.printf("🎯 EC Setpoint: %.0f µS/cm\n", ecSetpoint);
            Serial.printf("⚡ Erro: %.0f µS/cm\n", (ecSetpoint - ec));
            Serial.printf("💧 u(t) calculado: %.3f ml (proporção milimétrica)\n", dosageML);
            Serial.printf("⏱️ Tempo de dosagem: %.2f segundos\n", dosageTime);
            Serial.printf("⏱️  Tempo de dosagem: %.1f segundos\n", dosageTime);
            Serial.println("================================\n");
            
            // ✅ EXECUTAR DOSAGEM SEQUENCIAL AUTOMÁTICA
            // Verificar se não há dosagem ativa antes de iniciar nova
            if (currentState == IDLE) {
                startSimpleSequentialDosage(dosageML, ecSetpoint, ec);
            } else {
                Serial.println("⚠️  Auto EC: Sistema sequencial já ativo - aguardando conclusão");
            }
            
        } else {
            Serial.printf("ℹ️  Auto EC: Dosagem muito pequena (%.3f ml) - ignorada\n", dosageML);
        }
    } else {
        // Log ocasional quando não precisa ajuste
        static unsigned long lastNoAdjustLog = 0;
        if (currentMillis - lastNoAdjustLog > 60000) {  // Log a cada 1 minuto
            lastNoAdjustLog = currentMillis;
            float error = abs(ecSetpoint - ec);
            Serial.printf("✅ Auto EC: Sem ajuste necessário (Erro: %.0f µS/cm, Tolerância: %.0f µS/cm)\n", error, ecTolerance);
        }
    }
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
    if (currentState != IDLE) {
        Serial.println("\n🛑 CANCELANDO DOSAGEM SEQUENCIAL EM ANDAMENTO...");
        
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
    
    Serial.println("✅ EC_CONFIG carregado e aplicado com sucesso");
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

void HydroControl::notifyEcOperationChanged() {
    if (ecOperationSyncCallback) {
        ecOperationSyncCallback(ecOperationSyncCallbackUserData);
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
    if (!autoECEnabled || currentState != IDLE) {
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

void HydroControl::setPhPumpConfig(int relayUp, int relayDown, float flowUp, float flowDown, float mlPerUnit) {
    relayPhUp = relayUp;
    relayPhDown = relayDown;
    flowRatePhUp = flowUp > 0 ? flowUp : 1.0f;
    flowRatePhDown = flowDown > 0 ? flowDown : 1.0f;
    mlPerPhUnit = mlPerUnit > 0 ? mlPerUnit : 2.0f;
}

void HydroControl::setPhOperationSyncCallback(PhOperationSyncCallback cb, void* userData) {
    phOperationSyncCallback = cb;
    phOperationSyncCallbackUserData = userData;
}

void HydroControl::notifyPhOperationChanged() {
    if (phOperationSyncCallback) {
        phOperationSyncCallback(phOperationSyncCallbackUserData);
    }
}

void HydroControl::startPhAutoDosage(int relay, float durationSec) {
    if (relay < 0 || relay >= 8 || durationSec < 0.5f) return;
    if (currentState != IDLE) {
        Serial.println("⚠️ [AUTO PH] EC sequencial ativo — adiando dosagem pH");
        return;
    }
    int sec = max(1, (int)round(durationSec));
    toggleRelay(relay, sec);
    phAutoState = PH_DOSING;
    phActiveRelay = relay;
    phStateStartMs = millis();
    Serial.printf("🧪 [AUTO PH] Dosagem relé %d por %d s\n", relay + 1, sec);
    notifyPhOperationChanged();
}

void HydroControl::processPhAutoState() {
    if (phAutoState == PH_IDLE) return;
    unsigned long now = millis();
    if (phAutoState == PH_DOSING) {
        int durationMs = (phActiveRelay >= 0 && phActiveRelay < NUM_RELAYS)
            ? timerSeconds[phActiveRelay] * 1000
            : 0;
        unsigned long elapsed = now - phStateStartMs;
        if (elapsed >= (unsigned long)max(durationMs, 1000)) {
            phAutoState = PH_RECIRCULATING;
            phStateStartMs = now;
            Serial.printf("⏳ [AUTO PH] Recirculando %lu s\n", phRecircSeconds);
            notifyPhOperationChanged();
        }
    } else if (phAutoState == PH_RECIRCULATING) {
        if ((now - phStateStartMs) >= phRecircSeconds * 1000UL) {
            phAutoState = PH_IDLE;
            phActiveRelay = -1;
            lastPHCheck = now;
            lastPHCheckAtMs = now;
            Serial.println("✅ [AUTO PH] Recirculação concluída");
            notifyPhOperationChanged();
        }
    }
}

void HydroControl::checkAutoPH() {
    if (!autoPHEnabled) return;
    if (phAutoState != PH_IDLE) return;
    if (currentState != IDLE) return;

    unsigned long now = millis();
    unsigned long checkInterval = autoPHIntervalSeconds > 0
        ? (autoPHIntervalSeconds * 1000UL) : 300000UL;
    if (now - lastPHCheck < checkInterval) return;

    lastPHCheck = now;
    lastPHCheckAtMs = now;

    if (!phController.needsAdjustment(phSetpoint, pH, phTolerance)) {
        return;
    }

    int direction = phController.getDirection(phSetpoint, pH, phTolerance);
    if (direction == 0) return;

    float ml = phController.calculateDosageMl(phSetpoint, pH, mlPerPhUnit);
    if (ml < 0.1f) return;

    int relay = direction > 0 ? relayPhUp : relayPhDown;
    float flow = direction > 0 ? flowRatePhUp : flowRatePhDown;
    float durationSec = phController.calculateDosageTime(ml, flow);

    Serial.println("\n🧪 === CONTROLE AUTOMÁTICO pH ===");
    Serial.printf("📊 pH Atual: %.2f | Setpoint: %.2f | ml: %.3f\n", pH, phSetpoint, ml);
    startPhAutoDosage(relay, durationSec);
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

int HydroControl::getPhOperationRemainingSec() const {
    if (phAutoState == PH_IDLE) return 0;
    unsigned long elapsed = (millis() - phStateStartMs) / 1000UL;
    if (phAutoState == PH_DOSING && phActiveRelay >= 0) {
        long rem = (long)timerSeconds[phActiveRelay] - (long)elapsed;
        return rem > 0 ? (int)rem : 0;
    }
    if (phAutoState == PH_RECIRCULATING) {
        long rem = (long)phRecircSeconds - (long)elapsed;
        return rem > 0 ? (int)rem : 0;
    }
    return 0;
}

int HydroControl::getPhNextCheckInSec() const {
    if (!autoPHEnabled || phAutoState != PH_IDLE || currentState != IDLE) return 0;
    unsigned long checkInterval = autoPHIntervalSeconds > 0
        ? (autoPHIntervalSeconds * 1000UL) : 300000UL;
    if (lastPHCheckAtMs == 0) return (int)(checkInterval / 1000UL);
    unsigned long elapsed = millis() - lastPHCheckAtMs;
    if (elapsed >= checkInterval) return 0;
    return (int)((checkInterval - elapsed) / 1000UL);
}
