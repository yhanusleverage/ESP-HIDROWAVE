#include "RelayCoordinator.h"
#include "HydroControl.h"
#include "MasterSlaveManager.h"
#include "ESPNowController.h"
#include "PreferencesManager.h"
#include "Config.h"

static const char* KEY_CIRC_MAC = "circ_slave_mac";
static const char* KEY_CIRC_RELAY = "circ_relay_id";

const char* relayOwnerName(RelayOwner owner) {
    switch (owner) {
        case RelayOwner::AutoEcRecirc: return "AutoEcRecirc";
        case RelayOwner::AutoPhRecirc: return "AutoPhRecirc";
        case RelayOwner::AutoEcDilution: return "AutoEcDilution";
        case RelayOwner::ScheduleP4: return "ScheduleP4";
        case RelayOwner::TankScriptP1: return "TankScriptP1";
        case RelayOwner::Manual: return "Manual";
        case RelayOwner::DecisionRule: return "DecisionRule";
        default: return "None";
    }
}

RelayTarget RelayTarget::local(int relayNumber) {
    RelayTarget target = {};
    target.isLocal = true;
    target.relay = relayNumber;
    memset(target.slaveMac, 0, sizeof(target.slaveMac));
    return target;
}

RelayTarget RelayTarget::remote(const uint8_t mac[6], int relayNumber) {
    RelayTarget target = {};
    target.isLocal = false;
    target.relay = relayNumber;
    if (mac) {
        memcpy(target.slaveMac, mac, 6);
    }
    return target;
}

bool RelayTarget::matchesMac(const uint8_t mac[6]) const {
    if (isLocal || !mac) {
        return false;
    }
    return memcmp(slaveMac, mac, 6) == 0;
}

RelayCoordinator::RelayCoordinator()
    : hydroControl(nullptr),
      masterManager(nullptr),
      circulationConfigured(false),
      circulationRelay(CIRCULATION_RELAY_DEFAULT),
      circulationOwner(RelayOwner::None),
      circulationRefCount(0) {
    memset(circulationMac, 0, sizeof(circulationMac));
}

void RelayCoordinator::begin(HydroControl* hydro, MasterSlaveManager* masterManagerPtr) {
    hydroControl = hydro;
    masterManager = masterManagerPtr;
    loadConfigFromNVS();
    Serial.println("[COORD] RelayCoordinator ready");
}

void RelayCoordinator::loadConfigFromNVS() {
    String macStr;
    int32_t relayId = CIRCULATION_RELAY_DEFAULT;
    circulationConfigured = false;
    memset(circulationMac, 0, sizeof(circulationMac));
    circulationRelay = CIRCULATION_RELAY_DEFAULT;

    if (PreferencesManager::loadConfig(KEY_CIRC_MAC, macStr) && macStr.length() >= 11) {
        if (parseMacString(macStr, circulationMac)) {
            circulationConfigured = true;
        }
    }

    if (PreferencesManager::loadConfigInt(KEY_CIRC_RELAY, relayId) && relayId >= 0 && relayId < 8) {
        circulationRelay = (int)relayId;
    }

    if (circulationConfigured) {
        Serial.printf("[COORD] Circulation target: %s relay %d\n",
            ESPNowController::macToString(circulationMac).c_str(),
            circulationRelay + 1);
    } else {
        Serial.println("[COORD] Circulation target not configured (NVS circ_slave_mac)");
    }
}

void RelayCoordinator::setCirculationTarget(const uint8_t mac[6], int relayNumber) {
    if (!mac || relayNumber < 0 || relayNumber >= 8) {
        return;
    }
    memcpy(circulationMac, mac, 6);
    circulationRelay = relayNumber;
    circulationConfigured = true;
    PreferencesManager::saveConfig(KEY_CIRC_MAC, ESPNowController::macToString(circulationMac));
    PreferencesManager::saveConfigInt(KEY_CIRC_RELAY, (int32_t)relayNumber);
}

RelayTarget RelayCoordinator::getCirculationTarget() const {
    return RelayTarget::remote(circulationMac, circulationRelay);
}

bool RelayCoordinator::macEquals(const uint8_t* a, const uint8_t* b) {
    return a && b && memcmp(a, b, 6) == 0;
}

