#ifndef PHSENSOR_H  //se não está definido, define
#define PHSENSOR_H  //define

#include <Arduino.h> //biblioteca do Arduino

class phSensor {
public:
    phSensor();  // Construtor padrão
    void calibrate(float cal_ph7, float cal_ph4, float cal_ph10 = 0.0, bool use_ph10 = false);  // Método para configurar a calibração
    float readPH(uint8_t pin);  // Método para ler o valor de pH
    void printSerialPH(uint8_t pin); // Método para exibir o valor de pH no Serial Monitor

private:
    float calibracao_ph7; //calibração do pH 7
    float calibracao_ph4; //calibração do pH 4
    float calibracao_ph10; //calibração do pH 10
    bool UTILIZAR_PH_10; //utilizar o pH 10
    float m, b; //coeficientes da reta
    int buf[10]; //buffer para armazenar os valores

    float calculatePH(float voltage);  // Método para calcular o pH baseado no voltaje
    float getAverage(uint8_t pin);     // Método para obter o valor médio do sensor
};

#endif //   
