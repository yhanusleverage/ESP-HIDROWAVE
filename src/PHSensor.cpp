#include "PHSensor.h"

// Construtor - inicializa todas as variáveis com valores padrão
phSensor::phSensor() {
    // Valores típicos de calibração
    calibracao_ph7 = 2.56;   // ~2.5V para pH 7
    calibracao_ph4 = 3.3;    // ~3.3V para pH 4
    calibracao_ph10 = 2.05;  // ~2.0V para pH 10
    UTILIZAR_PH_10 = false;  // Por padrão, usa calibração de 2 pontos

    // Inicializar coeficientes da equação
    m = (4.0 - 7.0) / (calibracao_ph4 - calibracao_ph7); // Slope inicial
    b = 7.0 - m * calibracao_ph7;                        // Offset inicial

    // Limpar buffer de leituras
    for (int i = 0; i < 10; i++) {
        buf[i] = 0;
    }
}

// Calibração do sensor com 2 ou 3 pontos
void phSensor::calibrate(float cal_ph7, float cal_ph4, float cal_ph10, bool use_ph10) {
    calibracao_ph7 = cal_ph7;
    calibracao_ph4 = cal_ph4;
    calibracao_ph10 = cal_ph10;
    UTILIZAR_PH_10 = use_ph10;

    if (UTILIZAR_PH_10) {
        m = (7.0 - 10.0) / (calibracao_ph7 - calibracao_ph10);
        b = 10.0 - m * calibracao_ph10;
    } else {
        m = (4.0 - 7.0) / (calibracao_ph4 - calibracao_ph7);
        b = 7.0 - m * calibracao_ph7;
    }

    Serial.println("Calibração concluída!");
}

// Calcula a média das leituras, descartando valores extremos
float phSensor::getAverage(uint8_t pin) {
    for (int i = 0; i < 10; i++) {
        buf[i] = analogRead(pin);
        delay(10);
    }

    // Ordenar as leituras
    for (int i = 0; i < 9; i++) {
        for (int j = i + 1; j < 10; j++) {
            if (buf[i] > buf[j]) {
                int temp = buf[i];
                buf[i] = buf[j];
                buf[j] = temp;
            }
        }
    }

    // Calcular o valor médio das leituras centrais
    int valorMedio = 0;
    for (int i = 2; i < 8; i++) {
        valorMedio += buf[i];
    }

    // Converter o valor médio para voltagem
    return (valorMedio * 3.3) / 4095.0 / 6;
}

// Converte voltagem para valor de pH usando equação da reta
float phSensor::calculatePH(float voltage) {
    return m * voltage + b;  // Calcular o pH
}

// Método público para leitura do pH
float phSensor::readPH(uint8_t pin) {
    float voltage = getAverage(pin);
    return calculatePH(voltage);
}

// Método para debug - imprime leitura na Serial
void phSensor::printSerialPH(uint8_t pin) {
    float ph = readPH(pin);
    Serial.print("pH = ");
    Serial.println(ph, 2);  // Exibir o pH com 2 casas decimais
}
