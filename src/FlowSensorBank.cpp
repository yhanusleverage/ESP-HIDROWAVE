#include "FlowSensorBank.h"
#include "Config.h"
#include <Arduino.h>

FlowSensorBank::FlowSensorBank() {
  for (uint8_t i = 0; i < MAX_FLOW_SENSORS; i++) {
    slots_[i].enabled = false;
    slots_[i].id = i;
    slots_[i].role = FLOW_ROLE_GENERIC;
    slots_[i].sensor = nullptr;
  }
}

void FlowSensorBank::beginDefaultDilution(uint8_t pulsePin,
                                          float pulseFactor,
                                          float calibrationFactor,
                                          unsigned long windowMs) {
  (void)pulseFactor;
  (void)windowMs;
  if (slots_[0].sensor != nullptr) {
    return;
  }
  slots_[0].enabled = true;
  slots_[0].id = 0;
  slots_[0].role = FLOW_ROLE_DILUTION;
  slots_[0].sensor = new WaterFlowSensor(pulsePin, calibrationFactor);
  slots_[0].sensor->begin();
#if FLOW_SERIAL_DEBUG
  Serial.printf("[FLOW] bank slot0 dilution GPIO%d K=%.3f (FALLING+debounce+filter)\n",
                pulsePin, calibrationFactor);
#endif
}

void FlowSensorBank::tickAll() {
  for (uint8_t i = 0; i < MAX_FLOW_SENSORS; i++) {
    if (slots_[i].enabled && slots_[i].sensor != nullptr) {
      slots_[i].sensor->tick();
    }
  }
}

WaterFlowSensor* FlowSensorBank::sensor(uint8_t id) const {
  if (id >= MAX_FLOW_SENSORS || !slots_[id].enabled) {
    return nullptr;
  }
  return slots_[id].sensor;
}

WaterFlowSensor* FlowSensorBank::dilutionSensor() const {
  for (uint8_t i = 0; i < MAX_FLOW_SENSORS; i++) {
    if (slots_[i].enabled && slots_[i].role == FLOW_ROLE_DILUTION) {
      return slots_[i].sensor;
    }
  }
  return sensor(0);
}

uint8_t FlowSensorBank::enabledCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_FLOW_SENSORS; i++) {
    if (slots_[i].enabled && slots_[i].sensor != nullptr) {
      n++;
    }
  }
  return n;
}
