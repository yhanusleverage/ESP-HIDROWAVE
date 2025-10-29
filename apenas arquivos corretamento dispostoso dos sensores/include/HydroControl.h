#ifndef HYDROCONTROL_H
#define HYDROCONTROL_H

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "TDSReaderSerial.h"
#include "phSensor.h"
#include "PCF8574.h"
#include "ThingSpeakManager.h"

class HydroControl {
public:
    HydroControl(); //construtor
    void begin(); //inicializar o sistema   
    void update();  // Atualiza tudo
    void showMessage(String msg); //mostrar mensagens
    
    // Controle dos relés
    void toggleRelay(int relay, int seconds = 0); //ligar e desligar os relés
    bool* getRelayStates() { return relayStates; } //obter os estados dos relés
    
    // Getters dos sensores
    float& getTemperature() { return temperature; }
    float& getpH() { return pH; }
    float& getTDS() { return tds; }
    float& getEC() { return ec; }

private:
    static const int NUM_RELAYS = 8;  // Total de relés
    PCF8574 pcf1;  // Primeiro PCF8574 (0x20)
    PCF8574 pcf2;  // Segundo PCF8574 (0x24)
    
    // Pinos e configurações
    static const int PH_PIN = 35; //pino do pH
    static const int TEMP_PIN = 33; //pino da temperatura
    static const int TDS_PIN = 34; //pino do TDS
    
    // Objetos dos sensores
    LiquidCrystal_I2C lcd; //display
    OneWire oneWire; //sensor de temperatura
    DallasTemperature sensors; //sensor de temperatura
    TDSReaderSerial* tdsSensor; //sensor de TDS
    phSensor* pHSensor; //sensor de pH
    
    // Estados dos relés
    bool relayStates[8];
    unsigned long startTimes[8]; //tempo de início
    int timerSeconds[8]; //tempo de duração
    
    // Valores dos sensores
    float temperature; //temperatura
    float pH; //pH
    float tds; //TDS
    float ec; //EC
    
    // Funções internas
    void updateSensors(); //atualizar os sensores
    void updateDisplay(); //atualizar o display
    void checkRelayTimers(); //verificar os relés
    ThingSpeakManager thingSpeak;
};

#endif