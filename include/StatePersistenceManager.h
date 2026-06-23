#ifndef STATE_PERSISTENCE_MANAGER_H
#define STATE_PERSISTENCE_MANAGER_H

#include <Arduino.h>
#include <nvs.h>
#include "StateCacheTypes.h"

class HydroControl;

/**
 * @brief NVS unificado (namespace hidro_state) para estados operacionais
 */
class StatePersistenceManager {
public:
    static const uint32_t MAX_CACHE_AGE_MS = 300000UL; // 5 min

    static bool saveMasterRelayCache(const MasterRelayStatesCache& cache);
    static bool loadMasterRelayCache(MasterRelayStatesCache& cache);

    static bool saveEcPhBootSnapshot(const char* ecState, const char* phState, bool interrupted);
    static bool loadEcPhBootSnapshot(EcPhBootSnapshot& snapshot);

    /** Política selective: dosadores OFF, level/reserved restore se cache válido */
    static bool applySelectiveMasterRelayRestore(HydroControl& hydroControl);

    static uint8_t computeChecksum(const uint8_t* data, size_t len);
};

#endif
