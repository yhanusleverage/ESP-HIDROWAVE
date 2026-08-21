#include "WaterFlowSensor.h"
#include "Config.h"
#include <esp_timer.h>
#include <limits.h>

WaterFlowSensor::WaterFlowSensor(uint8_t pulsePin, float calibrationFactor)
    : pulsePin_(pulsePin)
    , calibrationFactor_(calibrationFactor > 0.0f ? calibrationFactor : 1.0f)
    , isrCount_(0)
    , isrEdgesRaw_(0)
    , isrDebounceRejects_(0)
    , isrMinIntervalUs_(UINT32_MAX)
    , isrMaxIntervalUs_(0)
    , lastPulseUs_(0)
    , lastPulses_(0)
    , lastEdgesRaw_(0)
    , lastDebounceRejects_(0)
    , lastMinIntervalUs_(0)
    , lastMaxIntervalUs_(0)
    , totalPulses_(0)
    , freqHz_(0.0f)
    , qLpm_(0.0f)
    , totalLiters_(0.0f)
    , valid_(true)
    , pending_(false)
    , isrArmed_(false)
    , windowStartMs_(0)
    , rejectReason_("boot")
{
}

void WaterFlowSensor::begin() {
    // Paridad flowmeter/: GPIO4 = INPUT_PULLUP (idle HIGH / lvl=1).
#if defined(ESP32)
    if (pulsePin_ >= 34 && pulsePin_ <= 39) {
        pinMode(pulsePin_, INPUT);
    } else {
        pinMode(pulsePin_, INPUT_PULLUP);
    }
#else
    pinMode(pulsePin_, INPUT_PULLUP);
#endif

    noInterrupts();
    isrCount_ = 0;
    isrEdgesRaw_ = 0;
    isrDebounceRejects_ = 0;
    isrMinIntervalUs_ = UINT32_MAX;
    isrMaxIntervalUs_ = 0;
    lastPulseUs_ = 0;
    interrupts();

    lastPulses_ = 0;
    lastEdgesRaw_ = 0;
    lastDebounceRejects_ = 0;
    lastMinIntervalUs_ = 0;
    lastMaxIntervalUs_ = 0;
    totalPulses_ = 0;
    freqHz_ = 0.0f;
    qLpm_ = 0.0f;
    totalLiters_ = 0.0f;
    valid_ = true;
    pending_ = false;
    rejectReason_ = "boot";
    windowStartMs_ = millis();
    isrArmed_ = false;

#if FLOW_ISR_ONLY_WHEN_DILUTING
#if FLOW_SERIAL_DEBUG
    Serial.printf("[FLOW] GPIO%d pin listo — ISR off hasta dilución\n", pulsePin_);
#endif
#else
    enable();
#endif
}

void WaterFlowSensor::enable() {
    if (isrArmed_) {
        reset();
        return;
    }
    reset();
    attachInterruptArg(digitalPinToInterrupt(pulsePin_), onPulseIsr, this, FALLING);
    isrArmed_ = true;
#if FLOW_SERIAL_DEBUG
    Serial.printf("[FLOW] ISR ON GPIO%d\n", pulsePin_);
#endif
}

void WaterFlowSensor::disable() {
    if (!isrArmed_) {
        return;
    }
    detachInterrupt(digitalPinToInterrupt(pulsePin_));
    isrArmed_ = false;
    noInterrupts();
    isrCount_ = 0;
    isrEdgesRaw_ = 0;
    lastPulseUs_ = 0;
    interrupts();
    freqHz_ = 0.0f;
    qLpm_ = 0.0f;
    pending_ = false;
    rejectReason_ = "off";
#if FLOW_SERIAL_DEBUG
    Serial.printf("[FLOW] ISR OFF GPIO%d\n", pulsePin_);
#endif
}

void IRAM_ATTR WaterFlowSensor::onPulseIsr(void* arg) {
    auto* self = static_cast<WaterFlowSensor*>(arg);
    if (self == nullptr) {
        return;
    }
    self->isrEdgesRaw_++;

    const int64_t nowUs = esp_timer_get_time();
    const int64_t last = static_cast<int64_t>(self->lastPulseUs_);
    if (last != 0) {
        const uint32_t dt = static_cast<uint32_t>(nowUs - last);
        if (dt < static_cast<uint32_t>(FLOW_ISR_DEBOUNCE_US)) {
            self->isrDebounceRejects_++;
            return;
        }
        if (dt < self->isrMinIntervalUs_) {
            self->isrMinIntervalUs_ = dt;
        }
        if (dt > self->isrMaxIntervalUs_) {
            self->isrMaxIntervalUs_ = dt;
        }
    }
    self->lastPulseUs_ = static_cast<unsigned long>(nowUs);
    self->isrCount_++;
}

