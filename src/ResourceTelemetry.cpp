#include "ResourceTelemetry.h"
#include "Config.h"

#if RESOURCE_SERIAL_DEBUG

namespace ResourceTelemetry {

static TaskHandle_t espNowHandle_ = nullptr;
static TaskHandle_t loopHandle_ = nullptr;
static uint32_t loopCount_ = 0;
static unsigned long windowStartMs_ = 0;
static bool mqttConnected_ = false;
static bool sslBusy_ = false;
static bool wifiConnected_ = false;
static int slavesOnline_ = 0;
static const char* dilPhase_ = "n/a";

void setEspNowTaskHandle(TaskHandle_t handle) {
    espNowHandle_ = handle;
}

void setContext(bool mqttConnected, bool sslBusy, const char* dilPhase,
                bool wifiConnected, int slavesOnline) {
    mqttConnected_ = mqttConnected;
    sslBusy_ = sslBusy;
    wifiConnected_ = wifiConnected;
    slavesOnline_ = slavesOnline < 0 ? 0 : slavesOnline;
    if (dilPhase != nullptr) {
        dilPhase_ = dilPhase;
    }
}

void tick() {
    if (loopHandle_ == nullptr) {
        loopHandle_ = xTaskGetCurrentTaskHandle();
        windowStartMs_ = millis();
        loopCount_ = 0;
    }

    loopCount_++;
    const unsigned long now = millis();
    if (now - windowStartMs_ < RESOURCE_LOG_MS) {
        return;
    }

    const float elapsedSec = static_cast<float>(now - windowStartMs_) / 1000.0f;
    const float loopsPerSec = (elapsedSec > 0.001f)
        ? (static_cast<float>(loopCount_) / elapsedSec)
        : 0.0f;

    UBaseType_t loopHwm = 0;
    if (loopHandle_ != nullptr) {
        loopHwm = uxTaskGetStackHighWaterMark(loopHandle_);
    }
    UBaseType_t espNowHwm = 0;
    if (espNowHandle_ != nullptr) {
        espNowHwm = uxTaskGetStackHighWaterMark(espNowHandle_);
    }

    Serial.printf(
        "[RES] up=%lus heap=%u min=%u maxAlloc=%u loop/s≈%.0f loop_hwm=%u espnow_hwm=%u "
        "wifi=%d mqtt=%d slaves=%d sslBusy=%d dil=%s\n",
        static_cast<unsigned long>(now / 1000UL),
        static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(ESP.getMinFreeHeap()),
        static_cast<unsigned>(ESP.getMaxAllocHeap()),
        loopsPerSec,
        static_cast<unsigned>(loopHwm),
        static_cast<unsigned>(espNowHwm),
        wifiConnected_ ? 1 : 0,
        mqttConnected_ ? 1 : 0,
        slavesOnline_,
        sslBusy_ ? 1 : 0,
        dilPhase_);

    windowStartMs_ = now;
    loopCount_ = 0;
}

}  // namespace ResourceTelemetry

#else  // !RESOURCE_SERIAL_DEBUG

namespace ResourceTelemetry {

void setEspNowTaskHandle(TaskHandle_t) {}
void setContext(bool, bool, const char*, bool, int) {}
void tick() {}

}  // namespace ResourceTelemetry

#endif
