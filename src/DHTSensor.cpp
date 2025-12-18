#include "DHTSensor.h"

DHTSensor::DHTSensor(uint8_t pin, uint8_t type) : dht(pin, type), pin(pin), type(type) {
}

void DHTSensor::begin() {
    dht.begin();
}

float DHTSensor::readTemperature() {
    float temp = dht.readTemperature();
    if (isnan(temp)) {
        return -999.0;  // Valor de erro
    }
    return temp;
}

float DHTSensor::readHumidity() {
    float humidity = dht.readHumidity();
    if (isnan(humidity)) {
        return -999.0;  // Valor de erro
    }
    return humidity;
} 