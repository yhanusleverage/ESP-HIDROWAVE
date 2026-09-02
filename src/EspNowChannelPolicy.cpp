#include "EspNowChannelPolicy.h"
#include "ESPNowController.h"
#include "MasterSlaveManager.h"
#include "Config.h"
#include "PreferencesManager.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace {
    bool mqttConnectedFlag = false;
    const unsigned long bootMs = millis();

    static constexpr const char* NVS_CH_KEY = "espnow_ch";
    static constexpr const char* NVS_CH_KEY_LEGACY = "espnow_last_channel";

    void sendProvisioningBurstOnCurrentChannel(ESPNowController* controller,
                                               const String& ssid,
                                               const String& password,
                                               uint8_t opChannel) {
        for (int i = 0; i < 3; ++i) {
            controller->sendWiFiCredentialsBroadcast(ssid, password, opChannel);
            delay(150);
        }
        for (int i = 0; i < 3; ++i) {
            controller->sendDiscoveryBroadcast();
            if (i < 2) {
                delay(150);
            }
        }
    }
}

uint8_t EspNowChannelPolicy::getOperationalChannel() {
    if (WiFi.isConnected()) {
        const uint8_t ch = WiFi.channel();
        if (ch >= 1 && ch <= 13) {
            return ch;
        }
    }
    const uint8_t last = loadLastChannelFromNvs();
    if (last >= 1 && last <= 13) {
        return last;
    }
    return ESPNOW_CHANNEL;
}

bool EspNowChannelPolicy::hopToConfigChannel(ESPNowController* controller) {
    if (!controller) {
        return false;
    }
    return controller->hopToChannel(ESPNOW_CONFIG_CHANNEL);
}

bool EspNowChannelPolicy::hopToOperationalChannel(ESPNowController* controller, uint8_t opChannel) {
    if (!controller || opChannel < 1 || opChannel > 13) {
        return false;
    }
    return controller->hopToChannel(opChannel);
}

bool EspNowChannelPolicy::loadWifiCredentials(String& ssid, String& password, uint8_t& channel) {
    ssid = "";
    password = "";
    channel = 0;

    Preferences hydro;
    if (hydro.begin("hydro_system", true)) {
        ssid = hydro.getString("ssid", "");
        password = hydro.getString("password", "");
        channel = hydro.getUChar("wifi_chan", 0);
        hydro.end();
        if (ssid.length() > 0) {
            return true;
        }
    }

    if (PreferencesManager::loadWiFiCredentials(ssid, password, channel)) {
        return true;
    }

    if (WiFi.isConnected()) {
        ssid = WiFi.SSID();
        password = "";
        channel = WiFi.channel();
        return ssid.length() > 0;
    }

    return false;
}

void EspNowChannelPolicy::runProvisioningBurst(ESPNowController* controller,
                                               MasterSlaveManager* manager,
                                               const String& ssid,
                                               const String& password,
                                               uint8_t opChannel) {
    if (!controller || !manager || ssid.length() == 0) {
        return;
    }
    if (opChannel < 1 || opChannel > 13) {
        opChannel = getOperationalChannel();
    }

    const bool staUp = WiFi.isConnected();
    const bool configHop = hopToConfigChannel(controller);

    if (configHop) {
        Serial.printf("[CHANNEL] provisioning hop config=%u op=%u\n",
                      static_cast<unsigned>(ESPNOW_CONFIG_CHANNEL),
                      static_cast<unsigned>(opChannel));
        delay(50);
        sendProvisioningBurstOnCurrentChannel(controller, ssid, password, opChannel);
        if (staUp && opChannel != ESPNOW_CONFIG_CHANNEL) {
            hopToOperationalChannel(controller, opChannel);
            delay(50);
        }
    } else if (staUp) {
        Serial.printf("[CHANNEL] hop config skip (STA ch=%u) — burst op direct\n",
                      static_cast<unsigned>(WiFi.channel()));
        sendProvisioningBurstOnCurrentChannel(controller, ssid, password, opChannel);
    } else {
        Serial.println("[CHANNEL] hop config fail, sem STA — burst no canal atual");
        sendProvisioningBurstOnCurrentChannel(controller, ssid, password, opChannel);
    }

    manager->refreshEspNowPeersOnCurrentChannel();
    if (staUp) {
        saveLastChannelToNvs(opChannel);
    }
    Serial.println("[RES] radio=provisioning");
}

void EspNowChannelPolicy::checkStaChannelChange(ESPNowController* controller,
                                                MasterSlaveManager* manager) {
    if (!controller || !manager || !WiFi.isConnected()) {
        return;
    }

    const uint8_t current = WiFi.channel();
    if (current < 1 || current > 13) {
        return;
    }

    const uint8_t last = loadLastChannelFromNvs();
    if (last == 0 || last == current) {
        if (last == 0) {
            saveLastChannelToNvs(current);
        }
        return;
    }

    Serial.printf("[CHANNEL-SWITCH] old=%u new=%u hop_back=1\n",
                  static_cast<unsigned>(last),
                  static_cast<unsigned>(current));

    if (controller->sendChannelChangeNotification(last, current, 1)) {
        saveLastChannelToNvs(current);
        manager->refreshEspNowPeersOnCurrentChannel();
        Serial.println("[RES] radio=operational");
    }
}

void EspNowChannelPolicy::setMqttConnected(bool connected) {
    mqttConnectedFlag = connected;
}

bool EspNowChannelPolicy::canRunEspNowDiscovery() {
    if (mqttConnectedFlag) {
        return true;
    }
    return isProvisioningWindowActive();
}

bool EspNowChannelPolicy::isProvisioningWindowActive() {
#if ESPNOW_PROVISIONING_ENABLED
    return (millis() - bootMs) < ESPNOW_PROVISIONING_BURST_MS;
#else
    return false;
#endif
}

void EspNowChannelPolicy::tickProvisioningCountdown() {
#if ESPNOW_PROVISIONING_ENABLED
    if (!isProvisioningWindowActive()) {
        return;
    }
    static unsigned long lastWholeSec = ULONG_MAX;
    const unsigned long elapsed = millis() - bootMs;
    const unsigned long wholeSec = elapsed / 1000UL;
    if (wholeSec == lastWholeSec) {
        return;
    }
    lastWholeSec = wholeSec;
    const unsigned long remMs = (elapsed < ESPNOW_PROVISIONING_BURST_MS)
        ? (ESPNOW_PROVISIONING_BURST_MS - elapsed) : 0;
    const unsigned long remSec = (remMs + 999UL) / 1000UL;
    uint8_t ch = 1;
    if (WiFi.status() == WL_CONNECTED) {
        wifi_second_chan_t secondChan;
        esp_wifi_get_channel(&ch, &secondChan);
    }
    Serial.printf("[PROV] burst STA ch%u — faltam %lus\n", ch, remSec);
#endif
}

uint8_t EspNowChannelPolicy::loadLastChannelFromNvs() {
    Preferences prefs;
    if (!prefs.begin("hidro_state", true)) {
        return 0;
    }
    uint8_t ch = prefs.getUChar(NVS_CH_KEY, 0);
    if (ch == 0) {
        ch = prefs.getUChar(NVS_CH_KEY_LEGACY, 0);
    }
    prefs.end();
    return ch;
}

void EspNowChannelPolicy::saveLastChannelToNvs(uint8_t channel) {
    if (channel < 1 || channel > 13) {
        return;
    }
    Preferences prefs;
    if (!prefs.begin("hidro_state", false)) {
        return;
    }
    prefs.putUChar(NVS_CH_KEY, channel);
    prefs.end();
}
