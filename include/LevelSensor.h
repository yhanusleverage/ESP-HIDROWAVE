#ifndef LEVELSENSOR_H
#define LEVELSENSOR_H

#include <Arduino.h>

class LevelSensor {
public:
    LevelSensor(uint8_t pinNPN, uint8_t pinPNP);
    void begin();
    bool isLow();    // Retorna true se o nível está baixo
    bool isFull();   // Retorna true se o nível está cheio
    String getStatus(); // Retorna "BAIXO", "MÉDIO", "CHEIO" ou "ERRO"
    bool checkWaterLevel(); // Retorna true se o nível está OK (não baixo e sem erro)

private:
    uint8_t _pinNPN;  // Sensor NPN
    uint8_t _pinPNP;  // Sensor PNP
    String _lastStatus;  // Último status lido
    unsigned long _lastCheck;  // Timestamp da última leitura
    
    // Funções auxiliares para normalizar as leituras
    bool sensorNPNDetectando();
    bool sensorPNPDetectando();
};

#endif 