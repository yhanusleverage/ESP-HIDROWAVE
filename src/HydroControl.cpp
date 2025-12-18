#include "HydroControl.h"
#include "PreferencesManager.h"  // ✅ Para persistência em NVS

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
    autoECEnabled = false;
    lastECCheck = 0;
    autoECIntervalSeconds = 5;  // ✅ Padrão: 5 segundos
    ecValid = false;  // ✅ EC não é válido até que o buffer TDS esteja cheio
    
    // ✅ TEMPO MORTO (recirculação) - Aguardar após dosagem
    lastDosageCompleteTime = 0;
    tempoRecirculacao = 60;  // Padrão: 60 segundos
    
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
    
    // ✅ Inicializar Task de dosagem
    dosingTaskHandle = nullptr;
    dosingMutex = nullptr;
    dosingTaskRunning = false;
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
    // ✅ Usar TDS_VREF de Config.h (3.3V - ajustado para leituras mais precisas)
    // ✅ Usar TDS_CALIBRATION_FACTOR de Config.h
    tdsSensor = new TDSReaderSerial(TDS_PIN, TDS_VREF, TDS_CALIBRATION_FACTOR);
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
    
    // ✅ Criar mutex para sincronização da Task de dosagem
    dosingMutex = xSemaphoreCreateMutex();
    if (dosingMutex == nullptr) {
        Serial.println("❌ [DOSAGEM] Falha ao criar mutex!");
    } else {
        Serial.println("✅ [DOSAGEM] Mutex criado para Task dedicada");
    }
    
    // ✅ Criar Task dedicada para dosagem (Core 1, prioridade alta)
    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        dosingTaskFunction,     // Função da task
        "DosingTask",           // Nome
        4096,                   // Stack size
        this,                   // Parâmetro (ponteiro para HydroControl)
        3,                      // Prioridade ALTA (maior que loop principal)
        &dosingTaskHandle,      // Handle
        1                       // Core 1 (mesmo que sensores)
    );
    
    if (taskCreated == pdPASS) {
        Serial.println("✅ [DOSAGEM] Task dedicada criada (Core 1, Prioridade 3)");
        Serial.println("   💡 Timing preciso garantido - independente do loop principal");
    } else {
        Serial.println("❌ [DOSAGEM] Falha ao criar Task dedicada!");
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
    updateDisplay();
    checkRelayTimers();
    checkAutoEC();  // ✅ Verificar e ajustar EC automaticamente
    processSimpleSequential();  // ✅ Processar máquina de estados sequencial
    
    // Debug status
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 5000) {  // A cada 5 segundos
        lastDebug = millis();
        Serial.println("\n=== Status do Sistema ===");
        Serial.printf("Temperatura: %.1f°C\n", temperature);
        Serial.printf("pH: %.2f\n", pH);
        // ✅ Mostrar TDS/EC (0.0 si buffer no está listo)
        Serial.printf("TDS: %.1f ppm\n", tds);  // ✅ Siempre mostrar (0.0 si buffer no listo)
        Serial.printf("EC: %.1f uS/cm\n", ec);   // ✅ Siempre mostrar (0.0 si buffer no listo)
        if (!ecValid) {
            Serial.println("   ⏳ Buffer EC inicializando...");
        }
        Serial.println("Estado dos Relés:");
        for (int i = 0; i < NUM_RELAYS; i++) {
            Serial.printf("Relé %d: %s\n", i+1, relayStates[i] ? "ON" : "OFF");
        }
        Serial.println("=====================\n");
    }
}

