#include "TelemetrySerial.h"

void printTelemetrySerialLine(const MqttTelemetryReading& reading) {
    const float ecUsCm = reading.tds * 2.0f;
    Serial.printf(
        "[TELEMETRIA MQTT] EC: %.0f uS/cm | pH: %.2f | Temp agua: %.1f C | Temp ar: %.1f C | Umidade: %.0f%% | Nivel: %s (L1-%s L4-%s)\n",
        ecUsCm,
        reading.ph,
        reading.temperature,
        reading.airTemperature,
        reading.humidity,
        reading.waterLevel ? reading.waterLevel : "?",
        reading.level1Wet ? "ON" : "OFF",
        reading.level4Wet ? "ON" : "OFF");
}
