#include "FlowmeterSensor.h"

FlowmeterSensor* FlowmeterSensor::activeInstance_ = nullptr;

FlowmeterSensor::FlowmeterSensor(uint8_t pulsePin, float pulsesPerLiter)
    : pulsePin_(pulsePin),
      pulsesPerLiter_(pulsesPerLiter > 0.0f ? pulsesPerLiter : 450.0f),
      pulseCount_(0),
      lastLevel_(HIGH) {}

void FlowmeterSensor::begin() {
    activeInstance_ = this;
    pinMode(pulsePin_, INPUT_PULLUP);
    lastLevel_ = digitalRead(pulsePin_);
    pulseCount_ = 0;
    attachInterrupt(digitalPinToInterrupt(pulsePin_), isrThunk, FALLING);
}

void IRAM_ATTR FlowmeterSensor::isrThunk() {
    if (activeInstance_ != nullptr) {
        activeInstance_->pulseCount_++;
    }
}

void FlowmeterSensor::onPulse() {
    ++pulseCount_;
}

void FlowmeterSensor::tick() {
    (void)lastLevel_;
}

void FlowmeterSensor::reset() {
    noInterrupts();
    pulseCount_ = 0;
    interrupts();
}

float FlowmeterSensor::consumedLiters() const {
    if (pulsesPerLiter_ <= 0.0f) {
        return 0.0f;
    }
    return static_cast<float>(pulseCount_) / pulsesPerLiter_;
}

void FlowmeterSensor::setPulsesPerLiter(float ppl) {
    if (ppl > 0.0f) {
        pulsesPerLiter_ = ppl;
    }
}
