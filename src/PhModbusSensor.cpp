#include "PhModbusSensor.h"
#include "Config.h"

PhModbusSensor* PhModbusSensor::activeInstance_ = nullptr;

PhModbusSensor::PhModbusSensor(uint8_t rxPin,
                               uint8_t txPin,
                               uint8_t deRePin,
                               uint32_t baud,
                               uint8_t slaveAddr,
                               uint16_t holdingReg,
                               float scale)
    : rxPin_(rxPin),
      txPin_(txPin),
      deRePin_(deRePin),
      baud_(baud),
      slaveAddr_(slaveAddr),
      holdingReg_(holdingReg),
      scale_(scale),
      lastValidPh_(NAN),
      lastTempC_(NAN),
      lastRawPh_(0),
      lastReg0Raw_(0),
      lastReg1Raw_(0),
      lastError_(0) {}

void PhModbusSensor::preTransmissionStatic() {
    if (activeInstance_ != nullptr) {
        digitalWrite(activeInstance_->deRePin_, HIGH);
    }
}

void PhModbusSensor::postTransmissionStatic() {
    if (activeInstance_ != nullptr) {
        digitalWrite(activeInstance_->deRePin_, LOW);
    }
}

void PhModbusSensor::begin() {
    activeInstance_ = this;

    pinMode(deRePin_, OUTPUT);
    digitalWrite(deRePin_, LOW);

    Serial2.begin(baud_, SERIAL_8N1, rxPin_, txPin_);
    modbus_.begin(slaveAddr_, Serial2);
    modbus_.preTransmission(preTransmissionStatic);
    modbus_.postTransmission(postTransmissionStatic);
}

float PhModbusSensor::readPH() {
    const uint16_t blockStart = PH_MODBUS_TEMP_REG;
    const uint16_t regCount =
        static_cast<uint16_t>((holdingReg_ - PH_MODBUS_TEMP_REG) + 1u);

    const uint8_t result = modbus_.readHoldingRegisters(blockStart, regCount);
    lastError_ = result;

    if (result != modbus_.ku8MBSuccess) {
        Serial.printf("[pH Modbus] err=0x%02X addr=%u\n",
                      result,
                      static_cast<unsigned>(slaveAddr_));
        return NAN;
    }

    lastReg0Raw_ = modbus_.getResponseBuffer(0);
    lastReg1Raw_ = (regCount > 1u) ? modbus_.getResponseBuffer(1) : 0u;
    lastTempC_ = static_cast<float>(lastReg0Raw_) / PH_MODBUS_TEMP_SCALE;

    const uint16_t phIndex = static_cast<uint16_t>(holdingReg_ - PH_MODBUS_TEMP_REG);
    lastRawPh_ = modbus_.getResponseBuffer(phIndex);
    const float ph = static_cast<float>(lastRawPh_) / scale_;

    Serial.printf("[pH Modbus] reg0=%u reg1=%u temp=%.1f ph=%.2f err=0x00\n",
                  lastReg0Raw_,
                  lastReg1Raw_,
                  lastTempC_,
                  ph);

    if (ph < MIN_PH || ph > MAX_PH) {
        Serial.printf("[pH Modbus] fuera de rango: ph=%.2f\n", ph);
        return NAN;
    }

    lastValidPh_ = ph;
    return ph;
}
