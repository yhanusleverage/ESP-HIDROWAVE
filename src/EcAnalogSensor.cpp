#include "Config.h"
#include "EcAnalogSensor.h"

EcAnalogSensor::EcAnalogSensor(uint8_t analogPin, float fullScaleVolts, float calibrationFactor)
    : analogPin_(analogPin)
    , fullScaleVolts_(fullScaleVolts)
    , calibrationFactor_(calibrationFactor)
    , liquidTempC_(25.0f)
    , windowSumMv_(0)
    , windowCount_(0)
    , nextSampleDueMs_(0)
    , adcMeanRaw_(0)
    , lastPinVoltsLin_(0)
    , lastPinVoltsCal_(0)
    , ecMean_uScm_(0)
    , pendingWindow_(false)
{
}

void EcAnalogSensor::begin() {
    pinMode(analogPin_, INPUT);
    ESP32_ADC_CONFIGURE_PIN(analogPin_);
    windowSumMv_ = 0;
    windowCount_ = 0;
    nextSampleDueMs_ = millis();
    pendingWindow_ = false;
}

void EcAnalogSensor::closeBatch_() {
    if (windowCount_ <= 0) {
        return;
    }

    const uint32_t meanMv = static_cast<uint32_t>(windowSumMv_ / static_cast<uint64_t>(windowCount_));
    lastPinVoltsCal_ = static_cast<float>(meanMv) / 1000.0f;

    const uint32_t rawU = (meanMv * 4095u + 1650u) / 3300u;
    if (rawU > 4095u) {
        adcMeanRaw_ = 4095;
    } else {
        adcMeanRaw_ = static_cast<int>(rawU);
    }
    lastPinVoltsLin_ = adcMeanRaw_ * (ESP32_ADC_MAX_VOLTS / 4095.0f);

    const float vPinEc = lastPinVoltsCal_;
    float rawEc = (EC_SENSOR_RANGE_US_CM / fullScaleVolts_) * vPinEc * calibrationFactor_;
    if (rawEc < 0.0f) {
        rawEc = 0.0f;
    }
    ecMean_uScm_ = rawEc;

    pendingWindow_ = true;
}

void EcAnalogSensor::tick() {
    const unsigned long now = millis();
    if (now < nextSampleDueMs_) {
        return;
    }

    nextSampleDueMs_ = now + EC_SAMPLE_INTERVAL_MS;

    const uint32_t mv = analogReadMilliVolts(analogPin_);
    windowSumMv_ += static_cast<uint64_t>(mv);
    ++windowCount_;

    if (windowCount_ >= EC_SAMPLES_PER_WINDOW) {
        closeBatch_();
        windowSumMv_ = 0;
        windowCount_ = 0;
    }
}

bool EcAnalogSensor::consumeWindowReady() {
    if (!pendingWindow_) {
        return false;
    }
    pendingWindow_ = false;
    return true;
}

void EcAnalogSensor::updateLiquidTemperatureC(float tempC) {
    liquidTempC_ = tempC;
    (void)liquidTempC_;
}

bool EcAnalogSensor::isBufferReady() const {
    return true;
}

bool EcAnalogSensor::calibrate(float standardEc_uScm, float measuredEc_uScm) {
    if (measuredEc_uScm <= 0.0f || standardEc_uScm <= 0.0f) {
        Serial.println("[EC] calibracion: valores invalidos.");
        return false;
    }

    const float nextK = standardEc_uScm / measuredEc_uScm;
    Serial.printf("[EC] calibracion patron=%.1f medido=%.1f K %.4f -> %.4f\n",
                  standardEc_uScm,
                  measuredEc_uScm,
                  calibrationFactor_,
                  nextK);
    calibrationFactor_ = nextK;
    return true;
}

bool EcAnalogSensor::calibrateWithSolution1413() {
    const int rounds = 10;
    float sumEc = 0.0f;
    int ok = 0;

    Serial.println("[EC] calibracion 1413 uS/cm (bloques de muestras)...");

    for (int i = 0; i < rounds; i++) {
        while (!consumeWindowReady()) {
            tick();
            delay(1);
        }
        const float ec = ecMeanMicrosiemensPerCm();
        if (ec > 100.0f && ec < 8000.0f) {
            sumEc += ec;
            ok++;
        }
        delay(200);
    }

    if (ok < 5) {
        Serial.println("[EC] calibracion: pocas muestras validas.");
        return false;
    }

    const float meanMeasured = sumEc / ok;
    return calibrate(1413.0f, meanMeasured);
}

void EcAnalogSensor::setCalibrationFactor(float factor) {
    if (factor > 0.0f && factor <= 10.0f) {
        Serial.printf("[EC] K %.4f -> %.4f\n", calibrationFactor_, factor);
        calibrationFactor_ = factor;
    } else {
        Serial.printf("[EC] K invalido %.4f\n", factor);
    }
}

void EcAnalogSensor::setFullScaleVolts(float volts) {
    if (volts > 0.0f && volts <= 10.0f) {
        Serial.printf("[EC] V escala %.2f -> %.2f\n", fullScaleVolts_, volts);
        fullScaleVolts_ = volts;
    } else {
        Serial.printf("[EC] V escala invalido %.2f\n", volts);
    }
}

float EcAnalogSensor::calibrationFactor() const {
    return calibrationFactor_;
}

float EcAnalogSensor::fullScaleVolts() const {
    return fullScaleVolts_;
}