void HydroControl::updateSensors() {
    static unsigned long lastErrorPrint = 0;  // Controlar prints de erro
    bool shouldPrintError = (millis() - lastErrorPrint > 5000);  // A cada 5 segundos
    
    // Temperatura
    sensors.requestTemperatures();
    float tempReading = sensors.getTempCByIndex(0);
    if (tempReading != -127.0 && tempReading >= MIN_TEMP && tempReading <= MAX_TEMP) {
        temperature = tempReading;
        sensorsOk = true;
    } else {
        if (shouldPrintError) {
            Serial.println("⚠️ Erro na leitura da temperatura");
        }
        sensorsOk = false;
    }
    
    // pH
    float phReading = pHSensor->readPH(PH_PIN);
    if (phReading >= MIN_PH && phReading <= MAX_PH) {
        pH = phReading;
        sensorsOk &= true;
    } else {
        if (shouldPrintError) {
            Serial.println("⚠️ Erro na leitura do pH");
        }
        sensorsOk = false;
    }
    
    // TDS e EC
    tdsSensor->updateTemperature(temperature);
    tdsSensor->readTDS();
    
    // ✅ Verificar se o buffer TDS está pronto ANTES de usar valores
    bool bufferReady = tdsSensor->isBufferReady();
    
    // ✅ Contador de leituras estáveis após buffer pronto (precisa de 2 leituras)
    static int stableReadingsCount = 0;
    static bool ecStabilized = false;
    
    // ⚠️ Si buffer no está listo, NO usar valores (evita mostrar basura)
    if (!bufferReady) {
        static unsigned long lastBufferWarning = 0;
        if (millis() - lastBufferWarning > 3000) {
            Serial.println("⏳ [EC] Buffer inicializando... aguarde");
            lastBufferWarning = millis();
        }
        stableReadingsCount = 0;
        ecStabilized = false;
        ecValid = false;
        // ✅ FORZAR valores a 0 mientras buffer no está listo
        tds = 0;
        ec = 0;
    } else {
        float tdsReading = tdsSensor->getTDSValue();
        
        if (tdsReading >= MIN_TDS && tdsReading <= MAX_TDS && tdsReading > 0) {
            tds = tdsReading;
            ec = tdsSensor->getECValue();
            sensorsOk &= true;
            
            // ✅ Contar leituras estáveis após buffer pronto
            if (!ecStabilized) {
                stableReadingsCount++;
                if (stableReadingsCount >= 2) {
                    ecStabilized = true;
                    ecValid = true;
                    Serial.println("✅ [EC] Leitura estabilizada após 2 amostras - automação EC liberada!");
                    Serial.printf("   📊 EC atual: %.0f µS/cm (estável)\n", ec);
                } else {
                    ecValid = false;  // Ainda não estável
                    Serial.printf("⏳ [EC] Estabilizando... leitura %d/2 (EC: %.0f µS/cm)\n", stableReadingsCount, ec);
                }
            } else {
                ecValid = true;  // Já estabilizado
            }
        } else {
            // ✅ Melhor diagnóstico de erros
            static unsigned long lastErrorLog = 0;
            if (millis() - lastErrorLog > 5000) {
                if (tdsReading == 0.0) {
                    Serial.println("⚠️ [TDS/EC] Sensor não está lendo valores (TDS = 0)");
                } else {
                    Serial.printf("⚠️ [TDS/EC] Valor fora do intervalo: %.2f ppm\n", tdsReading);
                }
                lastErrorLog = millis();
            }
            sensorsOk = false;
            stableReadingsCount = 0;  // Reset se leitura inválida
            ecStabilized = false;
            ecValid = false;
        }
    }

    // Nível do reservatório
    String tankStatus = tankSensor->getStatus();
    tankLevelOk = tankSensor->checkWaterLevel();
    
    // Log detalhado (solo si debe imprimir o si todo está OK)
    if (sensorsOk) {
        if (shouldPrintError) {  // Imprimir OK también cada 5 segundos
            Serial.println("\n✅ Leitura dos sensores OK:");
            Serial.printf("  Temperatura: %.1f°C\n", temperature);
            Serial.printf("  pH: %.2f\n", pH);
            Serial.printf("  TDS: %.0f ppm\n", tds);
            Serial.printf("  EC: %.0f µS/cm\n", ec);
            Serial.println("  Nível: " + tankStatus);
            lastErrorPrint = millis();  // Actualizar timestamp
        }
    } else {
        if (shouldPrintError) {  // Solo imprimir errores cada 5 segundos
            Serial.println("\n⚠️ Problemas na leitura dos sensores:");
            Serial.printf("  Temperatura: %.1f°C %s\n", temperature, 
                (tempReading >= MIN_TEMP && tempReading <= MAX_TEMP) ? "✓" : "✗");
            Serial.printf("  pH: %.2f %s\n", pH,
                (phReading >= MIN_PH && phReading <= MAX_PH) ? "✓" : "✗");
            Serial.printf("  TDS: %.0f ppm %s\n", tds,
                (tds >= MIN_TDS && tds <= MAX_TDS) ? "✓" : "✗");
            Serial.printf("  EC: %.0f µS/cm\n", ec);
            Serial.println("  Nível: " + tankStatus);
            lastErrorPrint = millis();  // Actualizar timestamp
        }
    }
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
    
    // ✅ TEMPO MORTO (recirculação) - Aguardar após dosagem antes de medir EC novamente
    if (lastDosageCompleteTime > 0 && tempoRecirculacao > 0) {
        unsigned long elapsedSeconds = (millis() - lastDosageCompleteTime) / 1000;
        if (elapsedSeconds < tempoRecirculacao) {
            static unsigned long lastRecircLog = 0;
            if (millis() - lastRecircLog > 10000) {  // Log a cada 10 segundos
                Serial.printf("⏸️ [AUTO EC] Tempo morto: %lu/%lu seg (aguardando recirculação)\n", 
                    elapsedSeconds, tempoRecirculacao);
                lastRecircLog = millis();
            }
            return;  // Ainda em tempo morto - não medir EC
        } else {
            // Tempo morto terminou - resetar flag
            lastDosageCompleteTime = 0;
            Serial.println("✅ [AUTO EC] Tempo morto concluído - retomando verificação de EC");
        }
    }
    
    // ✅ MARGEM DE SEGURANÇA: Se EC setpoint < 50, não ativar automação
    // Esto evita activar dosificación cuando el setpoint no está configurado correctamente
    if (ecSetpoint < 50.0) {
        static unsigned long lastLowSetpointLog = 0;
        unsigned long now = millis();
        if (now - lastLowSetpointLog >= 30000) {  // Log a cada 30 segundos
            Serial.println("⚠️ [AUTO EC] EC setpoint muito baixo - automação pausada");
            Serial.printf("   📊 EC setpoint atual: %.0f µS/cm (mínimo: 50 µS/cm)\n", ecSetpoint);
            Serial.println("   💡 Configure um setpoint >= 50 µS/cm no frontend para ativar");
            lastLowSetpointLog = now;
        }
        return;
    }
    
    // Verificar intervalo de verificação
    unsigned long currentMillis = millis();
    unsigned long checkInterval = autoECIntervalSeconds > 0 ? 
        (autoECIntervalSeconds * 1000) : EC_CHECK_INTERVAL;
    
    if (currentMillis - lastECCheck < checkInterval) {
        return;  // Ainda não é hora de verificar
    }
    
    lastECCheck = currentMillis;
    
    // ✅ VALIDAÇÃO CRÍTICA: Não tomar decisões se EC não é confiável
    if (!ecValid) {
        static unsigned long lastInvalidLog = 0;
        if (currentMillis - lastInvalidLog > 5000) {  // Log a cada 5 segundos
            Serial.println("⏸️  [AUTO EC] Pausado - EC não é confiável (buffer TDS ainda não está cheio)");
            Serial.println("   💡 Aguardando estabilização do sensor...");
            lastInvalidLog = currentMillis;
        }
        return;  // Não fazer nada até que EC seja confiável
    }
    
    // ✅ VALIDAÇÃO: Não tomar decisões se EC = 0 (sensor provavelmente não está funcionando)
    if (ec <= 0.0) {
        static unsigned long lastZeroLog = 0;
        if (currentMillis - lastZeroLog > 10000) {  // Log a cada 10 segundos
            Serial.println("⚠️  [AUTO EC] EC = 0 - sensor pode estar com problema");
            Serial.println("   💡 Verifique conexão do sensor TDS");
            lastZeroLog = currentMillis;
        }
        return;  // Não fazer nada se EC = 0
    }
    
    // Verificar se precisa de ajuste (tolerância padrão: 50 µS/cm)
    if (ecController.needsAdjustment(ecSetpoint, ec, 50.0)) {
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
            Serial.printf("✅ Auto EC: Sem ajuste necessário (Erro: %.0f µS/cm, Tolerância: 50 µS/cm)\n", error);
        }
    }
}

