#ifndef SENSOR_SANITIZE_H
#define SENSOR_SANITIZE_H

#include <Arduino.h>
#include <math.h>
#include "Config.h"

/** Devuelve value si está en [minV, maxV] y es finito; si no, NAN. */
inline float sanitizeSensorRange(float value, float minV, float maxV) {
    if (!isfinite(value) || value < minV || value > maxV) {
        return NAN;
    }
    return value;
}

inline bool isValidPhReading(float ph) {
    return isfinite(ph) && ph >= MIN_PH && ph <= MAX_PH;
}

inline bool isValidWaterTempReading(float tempC) {
    return isfinite(tempC) && tempC >= MIN_TEMP && tempC <= MAX_TEMP;
}

inline bool isValidTdsReading(float tds) {
    return isfinite(tds) && tds >= MIN_TDS && tds <= MAX_TDS;
}

/** EC en µS/cm — 0 suele ser fallo de sensor RS485, no agua real. */
inline bool isValidEcMicroSiemens(float ecUsCm) {
    return isfinite(ecUsCm) && ecUsCm > 0.0f && ecUsCm <= 10000.0f;
}

inline bool isValidEnvironmentReading(float tempC, float humidity) {
    return isfinite(tempC) && tempC >= MIN_TEMP && tempC <= MAX_TEMP &&
           isfinite(humidity) && humidity >= MIN_HUMIDITY && humidity <= MAX_HUMIDITY;
}

#endif
