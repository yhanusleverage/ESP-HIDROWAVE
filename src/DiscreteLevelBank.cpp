#include "DiscreteLevelBank.h"
#include "Config.h"

#ifndef LEVEL_SENSOR_PCF_PINS
#define LEVEL_SENSOR_PCF_PINS 0, 1, 2, 3
#endif

static const uint8_t LEVEL_PCF_PINS[DiscreteLevelBank::LEVEL_COUNT] = { LEVEL_SENSOR_PCF_PINS };

static const char* levelLabel(int index0) {
    switch (index0) {
        case 0: return "L1";
        case 1: return "L2";
        case 2: return "L3";
        case 3: return "L4";
        default: return "L?";
    }
}

DiscreteLevelBank::DiscreteLevelBank()
    : available(false),
      hasStableSample(false) {
    memset(wet, 0, sizeof(wet));
    memset(stableWet, 0, sizeof(stableWet));
    memset(rawWet, 0, sizeof(rawWet));
    memset(lastChangeMs, 0, sizeof(lastChangeMs));
    strncpy(waterLevelAggregate, "vazio", sizeof(waterLevelAggregate));
    waterLevelAggregate[sizeof(waterLevelAggregate) - 1] = '\0';
}

void DiscreteLevelBank::begin() {
    available = false;
    hasStableSample = false;
}

bool DiscreteLevelBank::readPinWithRetry(PCF8574& pcf, int pinIndex, bool& outWet) const {
    if (pinIndex < 0 || pinIndex >= LEVEL_COUNT) {
        return false;
    }
    const uint8_t pin = LEVEL_PCF_PINS[pinIndex];
    for (int attempt = 0; attempt < 3; attempt++) {
        const uint8_t raw = pcf.read(pin);
        if (raw == LOW || raw == HIGH) {
#if defined(LEVEL_NPN_ACTIVE_LOW) && (LEVEL_NPN_ACTIVE_LOW)
            outWet = (raw == LOW);
#else
            outWet = (raw == HIGH);
#endif
            return true;
        }
        delay(1);
    }
    return false;
}

void DiscreteLevelBank::dumpRawPins(PCF8574& pcf) const {
    Serial.print("[LEVEL-RAW]");
    for (int i = 0; i < LEVEL_COUNT; i++) {
        const uint8_t pin = LEVEL_PCF_PINS[i];
        uint8_t raw = HIGH;
        for (int attempt = 0; attempt < 3; attempt++) {
            raw = pcf.read(pin);
            if (raw == LOW || raw == HIGH) {
                break;
            }
            delay(1);
        }
        Serial.printf(" P%d=%c", pin, (raw == LOW) ? 'L' : 'H');
    }
    Serial.println(" (L=LOW→MOJADO si NPN active-LOW)");
}

bool DiscreteLevelBank::poll(PCF8574& pcf, bool pcfOk) {
    if (!pcfOk) {
        available = false;
        return false;
    }

    available = true;
    const unsigned long now = millis();

    for (int i = 0; i < LEVEL_COUNT; i++) {
        bool reading = false;
        if (!readPinWithRetry(pcf, i, reading)) {
            // Paridad 4level: I2C fail → no actualizar pin (evita falso MOJADO)
            continue;
        }

        if (reading != rawWet[i]) {
            rawWet[i] = reading;
            lastChangeMs[i] = now;
        }

        if (reading != stableWet[i] && (now - lastChangeMs[i]) >= LEVEL_DEBOUNCE_MS) {
            const bool wasWet = stableWet[i];
            stableWet[i] = reading;
            if (hasStableSample) {
                Serial.printf("[WET-TEST] %s/P%u: %s -> %s\n",
                              levelLabel(i),
                              static_cast<unsigned>(LEVEL_PCF_PINS[i]),
                              wasWet ? "MOJADO" : "SECO",
                              reading ? "MOJADO" : "SECO");
            }
        }
        wet[i] = stableWet[i];
    }

    hasStableSample = true;
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
    // V2 + fracciones: 0/4 vazio … 2/4 medio … 3/4 medio_alto … 4/4 alto
    const bool l1 = wet[0];
    const bool l2 = wet[1];
    const bool l3 = wet[2];
    const bool l4 = wet[3];

    if (!l1) {
        strncpy(waterLevelAggregate, "vazio", sizeof(waterLevelAggregate));
    } else if (l1 && !l2) {
        strncpy(waterLevelAggregate, "baixo", sizeof(waterLevelAggregate));
    } else if (l4) {
        strncpy(waterLevelAggregate, "alto", sizeof(waterLevelAggregate));
    } else if (l1 && l2 && l3 && !l4) {
        strncpy(waterLevelAggregate, "medio_alto", sizeof(waterLevelAggregate));
    } else {
        // L1+L2, L3 seco, L4 seco → 2/4
        strncpy(waterLevelAggregate, "medio", sizeof(waterLevelAggregate));
    }
    waterLevelAggregate[sizeof(waterLevelAggregate) - 1] = '\0';
}
