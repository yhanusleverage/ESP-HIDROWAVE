#include "LevelSensor.h"

LevelSensor::LevelSensor(uint8_t pinNPN, uint8_t pinPNP)
    : _pinNPN(pinNPN), _pinPNP(pinPNP), _lastStatus(""), _lastCheck(0) {}

void LevelSensor::begin() {
    pinMode(_pinNPN, INPUT);
    pinMode(_pinPNP, INPUT);
    Serial.println("✅ Sensor de nível iniciado (XKR-25)");
    Serial.printf("   NPN: pino %d, PNP: pino %d\n", _pinNPN, _pinPNP);
}

bool LevelSensor::sensorNPNDetectando() {
    return digitalRead(_pinNPN) == HIGH;  // NPN detecta quando é HIGH
}

bool LevelSensor::sensorPNPDetectando() {
    return digitalRead(_pinPNP) == LOW;   // PNP detecta quando é LOW
}

String LevelSensor::getStatus() {
    unsigned long now = millis();
    if (now - _lastCheck < 1000) {  // Atualiza no máximo a cada 1 segundo
        return _lastStatus;
    }
    
    _lastCheck = now;
    bool npnDetecta = sensorNPNDetectando();
    bool pnpDetecta = sensorPNPDetectando();

    String oldStatus = _lastStatus;
    
    if (!npnDetecta) {
        _lastStatus = "CHEIO";
    } else if (!pnpDetecta && npnDetecta) {
        _lastStatus = "MÉDIO";
    } else if (pnpDetecta && npnDetecta) {
        _lastStatus = "BAIXO";
    } else {
        _lastStatus = "ERRO";  // Estado inconsistente
    }
    
    // Reportar mudanças de estado
    if (oldStatus != _lastStatus) {
        Serial.printf("💧 Nível da água: %s -> %s\n", 
            oldStatus.isEmpty() ? "?" : oldStatus.c_str(), 
            _lastStatus.c_str());
    }
    
    return _lastStatus;
}

bool LevelSensor::isLow() {
    return getStatus() == "BAIXO";
}

bool LevelSensor::isFull() {
    return getStatus() == "CHEIO";
}

bool LevelSensor::checkWaterLevel() {
    String status = getStatus();
    return status != "BAIXO" && status != "ERRO";
}
