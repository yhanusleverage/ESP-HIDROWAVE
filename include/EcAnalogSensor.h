#ifndef EC_ANALOG_SENSOR_H
#define EC_ANALOG_SENSOR_H

#include <Arduino.h>

/**
 * EC lineal: media de EC_SAMPLES_PER_WINDOW lecturas analogReadMilliVolts cada
 * EC_SAMPLE_INTERVAL_MS (una conversion calibrada por muestra; menos dispersion
 * que analogRead + mV seguidos).
 */
class EcAnalogSensor {
public:
    EcAnalogSensor(uint8_t analogPin, float fullScaleVolts, float calibrationFactor);

    void begin();
    void tick();
    bool consumeWindowReady();

    bool updateEcIfDue() {
        tick();
        return consumeWindowReady();
    }
    void readEc() {
        tick();
        (void)consumeWindowReady();
    }
    void readTds() { readEc(); }

    void updateLiquidTemperatureC(float tempC);

    float ecMeanMicrosiemensPerCm() const { return ecMean_uScm_; }
    float ecWindowMicrosiemensPerCm() const { return ecMean_uScm_; }
    float ecMicrosiemensPerCm() const { return ecMean_uScm_; }

    /** Raw 12b equivalente a la media de mV (solo referencia; EC desde Vcal). */
    int lastAdcRaw() const { return adcMeanRaw_; }
    float lastPinVolts() const { return lastPinVoltsCal_; }
    float lastPinVoltsFromRaw() const { return lastPinVoltsLin_; }
    unsigned long lastWindowSampleCount() const { return static_cast<unsigned long>(EC_SAMPLES_PER_WINDOW); }

    bool isBufferReady() const;

    bool calibrate(float standardEc_uScm, float measuredEc_uScm);
    bool calibrateWithSolution1413();
    void setCalibrationFactor(float factor);
    void setFullScaleVolts(float volts);
    float calibrationFactor() const;
    float fullScaleVolts() const;

private:
    void closeBatch_();

    uint8_t analogPin_;
    float fullScaleVolts_;
    float calibrationFactor_;
    float liquidTempC_;

    uint64_t windowSumMv_;
    int windowCount_;
    unsigned long nextSampleDueMs_;

    int adcMeanRaw_;
    float lastPinVoltsLin_;
    float lastPinVoltsCal_;
    float ecMean_uScm_;

    bool pendingWindow_;
};

#endif
