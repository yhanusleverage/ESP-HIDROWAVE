#ifndef FLOWMETER_SENSOR_H
#define FLOWMETER_SENSOR_H

#include <Arduino.h>

/** Contador de pulsos YF-S201 / similar na saída do dreno. */
class FlowmeterSensor {
public:
    explicit FlowmeterSensor(uint8_t pulsePin, float pulsesPerLiter);

    void begin();
    void tick();
    void reset();
    float consumedLiters() const;
    uint32_t pulseCount() const { return pulseCount_; }
    void setPulsesPerLiter(float ppl);
    float pulsesPerLiter() const { return pulsesPerLiter_; }

private:
    uint8_t pulsePin_;
    float pulsesPerLiter_;
    volatile uint32_t pulseCount_;
    bool lastLevel_;
    static void IRAM_ATTR isrThunk();
    static FlowmeterSensor* activeInstance_;
    void onPulse();
};

#endif
