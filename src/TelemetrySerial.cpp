#include "TelemetrySerial.h"
#include <math.h>

void printTelemetrySerialLine(const MqttTelemetryReading& reading) {
    Serial.print("[TELEMETRIA MQTT] EC: ");
    if (reading.ecValid && isfinite(reading.ec)) {
        Serial.printf("%.0f uS/cm", reading.ec);
    } else {
        Serial.print("--");
    }
    Serial.print(" | pH: ");
    if (reading.phValid && isfinite(reading.ph)) {
        Serial.printf("%.2f", reading.ph);
    } else {
        Serial.print("--");
    }
    Serial.print(" | Temp agua: ");
    if (reading.tempValid && isfinite(reading.temperature)) {
        Serial.printf("%.1f C", reading.temperature);
    } else {
        Serial.print("--");
    }
    Serial.printf(" | Temp ar: %.1f C | Umidade: %.0f%% | Nivel: %s (L1-%s L4-%s)\n",
        reading.airTemperature,
        reading.humidity,
        reading.waterLevel ? reading.waterLevel : "?",
        reading.level1Wet ? "ON" : "OFF",
        reading.level4Wet ? "ON" : "OFF");
}
