#include "HydroControl.h"

HydroControl::HydroControl()
    : lcd(0x27, 16, 2)
    , oneWire(TEMP_PIN)
    , sensors(&oneWire)
    , pcf1(0x20)  // Primeiro PCF8574
    , pcf2(0x24)  // Segundo PCF8574
{
    // Inicializa os estados dos relés
    for(int i = 0; i < NUM_RELAYS; i++) {
        relayStates[i] = false;
        startTimes[i] = 0;
        timerSeconds[i] = 0;
    }
}

void HydroControl::begin() {
    // Scan I2C
    Serial.println("Scanning I2C devices...");
    Wire.begin();
    for(byte i = 8; i < 120; i++) {
        Wire.beginTransmission(i);
        if (Wire.endTransmission() == 0) {
            Serial.printf("I2C device found at address 0x%02X\n", i);
        }
    }

    // Inicializar PCF8574s
    if (!pcf1.begin()) {
        Serial.println("Erro ao inicializar PCF8574 #1 (0x20)");
    } else {
        Serial.println("PCF8574 #1 inicializado com sucesso");
    }
    
    if (!pcf2.begin()) {
        Serial.println("Erro ao inicializar PCF8574 #2 (0x24)");
    } else {
        Serial.println("PCF8574 #2 inicializado com sucesso");
    }

    // Configurar todos os pinos como OUTPUT e HIGH (relés desligados)
    for (int i = 0; i < 8; i++) {
        pcf1.write(i, HIGH);
        pcf2.write(i, HIGH);
    }

    // Inicializar LCD
    lcd.init();
    lcd.backlight();
    lcd.print("Iniciando...");

    // Inicializar sensores
    sensors.begin();
    
    tdsSensor = new TDSReaderSerial(TDS_PIN, 3.3, 1.0);
    tdsSensor->begin();
    
    pHSensor = new phSensor();
    pHSensor->calibrate(2.56, 3.3, 2.05, false);

    thingSpeak.begin();
}

void HydroControl::update() {
    updateSensors();
    updateDisplay();
    checkRelayTimers();
    
    // Enviar dados para o ThingSpeak
    thingSpeak.sendData(temperature, pH, ec);
    
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
    // Temperatura
    sensors.requestTemperatures();
    float tempReading = sensors.getTempCByIndex(0);
    if (tempReading != -127.0 && tempReading >= 0 && tempReading <= 50) {
        temperature = tempReading;
    }
    
    // pH
    float phReading = pHSensor->readPH(PH_PIN);
    if (phReading >= 0 && phReading <= 14) {
        pH = phReading;
    }
    
    // TDS e EC
    tdsSensor->updateTemperature(temperature);
    tdsSensor->readTDS();
    float tdsReading = tdsSensor->getTDSValue();
    if (tdsReading >= 0 && tdsReading <= 2000) {
        tds = tdsReading;
        ec = tdsSensor->getECValue();
    }

    // Debug
    Serial.println("\n=== Status do Sistema ===");
    Serial.printf("Temperatura: %.1f°C\n", temperature);
    Serial.printf("pH: %.2f\n", pH);
    Serial.printf("TDS: %.0f ppm\n", tds);
    Serial.printf("EC: %.0f uS/cm\n", ec);
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
    if (relay >= 0 && relay < NUM_RELAYS) {
        relayStates[relay] = !relayStates[relay];
        bool state = !relayStates[relay];  // Invertido porque relés são ativos em LOW
        
        // Corrigir o mapeamento dos relés para os PCF8574s
        if (relay < 7) {  // Primeiro PCF8574 (P0-P6)
            pcf1.write(relay, state);
            Serial.printf("PCF1: Relé %d -> pino P%d = %d\n", relay+1, relay, state);
        } else {          // Segundo PCF8574 (P1-P7)
            int pcf2Pin = (relay - 6); // Ajuste para mapear para P1-P7
            pcf2.write(pcf2Pin, state);
            Serial.printf("PCF2: Relé %d -> pino P%d = %d\n", relay+1, pcf2Pin, state);
        }
        
        if (seconds > 0 && relayStates[relay]) {
            startTimes[relay] = millis();
            timerSeconds[relay] = seconds;
            Serial.printf("Relé %d ligado por %d segundos\n", relay+1, seconds);
        } else {
            startTimes[relay] = 0;
            timerSeconds[relay] = 0;
        }
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
                    pcf1.write(i, state);
                    Serial.printf("Timer PCF1: Relé %d -> pino P%d = %d\n", i+1, i, state);
                } else {      // Segundo PCF8574 (P1-P7)
                    int pcf2Pin = (i - 6); // Ajuste para mapear para P1-P7
                    pcf2.write(pcf2Pin, state);
                    Serial.printf("Timer PCF2: Relé %d -> pino P%d = %d\n", i+1, pcf2Pin, state);
                }
                
                timerSeconds[i] = 0;
                startTimes[i] = 0;
            }
        }
    }
}