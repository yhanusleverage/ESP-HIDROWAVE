#ifndef DISCRETE_LEVEL_BANK_H
#define DISCRETE_LEVEL_BANK_H

#include <Arduino.h>
#include <PCF8574.h>

/** Cuatro sondas discretas NPN vía PCF8574 (level_1 arriba → level_4 abajo). */
class DiscreteLevelBank {
public:
    static const int LEVEL_COUNT = 4;

    DiscreteLevelBank();

    void begin();
    /** Lee PCF8574 y aplica debounce. Retorna false si PCF no disponible. */
    bool poll(PCF8574& pcf, bool pcfOk);

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
    unsigned long lastChangeMs[LEVEL_COUNT];
    char waterLevelAggregate[8];

    void deriveWaterLevel();
    bool readPinWet(PCF8574& pcf, int pinIndex) const;
};

#endif