void WaterFlowSensor::closeWindow_() {
    noInterrupts();
    const uint32_t pulses = isrCount_;
    const uint32_t edgesRaw = isrEdgesRaw_;
    const uint32_t debRej = isrDebounceRejects_;
    const uint32_t minUs = isrMinIntervalUs_;
    const uint32_t maxUs = isrMaxIntervalUs_;
    isrCount_ = 0;
    isrEdgesRaw_ = 0;
    isrDebounceRejects_ = 0;
    isrMinIntervalUs_ = UINT32_MAX;
    isrMaxIntervalUs_ = 0;
    interrupts();

    const unsigned long elapsedMs = millis() - windowStartMs_;
    windowStartMs_ = millis();
    lastPulses_ = pulses;
    lastEdgesRaw_ = edgesRaw;
    lastDebounceRejects_ = debRej;
    lastMinIntervalUs_ = (minUs == UINT32_MAX) ? 0 : minUs;
    lastMaxIntervalUs_ = maxUs;

    if (elapsedMs == 0) {
        freqHz_ = 0.0f;
        qLpm_ = 0.0f;
        valid_ = true;
        rejectReason_ = "dt0";
        pending_ = true;
        return;
    }

    const float dt = static_cast<float>(elapsedMs) / 1000.0f;
    const float f = static_cast<float>(pulses) / dt;
    freqHz_ = f;
    const float qRaw = f / FLOW_HZ_PER_LPM;
    qLpm_ = qRaw * calibrationFactor_;
    valid_ = true;

#if FLOW_FILTER_ENABLE
    if (f < FLOW_IDLE_HZ) {
        qLpm_ = 0.0f;
        rejectReason_ = "idle";
        pending_ = true;
        return;
    }
    if (f > FLOW_MAX_HZ) {
        qLpm_ = 0.0f;
        valid_ = false;
        rejectReason_ = "noise";
        pending_ = true;
        return;
    }
    // Rebote/EMI: pares ~100 µs (SUPREME: real >= ~5 ms). No sumar litros.
    if (lastMinIntervalUs_ > 0 &&
        lastMinIntervalUs_ < static_cast<uint32_t>(FLOW_MIN_PULSE_GAP_US)) {
        qLpm_ = 0.0f;
        valid_ = false;
        rejectReason_ = "bounce";
        pending_ = true;
        return;
    }
    if (f < FLOW_MIN_HZ) {
        qLpm_ = 0.0f;
        rejectReason_ = "lo";
        pending_ = true;
        return;
    }
    if (qRaw > FLOW_Q_MAX_LPM) {
        qLpm_ = 0.0f;
        valid_ = false;
        rejectReason_ = "qmax";
        pending_ = true;
        return;
    }
#endif

    rejectReason_ = FLOW_FILTER_ENABLE ? "ok" : "raw";
    totalPulses_ += pulses;
    totalLiters_ += static_cast<float>(pulses) / FLOW_PULSES_PER_LITER * calibrationFactor_;
    pending_ = true;
}

void WaterFlowSensor::tick() {
    if (!isrArmed_) {
        return;
    }
    const unsigned long nowMs = millis();
    if (nowMs - windowStartMs_ >= FLOW_WINDOW_MS) {
        closeWindow_();
    }
}

bool WaterFlowSensor::consumeWindowReady() {
    if (!pending_) {
        return false;
    }
    pending_ = false;
    return true;
}

void WaterFlowSensor::reset() {
    noInterrupts();
    isrCount_ = 0;
    isrEdgesRaw_ = 0;
    isrDebounceRejects_ = 0;
    isrMinIntervalUs_ = UINT32_MAX;
    isrMaxIntervalUs_ = 0;
    lastPulseUs_ = 0;
    interrupts();

    lastPulses_ = 0;
    lastEdgesRaw_ = 0;
    lastDebounceRejects_ = 0;
    lastMinIntervalUs_ = 0;
    lastMaxIntervalUs_ = 0;
    totalPulses_ = 0;
    freqHz_ = 0.0f;
    qLpm_ = 0.0f;
    totalLiters_ = 0.0f;
    valid_ = true;
    pending_ = false;
    rejectReason_ = "reset";
    windowStartMs_ = millis();
}

void WaterFlowSensor::setCalibrationFactor(float factor) {
    if (factor > 0.0f) {
        calibrationFactor_ = factor;
    }
}