bool RelayCoordinator::parseMacString(const String& macStr, uint8_t* macOut) {
    if (!macOut) {
        return false;
    }
    int values[6];
    int matched = sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x",
        &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]);
    if (matched != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        macOut[i] = (uint8_t)values[i];
    }
    return true;
}

bool RelayCoordinator::isCirculationTarget(const RelayTarget& target) const {
    if (!circulationConfigured || target.isLocal) {
        return false;
    }
    return target.matchesMac(circulationMac) && target.relay == circulationRelay;
}

RelayOwner RelayCoordinator::getOwner(const RelayTarget& target) const {
    if (isCirculationTarget(target)) {
        return circulationOwner;
    }
    return RelayOwner::None;
}

ObservedRelayState RelayCoordinator::getObservedState(const RelayTarget& target) const {
    ObservedRelayState observed = {};
    observed.valid = false;

    if (target.isLocal) {
        if (!hydroControl || target.relay < 0 || target.relay >= 8) {
            return observed;
        }
        bool* relayStates = hydroControl->getRelayStates();
        observed.valid = true;
        observed.state = relayStates[target.relay];
        observed.online = true;
        observed.lastUpdateMs = millis();
        return observed;
    }

    if (!masterManager) {
        return observed;
    }

    auto slaves = masterManager->getAllTrustedSlaves();
    for (const auto& slave : slaves) {
        if (!target.matchesMac(slave.macAddress)) {
            continue;
        }
        if (target.relay < 0 || target.relay >= slave.numRelays || target.relay >= 8) {
            return observed;
        }
        const auto& relay = slave.relayStates[target.relay];
        observed.valid = true;
        observed.state = relay.state;
        observed.hasTimer = relay.hasTimer;
        observed.remainingSec = relay.remainingTime;
        observed.lastUpdateMs = relay.lastUpdate;
        observed.online = slave.isOnline();
        return observed;
    }

    return observed;
}

bool RelayCoordinator::claimCirculationOwner(RelayOwner owner) {
    if (owner == RelayOwner::None) {
        return false;
    }
    if (circulationOwner == RelayOwner::None || circulationOwner == owner) {
        circulationOwner = owner;
        if (circulationRefCount < 255) {
            circulationRefCount++;
        }
        Serial.printf("[COORD] claim owner=%s ref=%u\n", relayOwnerName(owner), circulationRefCount);
        return true;
    }
    if (circulationRefCount < 255) {
        circulationRefCount++;
    }
    Serial.printf("[COORD] claim shared owner=%s existing=%s ref=%u\n",
        relayOwnerName(owner), relayOwnerName(circulationOwner), circulationRefCount);
    return true;
}

bool RelayCoordinator::releaseCirculationOwner(RelayOwner owner, bool tryOff) {
    if (circulationRefCount == 0) {
        circulationOwner = RelayOwner::None;
        return true;
    }

    if (circulationRefCount > 0) {
        circulationRefCount--;
    }

    if (circulationRefCount > 0) {
        Serial.printf("[COORD] release skipped owner=%s ref=%u remaining\n",
            relayOwnerName(owner), circulationRefCount);
        return false;
    }

    circulationOwner = RelayOwner::None;

    if (!tryOff || !circulationConfigured) {
        return true;
    }

    RelayTarget target = getCirculationTarget();
    Serial.printf("[COORD] Post-dose recirc OFF (%s)\n", relayOwnerName(owner));
    return requestActuation(RelayOwner::None, target, RelayActuationAction::Off, 0, 0) > 0;
}

bool RelayCoordinator::executeLocalRelay(int relay, const String& action, int durationSec) {
    if (!hydroControl || relay < 0 || relay >= 8) {
        return false;
    }

    bool* relayStates = hydroControl->getRelayStates();
    if (action == "on") {
        if (!relayStates[relay]) {
            hydroControl->toggleRelay(relay, durationSec);
        }
        return true;
    }
    if (action == "off") {
        if (relayStates[relay]) {
            hydroControl->toggleRelay(relay, 0);
        }
        return true;
    }
    if (action == "toggle") {
        hydroControl->toggleRelay(relay, durationSec > 0 ? durationSec : 0);
        return true;
    }
    return false;
}

