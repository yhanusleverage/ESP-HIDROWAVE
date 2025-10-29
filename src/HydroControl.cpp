#include "HydroControl.h"

HydroControl::HydroControl()
    : lcd(0x27, 16, 2)
    , oneWire(TEMP_PIN)
    , sensors(&oneWire)
    , pcf1(0x20)  // Primeiro PCF8574
    , pcf2(0x24)  // Segundo PCF8574
    , pcf1_ok(false)
    , pcf2_ok(false)
{
    // Inicializa os estados dos relés
    for(int i = 0; i < NUM_RELAYS; i++) {
        relayStates[i] = false;
        startTimes[i] = 0;
        timerSeconds[i] = 0;
    }
    tankSensor = new LevelSensor(23, 32);
}

bool HydroControl::begin() {
    // Inicializar I2C uma única vez
    Wire.begin();
    Serial.println("🔍 Escaneando dispositivos I2C...");
    
    // Scan I2C antes de inicializar os dispositivos
    for(byte i = 8; i < 120; i++) {
        Wire.beginTransmission(i);
        if (Wire.endTransmission() == 0) {
            Serial.printf("✅ Dispositivo I2C encontrado no endereço 0x%02X\n", i);
        }
    }

    // Inicializar LCD sem reiniciar I2C
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
    Serial.println("\n🔌 Iniciando expansores I/O PCF8574...");
    
    // Tentar inicializar PCF1 sem reiniciar I2C
    pcf1_ok = pcf1.begin(false);  // false para não reiniciar I2C
    if (!pcf1_ok) {
        Serial.println("⚠️ Erro ao inicializar PCF8574 #1 (0x20) - Relés 1-7 indisponíveis");
    } else {
        Serial.println("✅ PCF8574 #1 iniciado com sucesso");
        // Configurar pinos como OUTPUT e HIGH (relés desligados)
        for (int i = 0; i < 7; i++) {
            pcf1.digitalWrite(i, HIGH);
        }
    }
    
    // Tentar inicializar PCF2 sem reiniciar I2C
    pcf2_ok = pcf2.begin(false);  // false para não reiniciar I2C
    if (!pcf2_ok) {
        Serial.println("⚠️ Erro ao inicializar PCF8574 #2 (0x24) - Relé 8 indisponível");
    } else {
        Serial.println("✅ PCF8574 #2 iniciado com sucesso");
        // Configurar pinos como OUTPUT e HIGH (relés desligados)
        for (int i = 0; i < 8; i++) {
            pcf2.digitalWrite(i, HIGH);
        }
    }

    // Resetar estados dos relés
    for (int i = 0; i < NUM_RELAYS; i++) {
        relayStates[i] = false;
        startTimes[i] = 0;
        timerSeconds[i] = 0;
    }

    Serial.println("\n🚀 Sistema iniciado" + 
                  String(!pcf1_ok || !pcf2_ok ? " com avisos" : " sem erros"));
    
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
    
    // Debug status
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 5000) {  // A cada 5 segundos
        lastDebug = millis();
        Serial.println("\n=== Status do Sistema ===");
        Serial.printf("Temperatura: %.1f°C\n", temperature);
        Serial.printf("pH: %.2f\n", pH);
        Serial.printf("TDS: %.0f ppm\n", tds);
        Serial.printf("EC: %.0f uS/cm\n", ec);
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
    float tdsReading = tdsSensor->getTDSValue();
    if (tdsReading >= MIN_TDS && tdsReading <= MAX_TDS) {
        tds = tdsReading;
        ec = tdsSensor->getECValue();
        sensorsOk &= true;
    } else {
        Serial.println("⚠️ Valor TDS fora do intervalo válido");
        sensorsOk = false;
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
                (tdsReading >= MIN_TDS && tdsReading <= MAX_TDS) ? "✓" : "✗");
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

void HydroControl::toggleRelay(int relay, int seconds) {
    if (relay < 0 || relay >= NUM_RELAYS) {
        Serial.printf("❌ Relé %d inválido\n", relay + 1);
        return;
    }

    // Verificar disponibilidade do PCF correspondente
    if (relay < 7 && !pcf1_ok) {
        Serial.printf("❌ Relé %d indisponível - PCF8574 #1 offline\n", relay + 1);
        return;
    }
    if (relay >= 7 && !pcf2_ok) {
        Serial.printf("❌ Relé %d indisponível - PCF8574 #2 offline\n", relay + 1);
        return;
    }

    // Atualizar estado do relé
    relayStates[relay] = !relayStates[relay];
    bool state = !relayStates[relay];  // Invertido porque relés são ativos em LOW
    
    bool success = false;
    // Tentar acionar o relé
    try {
        if (relay < 7) {  // Primeiro PCF8574 (P0-P6)
            pcf1.digitalWrite(relay, state);
            success = true;
            Serial.printf("✅ PCF1: Relé %d -> pino P%d = %d\n", relay+1, relay, state);
        } else {          // Segundo PCF8574 (P1-P7)
            int pcf2Pin = (relay - 6);
            pcf2.digitalWrite(pcf2Pin, state);
            success = true;
            Serial.printf("✅ PCF2: Relé %d -> pino P%d = %d\n", relay+1, pcf2Pin, state);
        }
    } catch (...) {
        success = false;
    }

    if (!success) {
        // Reverter estado se falhou
        relayStates[relay] = !relayStates[relay];
        Serial.printf("❌ Erro ao acionar relé %d\n", relay + 1);
        return;
    }
    
    // Configurar timer se necessário
    if (seconds > 0 && relayStates[relay]) {
        startTimes[relay] = millis();
        timerSeconds[relay] = seconds;
        Serial.printf("⏲️ Relé %d ligado por %d segundos\n", relay+1, seconds);
    } else {
        startTimes[relay] = 0;
        timerSeconds[relay] = 0;
    }
}

void HydroControl::checkRelayTimers() {
    unsigned long currentMillis = millis();
    for(int i = 0; i < NUM_RELAYS; i++) {
        if(relayStates[i] && timerSeconds[i] > 0) {
            if((currentMillis - startTimes[i]) / 1000 >= timerSeconds[i]) {
                relayStates[i] = false;
                bool state = !relayStates[i];  // Invertido porque relés são ativos em LOW
                
                // Corrigir o mapeamento dos relés para os PCF8574s
                if (i < 7) {  // Primeiro PCF8574 (P0-P6)
                    pcf1.digitalWrite(i, state);
                    Serial.printf("Timer PCF1: Relé %d -> pino P%d = %d\n", i+1, i, state);
                } else {      // Segundo PCF8574 (P1-P7)
                    int pcf2Pin = (i - 6); // Ajuste para mapear para P1-P7
                    pcf2.digitalWrite(pcf2Pin, state);
                    Serial.printf("Timer PCF2: Relé %d -> pino P%d = %d\n", i+1, pcf2Pin, state);
                }
                
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