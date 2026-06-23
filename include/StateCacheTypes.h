#ifndef STATE_CACHE_TYPES_H
#define STATE_CACHE_TYPES_H

#include <Arduino.h>

/**
 * @brief Estado cacheado de um relé master (guardado em NVS LOCAL)
 */
struct CachedMasterRelayState {
    uint8_t relayNumber;
    uint8_t state;
    uint8_t hasTimer;
    uint16_t remainingTime;
    uint32_t timestamp;
    uint8_t relayType;  // 0=doser, 1=level, 2=reserved
    uint8_t padding[2];
} __attribute__((packed));

struct MasterRelayStatesCache {
    uint32_t timestamp;
    uint8_t version;
    uint8_t numRelays;
    uint8_t padding[2];
    CachedMasterRelayState states[16];
    uint8_t checksum;
} __attribute__((packed));

struct EcPhBootSnapshot {
    uint32_t timestamp;
    uint8_t wasInterrupted;
    uint8_t padding[3];
    char lastEcState[24];
    char lastPhState[24];
    uint8_t checksum;
} __attribute__((packed));

#endif
