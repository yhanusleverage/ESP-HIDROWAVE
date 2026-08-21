#ifndef FLOW_SENSOR_BANK_H
#define FLOW_SENSOR_BANK_H

#include "WaterFlowSensor.h"

/**
 * Banco escalable 1..N de fluxómetros (volumen A→B por sesión).
 * Slot 0 = YF-B5 dilución (GPIO FLOW_SENSOR_PIN). Slots 1..N deshabilitados hasta config.
 * Sin task FreeRTOS: tickAll() desde HydroControl::update().
 */
static const uint8_t MAX_FLOW_SENSORS = 4;

enum FlowSensorRole : uint8_t {
  FLOW_ROLE_DILUTION = 0,
  FLOW_ROLE_DRAIN = 1,
  FLOW_ROLE_FILL = 2,
  FLOW_ROLE_IRRIGATE = 3,
  FLOW_ROLE_GENERIC = 4,
};

struct FlowSensorSlot {
  bool enabled;
  uint8_t id;
  FlowSensorRole role;
  WaterFlowSensor* sensor;
};

class FlowSensorBank {
public:
  FlowSensorBank();

  /** Inicializa slot 0 (dilución) con el YFB5 actual. */
  void beginDefaultDilution(uint8_t pulsePin,
                            float pulseFactor,
                            float calibrationFactor,
                            unsigned long windowMs);

  void tickAll();

  WaterFlowSensor* sensor(uint8_t id) const;
  WaterFlowSensor* dilutionSensor() const;
  uint8_t enabledCount() const;

  const FlowSensorSlot& slot(uint8_t id) const { return slots_[id < MAX_FLOW_SENSORS ? id : 0]; }

private:
  FlowSensorSlot slots_[MAX_FLOW_SENSORS];
};

#endif
