#ifndef PH_MODBUS_SENSOR_H
#define PH_MODBUS_SENSOR_H

#include <Arduino.h>
#include <ModbusMaster.h>

class PhModbusSensor {
public:
    PhModbusSensor(uint8_t rxPin,
                   uint8_t txPin,
                   uint8_t deRePin,
                   uint32_t baud,
                   uint8_t slaveAddr,
                   uint16_t holdingReg,
                   float scale);

    void begin();
    float readPH();
    float lastValidPH() const { return lastValidPh_; }
    uint16_t lastRawPh() const { return lastRawPh_; }
    uint16_t lastReg0Raw() const { return lastReg0Raw_; }
    uint16_t lastReg1Raw() const { return lastReg1Raw_; }
    float lastTempC() const { return lastTempC_; }
    uint8_t lastError() const { return lastError_; }

private:
    static void preTransmissionStatic();
    static void postTransmissionStatic();

    uint8_t rxPin_;
    uint8_t txPin_;
    uint8_t deRePin_;
    uint32_t baud_;
    uint8_t slaveAddr_;
    uint16_t holdingReg_;
    float scale_;

    ModbusMaster modbus_;
    float lastValidPh_;
    float lastTempC_;
    uint16_t lastRawPh_;
    uint16_t lastReg0Raw_;
    uint16_t lastReg1Raw_;
    uint8_t lastError_;

    static PhModbusSensor* activeInstance_;
};

#endif
