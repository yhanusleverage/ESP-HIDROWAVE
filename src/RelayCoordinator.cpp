#include "RelayCoordinator.h"
#include "HydroControl.h"
#include <string.h>
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

const char* relayDenyReasonName(RelayDenyReason reason) {
    switch (reason) {
        case RelayDenyReason::Ok: return "Ok";
        case RelayDenyReason::BlockedBit: return "BlockedBit";
        case RelayDenyReason::DilutionHold: return "DilutionHold";
        case RelayDenyReason::CirculationConflict: return "CirculationConflict";
        case RelayDenyReason::SlaveOffline: return "SlaveOffline";
        case RelayDenyReason::WaterInterlock: return "WaterInterlock";
        case RelayDenyReason::OwnerDenied: return "OwnerDenied";
        case RelayDenyReason::InvalidTarget: return "InvalidTarget";
        default: return "Unknown";
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
      circulationRefCount(0),
      lastDenyReason_(RelayDenyReason::Ok),
      waterInterlockEnabled_(false) {
    memset(circulationMac, 0, sizeof(circulationMac));
    memset(&localBank, 0, sizeof(localBank));
    memset(slaveBanks, 0, sizeof(slaveBanks));
}

bool RelayCoordinator::isAutomationOwner(RelayOwner owner) {
    return owner == RelayOwner::DecisionRule ||
           owner == RelayOwner::ScheduleP4 ||
           owner == RelayOwner::TankScriptP1;
}

RelayDenyReason RelayCoordinator::mayExecute(
    RelayOwner owner,
    const RelayTarget& target,
    RelayActuationAction action) const {
    const bool turningOn = (action == RelayActuationAction::On || action == RelayActuationAction::Toggle);
    const bool turningOff = (action == RelayActuationAction::Off);

    if (target.relay < 0 || target.relay > 15) {
        lastDenyReason_ = RelayDenyReason::InvalidTarget;
        return lastDenyReason_;
    }

    if (owner == RelayOwner::Manual && turningOn && hydroControl &&
        hydroControl->holdsDilutionValve(target.isLocal, target.slaveMac, target.relay)) {
        lastDenyReason_ = RelayDenyReason::DilutionHold;
        return lastDenyReason_;
    }

    const OccupancyBank* bank = target.isLocal ? &localBank : bankForMacConst(target.slaveMac);
    if (bank && target.relay >= 0 && target.relay < 8) {
        if (bitBlocked(bank, target.relay) && owner != RelayOwner::AutoEcDilution) {
            lastDenyReason_ = RelayDenyReason::BlockedBit;
            return lastDenyReason_;
        }
    }

    if (isCirculationTarget(target)) {
        if (turningOff && circulationRefCount > 0 &&
            owner != RelayOwner::None &&
            circulationOwner != RelayOwner::None &&
            owner != circulationOwner) {
            lastDenyReason_ = RelayDenyReason::CirculationConflict;
            return lastDenyReason_;
        }
    }

    if (!target.isLocal && isAutomationOwner(owner) && slaveReachableCb_) {
        if (!slaveReachableCb_(target.slaveMac)) {
            lastDenyReason_ = RelayDenyReason::SlaveOffline;
            return lastDenyReason_;
        }
    }

    if (waterInterlockEnabled_ && waterLevelOkCb_ && isAutomationOwner(owner) && turningOn) {
        if (!waterLevelOkCb_()) {
            lastDenyReason_ = RelayDenyReason::WaterInterlock;
            return lastDenyReason_;
        }
    }

    lastDenyReason_ = RelayDenyReason::Ok;
    return lastDenyReason_;
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
    Serial.printf("[COORD] Circulation target set: %s relay %d\n",
        ESPNowController::macToString(circulationMac).c_str(),
        circulationRelay + 1);
}

void RelayCoordinator::clearCirculationTarget() {
    circulationConfigured = false;
    memset(circulationMac, 0, sizeof(circulationMac));
    circulationRelay = CIRCULATION_RELAY_DEFAULT;
    PreferencesManager::removeConfig(KEY_CIRC_MAC);
    PreferencesManager::removeConfig(KEY_CIRC_RELAY);
    Serial.println("[COORD] Circulation target cleared");
}

CirculationMixGate RelayCoordinator::getCirculationMixGate() const {
    if (!circulationConfigured) {
        return CirculationMixGate::NotTyped;
    }
    const ObservedRelayState observed = getObservedState(getCirculationTarget());
    if (!observed.valid || !observed.state) {
        return CirculationMixGate::Inactive;
    }
    return CirculationMixGate::Ok;
}

bool RelayCoordinator::isCirculationMixActiveForDosing() const {
    return getCirculationMixGate() == CirculationMixGate::Ok;
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

    TrustedSlave* slave = masterManager->getTrustedSlave(target.slaveMac);
    if (!slave) {
        return observed;
    }
    if (target.relay < 0 || target.relay >= slave->numRelays || target.relay >= 8) {
        return observed;
    }
    const auto& relay = slave->relayStates[target.relay];
    observed.valid = true;
    observed.state = relay.state;
    observed.hasTimer = relay.hasTimer;
    observed.remainingSec = relay.remainingTime;
    observed.lastUpdateMs = relay.lastUpdate;
    observed.online = slave->isOnline();
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

    if (action == "on" || action == "on_forever" || action == "timed_on") {
        const int seconds = (action == "on_forever") ? 0 : durationSec;
        return hydroControl->setRelay(relay, true, seconds);
    }
    if (action == "off" || action == "cycle_stop") {
        return hydroControl->setRelay(relay, false, 0);
    }
    if (action == "toggle") {
        return hydroControl->toggleRelay(relay, durationSec > 0 ? durationSec : 0);
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

    const RelayDenyReason deny = mayExecute(owner, target, action);
    if (deny != RelayDenyReason::Ok) {
        Serial.printf("[COORD] deny owner=%s reason=%s R%d\n",
            relayOwnerName(owner), relayDenyReasonName(deny), target.relay + 1);
        return 0;
    }

    OccupancyBank* bank = target.isLocal ? &localBank : bankForMac(target.slaveMac, true);
    if (bank && target.relay >= 0 && target.relay < 8) {
        if (owner == RelayOwner::AutoEcDilution && turningOn) {
            bank->blockedBits |= (uint8_t)(1u << target.relay);
        }
        if (turningOff && owner == RelayOwner::AutoEcDilution) {
            bank->blockedBits &= (uint8_t)~(1u << target.relay);
        }
    }

    if (isCirculationTarget(target) && turningOn && owner != RelayOwner::None) {
        claimCirculationOwner(owner);
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

RelayCoordinator::OccupancyBank* RelayCoordinator::bankForMac(const uint8_t mac[6], bool create) {
    if (!mac) {
        return nullptr;
    }
    for (int i = 0; i < 4; i++) {
        if (slaveBanks[i].used && macEquals(slaveBanks[i].mac, mac)) {
            return &slaveBanks[i];
        }
    }
    if (!create) {
        return nullptr;
    }
    for (int i = 0; i < 4; i++) {
        if (!slaveBanks[i].used) {
            slaveBanks[i].used = true;
            memcpy(slaveBanks[i].mac, mac, 6);
            return &slaveBanks[i];
        }
    }
    return &slaveBanks[0];
}

const RelayCoordinator::OccupancyBank* RelayCoordinator::bankForMacConst(const uint8_t mac[6]) const {
    if (!mac) {
        return nullptr;
    }
    for (int i = 0; i < 4; i++) {
        if (slaveBanks[i].used && macEquals(slaveBanks[i].mac, mac)) {
            return &slaveBanks[i];
        }
    }
    return nullptr;
}

bool RelayCoordinator::bitBlocked(const OccupancyBank* bank, int relay) const {
    if (!bank || relay < 0 || relay > 7) {
        return false;
    }
    return (bank->blockedBits & (uint8_t)(1u << relay)) != 0;
}

uint8_t RelayCoordinator::blockedMaskFor(const uint8_t mac[6]) const {
    const OccupancyBank* b = bankForMacConst(mac);
    return b ? b->blockedBits : 0;
}

void RelayCoordinator::noteObservedMask(const uint8_t mac[6], uint8_t bitsOn) {
    OccupancyBank* b = bankForMac(mac, true);
    if (!b) {
        return;
    }
    b->bitsOn = bitsOn;
    b->updatedMs = millis();
}

uint32_t RelayCoordinator::requestMask(RelayOwner owner, const uint8_t mac[6], uint8_t mask,
                                         uint16_t durationSec) {
    if (!masterManager || !mac) {
        lastDenyReason_ = RelayDenyReason::InvalidTarget;
        return 0;
    }

    if (isAutomationOwner(owner) && slaveReachableCb_ && !slaveReachableCb_(mac)) {
        lastDenyReason_ = RelayDenyReason::SlaveOffline;
        Serial.printf("[COORD] deny owner=%s reason=SlaveOffline mask=0x%02X\n",
                      relayOwnerName(owner), mask);
        return 0;
    }
    if (waterInterlockEnabled_ && waterLevelOkCb_ && isAutomationOwner(owner) && mask != 0) {
        if (!waterLevelOkCb_()) {
            lastDenyReason_ = RelayDenyReason::WaterInterlock;
            Serial.printf("[COORD] deny owner=%s reason=WaterInterlock mask=0x%02X\n",
                          relayOwnerName(owner), mask);
            return 0;
        }
    }

    OccupancyBank* bank = bankForMac(mac, true);
    uint8_t blocked = bank ? bank->blockedBits : 0;
    uint8_t apply = mask & (uint8_t)~blocked;
    if (apply != mask) {
        Serial.printf("[PROC] mask 0x%02X -> 0x%02X deny=0x%02X owner=%s\n",
                      mask, apply, blocked, relayOwnerName(owner));
    }
    if (apply == 0 && mask != 0) {
        lastDenyReason_ = RelayDenyReason::BlockedBit;
        Serial.printf("[COORD] deny owner=%s reason=BlockedBit mask=0x%02X\n",
                      relayOwnerName(owner), mask);
        return 0;
    }

    lastDenyReason_ = RelayDenyReason::Ok;
    uint32_t id = masterManager->sendRelayMaskToSlave(mac, apply, durationSec, 0);
    if (id > 0 && bank) {
        bank->bitsOn = apply;
        bank->updatedMs = millis();
        for (int i = 0; i < 8; i++) {
            if (apply & (1u << i)) {
                bank->owners[i] = owner;
            }
        }
    }
    Serial.printf("[PROC] owner=%s mask=0x%02X deny=0x%02X id=%u\n",
                  relayOwnerName(owner), apply, blocked, (unsigned)id);
    return id;
}
