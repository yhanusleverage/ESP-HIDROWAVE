#ifndef RESOURCE_TELEMETRY_H
#define RESOURCE_TELEMETRY_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * Telemetría de contención Master: heap + stack HWM + loop/s.
 * Sin task propia — tick desde HydroSystemCore::loop / registro de handle en main.
 * Ver docs/handoffs/firmware/ESP32_MASTER_RESOURCE_MAP.md
 */
namespace ResourceTelemetry {

void setEspNowTaskHandle(TaskHandle_t handle);

/**
 * Contexto de red / dilución (cada HydroSystemCore::loop).
 * wifiConnected: WiFi.status() == WL_CONNECTED
 * slavesOnline: MasterSlaveManager::getOnlineSlaveCount() (0 si no hay manager)
 */
void setContext(bool mqttConnected, bool sslBusy, const char* dilPhase,
                bool wifiConnected, int slavesOnline);

/**
 * Cuenta un tick de loop y, si toca, imprime [RES].
 * Llamar al final de HydroSystemCore::loop (o main si no hay core hidro).
 */
void tick();

}  // namespace ResourceTelemetry

#endif
