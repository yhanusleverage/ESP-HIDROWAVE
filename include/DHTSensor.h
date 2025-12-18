#ifndef DHT_SENSOR_H
#define DHT_SENSOR_H

#include <DHT.h>
#include "Config.h"

class DHTSensor {
public:
    DHTSensor(uint8_t pin, uint8_t type = DHT22);
    void begin();
    float readTemperature();
    float readHumidity();
    
private:
    DHT dht;
    uint8_t pin;
    uint8_t type;
};

#endif // DHT_SENSOR_H 