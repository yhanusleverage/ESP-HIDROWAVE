#include "DiscreteLevelBank.h"
#include "Config.h"

#ifndef LEVEL_SENSOR_PCF_PINS
#define LEVEL_SENSOR_PCF_PINS 0, 1, 2, 3
#endif

static const uint8_t LEVEL_PCF_PINS[DiscreteLevelBank::LEVEL_COUNT] = { LEVEL_SENSOR_PCF_PINS };

DiscreteLevelBank::DiscreteLevelBank()
    : available(false) {
    memset(wet, 0, sizeof(wet));
    memset(stableWet, 0, sizeof(stableWet));
    memset(rawWet, 0, sizeof(rawWet));
    memset(lastChangeMs, 0, sizeof(lastChangeMs));
    strncpy(waterLevelAggregate, "vazio", sizeof(waterLevelAggregate));
    waterLevelAggregate[sizeof(waterLevelAggregate) - 1] = '\0';
}

void DiscreteLevelBank::begin() {
    available = false;
}

bool DiscreteLevelBank::readPinWet(PCF8574& pcf, int pinIndex) const {
    if (pinIndex < 0 || pinIndex >= LEVEL_COUNT) {
        return false;
    }
    const uint8_t pin = LEVEL_PCF_PINS[pinIndex];
    const uint8_t raw = pcf.read(pin);
#if defined(LEVEL_NPN_ACTIVE_LOW) && (LEVEL_NPN_ACTIVE_LOW)
    return raw == LOW;
#else
    return raw == HIGH;
#endif
}

bool DiscreteLevelBank::poll(PCF8574& pcf, bool pcfOk) {
    if (!pcfOk) {
        available = false;
        return false;
    }

    available = true;
    const unsigned long now = millis();

    for (int i = 0; i < LEVEL_COUNT; i++) {
        const bool reading = readPinWet(pcf, i);
        if (reading != rawWet[i]) {
            rawWet[i] = reading;
            lastChangeMs[i] = now;
        }
        if (reading != stableWet[i] && (now - lastChangeMs[i]) >= LEVEL_DEBOUNCE_MS) {
            stableWet[i] = reading;
        }
        wet[i] = stableWet[i];
    }

    deriveWaterLevel();
    return true;
}

bool DiscreteLevelBank::isWet(int levelIndex) const {
    if (levelIndex < 1 || levelIndex > LEVEL_COUNT) {
        return false;
    }
    return wet[levelIndex - 1];
}

const char* DiscreteLevelBank::getWaterLevel() const {
    return waterLevelAggregate;
}

bool DiscreteLevelBank::isLevelOk() const {
    return available && strcmp(waterLevelAggregate, "vazio") != 0;
}

void DiscreteLevelBank::deriveWaterLevel() {
    const bool l1 = wet[0];
    const bool l4 = wet[3];
    const bool l3 = wet[2];

    if (!l4) {
        strncpy(waterLevelAggregate, "vazio", sizeof(waterLevelAggregate));
    } else if (l4 && !l3) {
        strncpy(waterLevelAggregate, "baixo", sizeof(waterLevelAggregate));
    } else if (l1) {
        strncpy(waterLevelAggregate, "alto", sizeof(waterLevelAggregate));
    } else {
        strncpy(waterLevelAggregate, "medio", sizeof(waterLevelAggregate));
    }
    waterLevelAggregate[sizeof(waterLevelAggregate) - 1] = '\0';
}