// ✅ Máquina de estados para dosagem sequencial
// ⚠️ NOTA: Esta função agora é apenas para compatibilidade
// O processamento real é feito pela Task dedicada (dosingTaskFunction)
void HydroControl::processSimpleSequential() {
    // ✅ Task dedicada processa a dosagem - esta função não faz mais nada
    // Mantida apenas para compatibilidade com chamadas existentes
    return;
    
    unsigned long currentTime = millis();
    
    if (currentState == DOSING) {
        // ===== DOSANDO NUTRIENTE ATUAL =====
        SimpleNutrient& current = nutrients[currentNutrientIndex];
        
        // Verificar tempo decorrido
        unsigned long elapsedTime = currentTime - stateStartTime;
        
        // ✅ Progress log cada 1 segundo
        static unsigned long lastProgressLog = 0;
        if (currentTime - lastProgressLog >= 1000) {
            float progress = (elapsedTime * 100.0) / current.durationMs;
            Serial.printf("   ⏱️ [%s] %lu/%lums (%.0f%%) - Relé %d ATIVO\n", 
                current.name.c_str(), elapsedTime, current.durationMs, progress, current.relay + 1);
            lastProgressLog = currentTime;
        }
        
        // Verificar se terminou a dosagem
        if (elapsedTime >= current.durationMs) {
            // ===== DESLIGAR RELÉ IMEDIATAMENTE =====
            relayStates[current.relay] = false;
            bool state = !relayStates[current.relay];  // Invertido: LOW = ligado, HIGH = desligado
            
            // ✅ TODOS os relés estão no PCF2 (0x24) - mapeamento direto
            if (current.relay >= 0 && current.relay < 8) {
                pcf2.write(current.relay, state);
            }
            
            Serial.println("╔════════════════════════════════════════╗");
            Serial.printf("║ ✅ [DOSAGEM COMPLETA] %s\n", current.name.c_str());
            Serial.printf("║    📊 Volume: %.3f ml\n", current.dosageML);
            Serial.printf("║    ⏱️  Duração: %.3f segundos\n", current.durationMs / 1000.0);
            Serial.printf("║    🔌 Relé %d → DESLIGADO\n", current.relay + 1);
            Serial.println("╚════════════════════════════════════════╝");
            
            // ===== PRÓXIMO NUTRIENTE OU INTERVALO =====
            currentNutrientIndex++;
            
            if (currentNutrientIndex >= totalNutrients) {
                // ===== TERMINOU TODOS OS NUTRIENTES =====
                Serial.println("\n🎉 ════════════════════════════════════════");
                Serial.println("🎉 SEQUÊNCIA COMPLETA - TODOS OS NUTRIENTES!");
                Serial.printf("🎉 Total dosado: %d nutrientes\n", totalNutrients);
                Serial.printf("🎉 Iniciando tempo morto: %lu segundos\n", tempoRecirculacao);
                Serial.println("🎉 ════════════════════════════════════════\n");
                
                // ✅ MARCAR FIM DA DOSAGEM para iniciar tempo morto
                lastDosageCompleteTime = millis();
                
                currentState = IDLE;
                totalNutrients = 0;
                currentNutrientIndex = 0;
                showMessage("Sequencia OK!");
            } else {
                // ===== AGUARDAR INTERVALO ANTES DO PRÓXIMO =====
                currentState = WAITING;
                stateStartTime = currentTime;
                Serial.printf("\n⏳ [INTERVALO] Aguardando %ds antes do próximo nutriente (%d/%d)...\n", 
                    intervalSeconds, currentNutrientIndex + 1, totalNutrients);
                showMessage("Aguardando...");
            }
        }
        
    } else if (currentState == WAITING) {
        // ===== AGUARDANDO INTERVALO CONFIGURADO =====
        unsigned long elapsed = currentTime - stateStartTime;
        unsigned long target = intervalSeconds * 1000;
        
        // ✅ Progress log cada 2 segundos durante espera
        static unsigned long lastWaitLog = 0;
        if (currentTime - lastWaitLog >= 2000 && elapsed < target) {
            Serial.printf("   ⏳ [ESPERA] %lu/%lums restantes...\n", target - elapsed, target);
            lastWaitLog = currentTime;
        }
        
        if (elapsed >= target) {
            // ===== INICIAR PRÓXIMO NUTRIENTE =====
            SimpleNutrient& next = nutrients[currentNutrientIndex];
            
            Serial.println("\n╔════════════════════════════════════════╗");
            Serial.printf("║ 🚀 [INICIANDO DOSAGEM] %s\n", next.name.c_str());
            Serial.printf("║    📊 Volume: %.3f ml\n", next.dosageML);
            Serial.printf("║    ⏱️  Duração: %.3f segundos\n", next.durationMs / 1000.0);
            Serial.printf("║    🔌 Relé %d → LIGANDO\n", next.relay + 1);
            Serial.printf("║    📍 Progresso: %d/%d nutrientes\n", currentNutrientIndex + 1, totalNutrients);
            Serial.println("╚════════════════════════════════════════╝");
            
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
        
        // Distribuir u(t) proporcionalmente para cada nutriente
        for (int i = 0; i < 16 && totalNutrients < 8; i++) {
            if (!dynamicProportions[i].active || dynamicProportions[i].mlPerLiter <= 0.0) {
                continue;  // Pular nutrientes inativos
            }
            
            // ✅ CALCULAR DOSAGEM PROPORCIONAL
            // dosagemNutriente = u(t) × (mlPerLiter / totalMlPerLiter)
            float proportion = dynamicProportions[i].proportion;
            float nutDosage = totalML * proportion;
            float nutTime = nutDosage / ecController.getFlowRate();
            int durationMs = (int)(nutTime * 1000);
            
            if (durationMs < 100) durationMs = 100; // Mínimo 100ms
            
            if (nutDosage > 0.001) {
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
        
        Serial.println("\n╔════════════════════════════════════════════════════╗");
        Serial.println("║ 🚀 [INICIANDO SEQUÊNCIA DE DOSAGEM]                 ║");
        Serial.println("╠════════════════════════════════════════════════════╣");
        Serial.printf("║ 🎯 EC Atual: %.0f µS/cm → Setpoint: %.0f µS/cm\n", ecActual, ecSetpoint);
        Serial.printf("║ 📊 Total a dosar: %.3f ml\n", totalML);
        Serial.printf("║ 🧪 Nutrientes na fila: %d\n", totalNutrients);
        Serial.println("╠════════════════════════════════════════════════════╣");
        Serial.printf("║ ▶️  PRIMEIRO: %s\n", first.name.c_str());
        Serial.printf("║    📊 Volume: %.3f ml\n", first.dosageML);
        Serial.printf("║    ⏱️  Duração: %.3f segundos\n", first.durationMs / 1000.0);
        Serial.printf("║    🔌 Relé %d → LIGANDO\n", first.relay + 1);
        Serial.println("╚════════════════════════════════════════════════════╝\n");
        
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
    } else {
        Serial.println("ℹ️  Nenhuma dosagem ativa para cancelar");
    }
}

// ✅ Atualizar proporções dinâmicas da tabela nutricional (recebido do frontend)
void HydroControl::updateNutrientProportions(JsonArray nutrients) {
    // Limpar proporções anteriores
    activeNutrientsCount = 0;
    totalMlPerLiter = 0.0;
    
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
    bool autoEnabled = false;
    int intervalSeconds = 30;
    
    PreferencesManager::loadConfigFloat("ec_baseDose", baseDose);
    PreferencesManager::loadConfigFloat("ec_flowRate", flowRate);
    PreferencesManager::loadConfigFloat("ec_volume", volume);
    PreferencesManager::loadConfigFloat("ec_totalMl", totalMl);
    PreferencesManager::loadConfigFloat("ec_kp", kp);
    PreferencesManager::loadConfigFloat("ec_setpoint", setpoint);
    PreferencesManager::loadConfigInt("ec_autoEnabled", (int32_t&)autoEnabled);
    PreferencesManager::loadConfigInt("ec_interval", (int32_t&)intervalSeconds);
    
    // Carregar tempo de recirculação
    int32_t recirculacaoTemp = 60;  // Default 60 segundos
    PreferencesManager::loadConfigInt("ec_recirculacao", recirculacaoTemp);
    tempoRecirculacao = (unsigned long)recirculacaoTemp;
    
    // Mostrar valores carregados
    Serial.println("📊 Valores carregados do NVS:");
    Serial.printf("   • base_dose:        %.2f µS/cm\n", baseDose);
    Serial.printf("   • flow_rate:        %.3f ml/s\n", flowRate);
    Serial.printf("   • volume:           %.2f L\n", volume);
    Serial.printf("   • total_ml:         %.2f ml/L\n", totalMl);
    Serial.printf("   • kp:               %.2f\n", kp);
    Serial.printf("   • ec_setpoint:      %.0f µS/cm\n", setpoint);
    Serial.printf("   • auto_enabled:     %s\n", autoEnabled ? "true" : "false");
    Serial.printf("   • intervalo_auto_ec: %d segundos\n", intervalSeconds);
    Serial.printf("   • tempo_recirculacao: %lu segundos\n", tempoRecirculacao);
    
    // Aplicar valores carregados
    if (baseDose > 0.0) ecController.setBaseDose(baseDose);
    if (flowRate > 0.0) ecController.setFlowRate(flowRate);
    if (volume > 0.0) ecController.setVolume(volume);
    if (totalMl > 0.0) ecController.setTotalMl(totalMl);
    if (kp > 0.0) ecController.setKp(kp);
    if (setpoint > 0.0) ecSetpoint = setpoint;
    autoECEnabled = autoEnabled;
    if (intervalSeconds > 0) autoECIntervalSeconds = intervalSeconds;
    
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
    Serial.printf("   • auto_enabled:     %s\n", autoEnabled ? "true" : "false");
    Serial.printf("   • intervalo_auto_ec: %d segundos\n", interval);
    Serial.printf("   • tempo_recirculacao: %lu segundos\n", tempoRecirculacao);
    
    // Guardar en NVS
    bool success = true;
    success &= PreferencesManager::saveConfigFloat("ec_baseDose", baseDose);
    success &= PreferencesManager::saveConfigFloat("ec_flowRate", flowRate);
    success &= PreferencesManager::saveConfigFloat("ec_volume", volume);
    success &= PreferencesManager::saveConfigFloat("ec_totalMl", totalMl);
    success &= PreferencesManager::saveConfigFloat("ec_kp", kp);
    success &= PreferencesManager::saveConfigFloat("ec_setpoint", setpoint);
    success &= PreferencesManager::saveConfigInt("ec_autoEnabled", autoEnabled ? 1 : 0);
    success &= PreferencesManager::saveConfigInt("ec_interval", interval);
    success &= PreferencesManager::saveConfigInt("ec_recirculacao", (int)tempoRecirculacao);
    
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
        saveECControllerConfig();  // ✅ Salvar automaticamente no NVS
    }
}

void HydroControl::setAutoECEnabled(bool enabled, bool saveToNVS) {
    autoECEnabled = enabled;
    if (saveToNVS) {
        saveECControllerConfig();  // ✅ Salvar automaticamente no NVS
    }
}

void HydroControl::setAutoECInterval(int intervalSeconds, bool saveToNVS) {
    autoECIntervalSeconds = intervalSeconds;
    if (saveToNVS) {
        saveECControllerConfig();  // ✅ Salvar automaticamente no NVS
    }
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

// ✅ TASK DEDICADA PARA DOSAGEM - Timing preciso independente do loop principal
void HydroControl::dosingTaskFunction(void* parameter) {
    HydroControl* self = static_cast<HydroControl*>(parameter);
    
    // ✅ CRÍTICO: Esperar mutex ser válido antes de começar
    while (self->dosingMutex == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));  // Esperar 100ms
    }
    
    // ✅ Esperar um pouco mais para garantir que tudo está inicializado
    vTaskDelay(pdMS_TO_TICKS(500));
    
    Serial.println("🚀 [DOSING TASK] Task iniciada - mutex válido, aguardando comandos");
    self->dosingTaskRunning = true;
    
    while (true) {
        // Processar dosagem apenas se ativa E mutex válido
        if (self->currentState != IDLE && self->dosingMutex != nullptr) {
            self->processDosingTask();
            vTaskDelay(pdMS_TO_TICKS(20));  // 20ms durante dosagem (50 Hz)
        } else {
            vTaskDelay(pdMS_TO_TICKS(100)); // 100ms quando idle (economiza CPU/memória)
        }
    }
}

// ✅ Processamento da dosagem na Task dedicada
void HydroControl::processDosingTask() {
    // Adquirir mutex para acesso seguro
    if (xSemaphoreTake(dosingMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;  // Não conseguiu mutex, tentar novamente
    }
    
    unsigned long currentTime = millis();
    
    if (currentState == DOSING) {
        // ===== DOSANDO NUTRIENTE ATUAL =====
        SimpleNutrient& current = nutrients[currentNutrientIndex];
        unsigned long elapsedTime = currentTime - stateStartTime;
        
        // ✅ Verificar se terminou a dosagem
        if (elapsedTime >= current.durationMs) {
            // ===== DESLIGAR RELÉ IMEDIATAMENTE =====
            relayStates[current.relay] = false;
            bool state = !relayStates[current.relay];  // Invertido: LOW = ligado, HIGH = desligado
            
            if (current.relay >= 0 && current.relay < 8) {
                pcf2.write(current.relay, state);
            }
            
            Serial.println("╔════════════════════════════════════════╗");
            Serial.printf("║ ✅ [TASK] DOSAGEM COMPLETA: %s\n", current.name.c_str());
            Serial.printf("║    📊 Volume: %.3f ml\n", current.dosageML);
            Serial.printf("║    ⏱️  Real: %lu ms / Esperado: %lu ms\n", elapsedTime, current.durationMs);
            Serial.printf("║    📈 Precisão: %.1f%%\n", (current.durationMs * 100.0) / elapsedTime);
            Serial.printf("║    🔌 Relé %d → DESLIGADO\n", current.relay + 1);
            Serial.println("╚════════════════════════════════════════╝");
            
            // ===== PRÓXIMO NUTRIENTE OU INTERVALO =====
            currentNutrientIndex++;
            
            if (currentNutrientIndex >= totalNutrients) {
                Serial.println("\n🎉 ════════════════════════════════════════");
                Serial.println("🎉 [TASK] SEQUÊNCIA COMPLETA!");
                Serial.printf("🎉 Total dosado: %d nutrientes\n", totalNutrients);
                Serial.printf("🎉 Iniciando tempo morto: %lu segundos\n", tempoRecirculacao);
                Serial.println("🎉 ════════════════════════════════════════\n");
                
                // ✅ MARCAR FIM DA DOSAGEM para iniciar tempo morto
                lastDosageCompleteTime = millis();
                
                currentState = IDLE;
                totalNutrients = 0;
                currentNutrientIndex = 0;
            } else {
                currentState = WAITING;
                stateStartTime = currentTime;
                Serial.printf("\n⏳ [TASK] Intervalo: %ds antes do nutriente %d/%d\n", 
                    intervalSeconds, currentNutrientIndex + 1, totalNutrients);
            }
        }
        
    } else if (currentState == WAITING) {
        // ===== AGUARDANDO INTERVALO =====
        unsigned long elapsed = currentTime - stateStartTime;
        unsigned long target = intervalSeconds * 1000;
        
        if (elapsed >= target) {
            // ===== INICIAR PRÓXIMO NUTRIENTE =====
            SimpleNutrient& next = nutrients[currentNutrientIndex];
            
            Serial.println("\n╔════════════════════════════════════════╗");
            Serial.printf("║ 🚀 [TASK] INICIANDO: %s\n", next.name.c_str());
            Serial.printf("║    📊 Volume: %.3f ml\n", next.dosageML);
            Serial.printf("║    ⏱️  Duração: %lu ms\n", next.durationMs);
            Serial.printf("║    🔌 Relé %d → LIGANDO\n", next.relay + 1);
            Serial.printf("║    📍 Progresso: %d/%d\n", currentNutrientIndex + 1, totalNutrients);
            Serial.println("╚════════════════════════════════════════╝");
            
            // ===== LIGAR RELÉ =====
            relayStates[next.relay] = true;
            bool state = !relayStates[next.relay];
            
            if (next.relay >= 0 && next.relay < 8) {
                pcf2.write(next.relay, state);
            }
            
            currentState = DOSING;
            stateStartTime = currentTime;
        }
    }
    
    xSemaphoreGive(dosingMutex);
}