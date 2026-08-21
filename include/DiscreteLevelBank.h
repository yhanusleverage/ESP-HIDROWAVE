#ifndef DISCRETE_LEVEL_BANK_H
#define DISCRETE_LEVEL_BANK_H

#include <Arduino.h>
#include <PCF8574.h>

/** Cuatro sondas NPN vía PCF8574 — V2: level_1 base/vazio → level_4 topo/alto (ver LEVEL_LOGIC_VERSIONS.md). */
class DiscreteLevelBank {
public:
    static const int LEVEL_COUNT = 4;

    DiscreteLevelBank();

    void begin();
    /** Lee PCF8574 y aplica debounce. Retorna false si PCF no disponible. */
    bool poll(PCF8574& pcf, bool pcfOk);
    /** Dump una vez: [LEVEL-RAW] P0=H/L … (HIGH/LOW crudo del PCF). */
    void dumpRawPins(PCF8574& pcf) const;

    bool isAvailable() const { return available; }
    /** levelIndex: 1..4 */
    bool isWet(int levelIndex) const;
    const char* getWaterLevel() const;
    bool isLevelOk() const;

private:
    bool available;
    bool wet[LEVEL_COUNT];
    bool stableWet[LEVEL_COUNT];
    bool rawWet[LEVEL_COUNT];
    bool hasStableSample;
    unsigned long lastChangeMs[LEVEL_COUNT];
    char waterLevelAggregate[16];  // "medio_alto" + NUL

    void deriveWaterLevel();
    /** false = I2C fail → no actualizar ese pin (paridad 4level_sensors). */
    bool readPinWithRetry(PCF8574& pcf, int pinIndex, bool& outWet) const;
};

#endif
