#ifndef PHSENSOR_H
#define PHSENSOR_H

#include <Arduino.h>

class phSensor {
public:
    phSensor();
    void calibrate(float cal_ph7, float cal_ph4, float cal_ph10 = 0.0, bool use_ph10 = false);
    float readPH(uint8_t pin);
    void printSerialPH(uint8_t pin);

private:
    float calibracao_ph7;
    float calibracao_ph4;
    float calibracao_ph10;
    bool UTILIZAR_PH_10;
    float m, b;
    int buf[10];

    float calculatePH(float voltage);
    float getAverage(uint8_t pin);
};

#endif 
