#ifndef RELAY_COORDINATOR_H
#define RELAY_COORDINATOR_H

#include <Arduino.h>
#include <functional>

class HydroControl;
class MasterSlaveManager;

enum class RelayOwner : uint8_t {
    None = 0,
    AutoEcRecirc,
    AutoPhRecirc,
    AutoEcDilution,
    ScheduleP4,
    TankScriptP1,
    Manual,          // UI / cloud manual
    DecisionRule,
    AutoSync,        // poll status ALL_RELAYS
    Retry            // reenvío cola ESP-NOW
};

enum class RelayActuationAction : uint8_t {
    Off = 0,
    On,
    Toggle
};

/** Gate de mezcla Auto EC/pH (bomba de circulação tipada). */
enum class CirculationMixGate : uint8_t {
    Ok = 0,
    NotTyped = 1,
    Inactive = 2
};

/** Política pura — sin I/O. Ver docs/handoffs/HANDOFF_RELAY_ROUTER.md */
enum class RelayDenyReason : uint8_t {
    Ok = 0,
    BlockedBit,
    DilutionHold,
    CirculationConflict,
    SlaveOffline,
    WaterInterlock,
    OwnerDenied,
    InvalidTarget
};

struct RelayTarget {
    bool isLocal;
    int relay;
    uint8_t slaveMac[6];

    static RelayTarget local(int relayNumber);
    static RelayTarget remote(const uint8_t mac[6], int relayNumber);
    bool matchesMac(const uint8_t mac[6]) const;
};

struct ObservedRelayState {
    bool valid;
    bool state;
    bool hasTimer;
    uint16_t remainingSec;
    unsigned long lastUpdateMs;
    bool online;
};

class RelayCoordinator {
public:
    using SlaveReachableFn = std::function<bool(const uint8_t mac[6])>;
    using WaterLevelOkFn = std::function<bool()>;

    RelayCoordinator();

    void begin(HydroControl* hydro, MasterSlaveManager* masterManager);
    void loadConfigFromNVS();
    void setCirculationTarget(const uint8_t mac[6], int relayNumber);
    void clearCirculationTarget();
    /**
     * Interlock de mezcla Auto EC/pH:
     * - sin tipagem → NotTyped (bloquea)
     * - tipada + OFF/inválido → Inactive (bloquea)
     * - tipada + ON → Ok
     */
    CirculationMixGate getCirculationMixGate() const;
    bool isCirculationMixActiveForDosing() const;

    void setSlaveReachableCallback(SlaveReachableFn cb) { slaveReachableCb_ = cb; }
    /** Water interlock: callback + flag (default off — no romper bancada). */
    void setWaterLevelOkCallback(WaterLevelOkFn cb, bool enabled = false) {
        waterLevelOkCb_ = cb;
        waterInterlockEnabled_ = enabled;
    }
    void setWaterInterlockEnabled(bool enabled) { waterInterlockEnabled_ = enabled; }

    bool isCirculationConfigured() const { return circulationConfigured; }
    RelayTarget getCirculationTarget() const;

    ObservedRelayState getObservedState(const RelayTarget& target) const;
    RelayOwner getOwner(const RelayTarget& target) const;
    bool isCirculationTarget(const RelayTarget& target) const;

    /** Solo política. Actualiza lastDenyReason_. */
    RelayDenyReason mayExecute(
        RelayOwner owner,
        const RelayTarget& target,
        RelayActuationAction action) const;

    RelayDenyReason lastDenyReason() const { return lastDenyReason_; }

    uint32_t requestActuation(
        RelayOwner owner,
        const RelayTarget& target,
        RelayActuationAction action,
        uint32_t durationSec = 0,
        int supabaseCommandId = 0,
        int cycleOffSec = 0,
        const String& commandMode = "",
        const char* pathRuleId = nullptr);

    bool releaseActuation(RelayOwner owner, const RelayTarget& target, bool tryOff = true);

    bool startPostDoseRecirc(RelayOwner owner);
    bool endPostDoseRecirc(RelayOwner owner);

    bool actuateLocal(RelayOwner owner, int relay, const String& action, int durationSec,
                      const char* pathRuleId = nullptr);
    uint32_t actuateSlave(
        RelayOwner owner,
        const uint8_t mac[6],
        int relay,
        const String& action,
        int durationSec,
        int supabaseCommandId = 0,
        int cycleOffSec = 0,
        const String& commandMode = "",
        const char* pathRuleId = nullptr);

    /** Máscara atómica (on_all/off_all). Bits bloqueados se quitan. */
    uint32_t requestMask(RelayOwner owner, const uint8_t mac[6], uint8_t mask,
                         uint16_t durationSec = 0, const char* pathRuleId = nullptr);
    void noteObservedMask(const uint8_t mac[6], uint8_t bitsOn);
    uint8_t blockedMaskFor(const uint8_t mac[6]) const;

private:
    struct OccupancyBank {
        uint8_t bitsOn = 0;
        uint8_t blockedBits = 0;
        RelayOwner owners[8] = {};
        unsigned long updatedMs = 0;
        uint8_t mac[6] = {};
        bool used = false;
    };
    OccupancyBank localBank;
    OccupancyBank slaveBanks[4];
    OccupancyBank* bankForMac(const uint8_t mac[6], bool create);
    const OccupancyBank* bankForMacConst(const uint8_t mac[6]) const;
    HydroControl* hydroControl;
    MasterSlaveManager* masterManager;

    bool circulationConfigured;
    uint8_t circulationMac[6];
    int circulationRelay;
    RelayOwner circulationOwner;
    uint8_t circulationRefCount;

    mutable RelayDenyReason lastDenyReason_;
    SlaveReachableFn slaveReachableCb_;
    WaterLevelOkFn waterLevelOkCb_;
    bool waterInterlockEnabled_;

    static bool isAutomationOwner(RelayOwner owner);
    bool claimCirculationOwner(RelayOwner owner);
    bool releaseCirculationOwner(RelayOwner owner, bool tryOff);
    bool executeLocalRelay(int relay, const String& action, int durationSec);
    bool bitBlocked(const OccupancyBank* bank, int relay) const;
    uint32_t executeSlaveRelay(
        const uint8_t mac[6],
        int relay,
        const String& action,
        int durationSec,
        int supabaseCommandId,
        int cycleOffSec = 0,
        const String& commandMode = "");
    static bool macEquals(const uint8_t* a, const uint8_t* b);
    static bool parseMacString(const String& macStr, uint8_t* macOut);
};

const char* relayOwnerName(RelayOwner owner);
/** Etiqueta corta para serial [PATH] (Manual → UI). */
const char* pathOwnerLabel(RelayOwner owner);
const char* relayDenyReasonName(RelayDenyReason reason);
/** Una línea: [PATH] owner=… [rule=…] local|slave R0 on|off */
void logRelayPath(RelayOwner owner, bool isLocal, int relayZeroBased, const char* action,
                  const char* pathRuleId = nullptr);

#endif