uint32_t RelayCoordinator::executeSlaveRelay(
    const uint8_t mac[6],
    int relay,
    const String& action,
    int durationSec,
    int supabaseCommandId,
    int cycleOffSec,
    const String& commandMode) {
    if (!masterManager || !mac) {
        return 0;
    }
    return masterManager->sendRelayCommandToSlave(
        mac, relay, action, durationSec, supabaseCommandId, false, cycleOffSec, commandMode);
}

uint32_t RelayCoordinator::requestActuation(
    RelayOwner owner,
    const RelayTarget& target,
    RelayActuationAction action,
    uint32_t durationSec,
    int supabaseCommandId,
    int cycleOffSec,
    const String& commandMode) {
    const bool turningOn = (action == RelayActuationAction::On || action == RelayActuationAction::Toggle);
    const bool turningOff = (action == RelayActuationAction::Off);

    if (isCirculationTarget(target)) {
        if (turningOff && circulationRefCount > 0 &&
            owner != RelayOwner::None &&
            circulationOwner != RelayOwner::None &&
            owner != circulationOwner) {
            Serial.printf("[COORD] OFF denied owner=%s holder=%s ref=%u\n",
                relayOwnerName(owner), relayOwnerName(circulationOwner), circulationRefCount);
            return 0;
        }
        if (turningOn && owner != RelayOwner::None) {
            claimCirculationOwner(owner);
        }
    }

    String actionStr = turningOff ? "off" : "on";
    if (action == RelayActuationAction::Toggle) {
        actionStr = "toggle";
    }

    bool ok = false;
    uint32_t result = 0;
    if (target.isLocal) {
        ok = executeLocalRelay(target.relay, actionStr, (int)durationSec);
        result = ok ? 1u : 0u;
    } else {
        result = executeSlaveRelay(target.slaveMac, target.relay, actionStr, (int)durationSec,
                                   supabaseCommandId, cycleOffSec, commandMode);
        ok = result > 0;
    }

    if (ok) {
        Serial.printf("[COORD] %s %s relay %d owner=%s dur=%lus\n",
            target.isLocal ? "LOCAL" : "SLAVE",
            actionStr.c_str(),
            target.relay + 1,
            relayOwnerName(owner),
            (unsigned long)durationSec);
    }
    return result;
}

bool RelayCoordinator::releaseActuation(RelayOwner owner, const RelayTarget& target, bool tryOff) {
    if (!isCirculationTarget(target)) {
        return true;
    }
    return releaseCirculationOwner(owner, tryOff);
}

bool RelayCoordinator::startPostDoseRecirc(RelayOwner owner) {
    if (!circulationConfigured) {
        Serial.println("[COORD] Post-dose recirc skipped — circ_slave_mac not set");
        return false;
    }
    if (owner != RelayOwner::AutoEcRecirc && owner != RelayOwner::AutoPhRecirc) {
        return false;
    }

    RelayTarget target = getCirculationTarget();
    Serial.printf("[COORD] Post-dose recirc ON (%s)\n", relayOwnerName(owner));
    return requestActuation(owner, target, RelayActuationAction::On, 0, 0) > 0;
}

bool RelayCoordinator::endPostDoseRecirc(RelayOwner owner) {
    if (!circulationConfigured) {
        return false;
    }
    RelayTarget target = getCirculationTarget();
    return releaseActuation(owner, target, true);
}

bool RelayCoordinator::actuateLocal(RelayOwner owner, int relay, const String& action, int durationSec) {
    RelayTarget target = RelayTarget::local(relay);
    RelayActuationAction act = RelayActuationAction::Toggle;
    if (action == "on") {
        act = RelayActuationAction::On;
    } else if (action == "off") {
        act = RelayActuationAction::Off;
    }
    return requestActuation(owner, target, act, (uint32_t)durationSec, 0) > 0;
}

uint32_t RelayCoordinator::actuateSlave(
    RelayOwner owner,
    const uint8_t mac[6],
    int relay,
    const String& action,
    int durationSec,
    int supabaseCommandId,
    int cycleOffSec,
    const String& commandMode) {
    RelayTarget target = RelayTarget::remote(mac, relay);
    RelayActuationAction act = RelayActuationAction::Toggle;
    if (action == "on" || action == "timed_on") {
        act = RelayActuationAction::On;
    } else if (action == "off" || action == "cycle_stop") {
        act = RelayActuationAction::Off;
    }
    return requestActuation(owner, target, act, (uint32_t)durationSec, supabaseCommandId, cycleOffSec,
                            commandMode);
}
