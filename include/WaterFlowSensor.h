#ifndef WATER_FLOW_SENSOR_H
#define WATER_FLOW_SENSOR_H

#include <Arduino.h>

/**
 * YF-B5: F = FLOW_HZ_PER_LPM * Q; litros = pulsos / FLOW_PULSES_PER_LITER * K.
 * Paridad carpeta flowmeter/SUPREME: FALLING + debounce + filtro banda + diag raw/deb/dt.
 * Sin task FreeRTOS — tick() desde HydroControl::update().
 */
class WaterFlowSensor {
public:
    WaterFlowSensor(uint8_t pulsePin, float calibrationFactor = 1.0f);

    void begin();
    void enable();
    void disable();
    bool isEnabled() const { return isrArmed_; }
    void tick();
    bool consumeWindowReady();
    void reset();
    void resetSession() { reset(); }

    float flowRateLitersPerMin() const { return qLpm_; }
    float qLpm() const { return qLpm_; }
    float freqHz() const { return freqHz_; }
    float totalLiters() const { return totalLiters_; }
    float sessionLiters() const { return totalLiters_; }
    uint32_t lastWindowPulses() const { return lastPulses_; }
    uint32_t pulses() const { return lastPulses_; }
    uint32_t totalPulses() const { return totalPulses_; }
    uint8_t pulsePin() const { return pulsePin_; }
    bool valid() const { return valid_; }
    int pinLevel() const { return digitalRead(pulsePin_); }
    const char* rejectReason() const { return rejectReason_; }

    /** Diagnóstico última ventana (SUPREME / FLOW_DEBUG). */
    uint32_t edgesRaw() const { return lastEdgesRaw_; }
    uint32_t debounceRejects() const { return lastDebounceRejects_; }
    uint32_t minIntervalUs() const { return lastMinIntervalUs_; }
    uint32_t maxIntervalUs() const { return lastMaxIntervalUs_; }

    void setCalibrationFactor(float factor);
    float calibrationFactor() const { return calibrationFactor_; }

private:
    void closeWindow_();
    static void IRAM_ATTR onPulseIsr(void* arg);

    uint8_t pulsePin_;
    float calibrationFactor_;

    volatile uint32_t isrCount_;
    volatile uint32_t isrEdgesRaw_;
    volatile uint32_t isrDebounceRejects_;
    volatile uint32_t isrMinIntervalUs_;
    volatile uint32_t isrMaxIntervalUs_;
    volatile unsigned long lastPulseUs_;

    uint32_t lastPulses_;
    uint32_t lastEdgesRaw_;
    uint32_t lastDebounceRejects_;
    uint32_t lastMinIntervalUs_;
    uint32_t lastMaxIntervalUs_;
    uint32_t totalPulses_;
    float freqHz_;
    float qLpm_;
    float totalLiters_;
    bool valid_;
    bool pending_;
    bool isrArmed_;
    unsigned long windowStartMs_;
    const char* rejectReason_;
};

#endif
