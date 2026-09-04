#ifndef ESPNOW_CHANNEL_POLICY_H
#define ESPNOW_CHANNEL_POLICY_H

#include <Arduino.h>

class ESPNowController;
class MasterSlaveManager;

/**
 * @brief Política de canal ESP-NOW: provisioning CONFIG, operação STA, CHANNEL_CHANGE
 */
class EspNowChannelPolicy {
public:
    static uint8_t getOperationalChannel();
    static bool hopToConfigChannel(ESPNowController* controller);
    static bool hopToOperationalChannel(ESPNowController* controller, uint8_t opChannel);

    static void runProvisioningBurst(ESPNowController* controller,
                                     MasterSlaveManager* manager,
                                     const String& ssid,
                                     const String& password,
                                     uint8_t opChannel);

    static void checkStaChannelChange(ESPNowController* controller, MasterSlaveManager* manager);

    static void setMqttConnected(bool connected);
    static bool canRunEspNowDiscovery();

    static uint8_t loadLastChannelFromNvs();
    static void saveLastChannelToNvs(uint8_t channel);

    static bool isProvisioningWindowActive();
    /** Uma linha/s durante janela de provisioning (serial legível) */
    static void tickProvisioningCountdown();

    /** true durante WiFi.disconnect → ch11 → burst → reconnect */
    static bool isProvisioningStaSuspendActive();

    /** SSID/senha: hydro_system → PreferencesManager → WiFi STA */
    static bool loadWifiCredentials(String& ssid, String& password, uint8_t& channel);

    /** Provisioning burst: exige SSID+password em NVS (sem fallback STA sem senha) */
    static bool loadWifiCredentialsForProvisioning(String& ssid, String& password, uint8_t& channel);
};

#endif  // ESPNOW_CHANNEL_POLICY_H
