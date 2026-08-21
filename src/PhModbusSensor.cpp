#include "PhModbusSensor.h"
#include "Config.h"
#include <cstring>

PhModbusSensor* PhModbusSensor::activeInstance_ = nullptr;

namespace {

float modbusRegsToFloat(uint16_t regHi, uint16_t regLo, bool wordSwap) {
    const uint32_t u = wordSwap
                           ? (static_cast<uint32_t>(regLo) << 16) | regHi
                           : (static_cast<uint32_t>(regHi) << 16) | regLo;
    float out = 0.0f;
    memcpy(&out, &u, sizeof(out));
    return out;
}

bool isPlausiblePh(float v) {
    return v >= 0.0f && v <= 14.0f;
}

bool isPlausibleTempC(float v) {
    return v >= -10.0f && v <= 80.0f;
}

void printScaleHints(uint16_t raw) {
    const float d10 = static_cast<float>(raw) / 10.0f;
    const float d100 = static_cast<float>(raw) / 100.0f;
    const float d1000 = static_cast<float>(raw) / 1000.0f;
    Serial.printf("  /10=%.2f /100=%.3f /1000=%.4f", d10, d100, d1000);
    if (isPlausiblePh(d10)) {
        Serial.print("  [pH?/10]");
    }
    if (isPlausiblePh(d100)) {
        Serial.print("  [pH?/100]");
    }
    if (isPlausibleTempC(d10)) {
        Serial.print("  [tempC?/10]");
    }
    if (isPlausibleTempC(d100)) {
        Serial.print("  [tempC?/100]");
    }
}

}  // namespace

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

#if PH_MODBUS_DISCOVERY
    runDiscoveryScan();
#endif
}

void PhModbusSensor::runDiscoveryScan() {
    Serial.println();
    Serial.println("========== [pH DISCOVERY] inicio ==========");
    Serial.printf("[DISCOVERY] slave=%u baud=%lu holding 0x%04X..0x%04X + input + float pairs\n",
                  static_cast<unsigned>(slaveAddr_),
                  static_cast<unsigned long>(baud_),
                  static_cast<unsigned>(PH_MODBUS_DISCOVERY_REG_START),
                  static_cast<unsigned>(PH_MODBUS_DISCOVERY_REG_END));
    Serial.println();

    Serial.println("  -- Holding registers (1 reg / lectura) -----------------------");
    for (uint16_t reg = PH_MODBUS_DISCOVERY_REG_START; reg <= PH_MODBUS_DISCOVERY_REG_END; ++reg) {
        const uint8_t result = modbus_.readHoldingRegisters(reg, 1);
        if (result == modbus_.ku8MBSuccess) {
            const uint16_t raw = modbus_.getResponseBuffer(0);
            Serial.printf("  [SCAN-H] reg=0x%04X raw=%5u", reg, raw);
            printScaleHints(raw);
            Serial.println();
        } else {
            Serial.printf("  [SCAN-H] reg=0x%04X err=0x%02X\n", reg, result);
        }
        delay(PH_MODBUS_DISCOVERY_REG_DELAY_MS);
    }

    Serial.println();
    Serial.println("  -- Input registers (1 reg / lectura) ---------------------------");
    for (uint16_t reg = PH_MODBUS_DISCOVERY_REG_START; reg <= PH_MODBUS_DISCOVERY_REG_END; ++reg) {
        const uint8_t result = modbus_.readInputRegisters(reg, 1);
        if (result == modbus_.ku8MBSuccess) {
            const uint16_t raw = modbus_.getResponseBuffer(0);
            Serial.printf("  [SCAN-I] reg=0x%04X raw=%5u", reg, raw);
            printScaleHints(raw);
            Serial.println();
        } else {
            Serial.printf("  [SCAN-I] reg=0x%04X err=0x%02X\n", reg, result);
        }
        delay(PH_MODBUS_DISCOVERY_REG_DELAY_MS);
    }

    Serial.println();
    Serial.println("  -- Float IEEE (pares holding consecutivos) ---------------------");
    const uint16_t blockLen =
        static_cast<uint16_t>(PH_MODBUS_DISCOVERY_REG_END - PH_MODBUS_DISCOVERY_REG_START + 1u);
    const uint8_t blockResult =
        modbus_.readHoldingRegisters(PH_MODBUS_DISCOVERY_REG_START, blockLen);
    if (blockResult == modbus_.ku8MBSuccess) {
        for (uint16_t i = 0; i + 1u < blockLen; i += 2u) {
            const uint16_t regA =
                static_cast<uint16_t>(PH_MODBUS_DISCOVERY_REG_START + i);
            const uint16_t regB =
                static_cast<uint16_t>(PH_MODBUS_DISCOVERY_REG_START + i + 1u);
            const uint16_t w0 = modbus_.getResponseBuffer(i);
            const uint16_t w1 = modbus_.getResponseBuffer(i + 1u);
            const float fBe = modbusRegsToFloat(w0, w1, false);
            const float fSwap = modbusRegsToFloat(w0, w1, true);
            Serial.printf("  [SCAN-F] reg=0x%04X+0x%04X  w0=%u w1=%u  BE=%.4f  swap=%.4f",
                          regA,
                          regB,
                          w0,
                          w1,
                          static_cast<double>(fBe),
                          static_cast<double>(fSwap));
            if (isPlausiblePh(fBe)) {
                Serial.print("  [pH?BE]");
            }
            if (isPlausiblePh(fSwap)) {
                Serial.print("  [pH?swap]");
            }
            if (isPlausibleTempC(fBe)) {
                Serial.print("  [temp?BE]");
            }
            if (isPlausibleTempC(fSwap)) {
                Serial.print("  [temp?swap]");
            }
            Serial.println();
        }
    } else {
        Serial.printf("  [SCAN-F] bloque holding err=0x%02X\n", blockResult);
    }

    Serial.println();
    Serial.println("  Criterio: [pH?/10] o [pH?/100] en rango 0-14; temp -10..80 C.");
    Serial.println("  Si solo reg0x0001 tiene [pH?/10], escala actual x10 es correcta.");
    Serial.println("========== [pH DISCOVERY] fin ==========");
    Serial.println();
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
