#ifndef TELEMETRY_SERIAL_H
#define TELEMETRY_SERIAL_H

#include "MqttClient.h"

/** Uma linha concisa — mesmos campos do payload MQTT telemetry. */
void printTelemetrySerialLine(const MqttTelemetryReading& reading);

#endif
