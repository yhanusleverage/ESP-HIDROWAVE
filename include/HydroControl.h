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

class HydroControl {
public:
    static const int NUM_RELAYS = 16;
    
    HydroControl();
    bool begin();
    void loop();
    void update();
    void showMessage(String msg);
    void toggleRelay(int relay, int seconds = 0);
    void updateSensorData(float temp, float humidity, float ph, float tds);
    void updateRelayTimers();
    bool* getRelayStates() { return relayStates; }
    bool areSensorsWorking() { return sensorsOk; }
    bool isWaterLevelOk() { return tankLevelOk; }
    
    // Getters para leituras dos sensores
    float& getTemperature() { return temperature; }
    float& getpH() { return pH; }
    float& getTDS() { return tds; }
    float& getEC() { return ec; }
    String getTankStatus();
    float getWaterTemp();

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
    
    // Estado dos relés
    bool relayStates[NUM_RELAYS];
    unsigned long startTimes[NUM_RELAYS];
    int timerSeconds[NUM_RELAYS];
    
    // Funções internas
    void updateSensors();
    void updateDisplay();
    void checkRelayTimers();
};

#endif