#include "Config.h"
#include "AdaptivePHController.h"
#include "PreferencesManager.h"
#include "SensorSanitize.h"
#include <cmath>

static const float K_MIN = 1e-12f;
static const float K_MAX = 1e-2f;
#if PH_PROTOTYPE_RELAX_GUARDS
static const float MIN_DOSE_ML = 0.01f;
static const float MIN_PULSE_SEC = 0.1f;
#else
static const float MIN_DOSE_ML = 0.05f;
static const float MIN_PULSE_SEC = 0.5f;
#endif
static const float COMMISSIONING_A = 0.3f;
static const int COMMISSIONING_CYCLES = 3;

#if HIDRO_DEV_RELAX_SENSORS
static float clampPhForHDomain(float ph) {
    if (!isfinite(ph)) return NAN;
    if (ph < MIN_PH) return MIN_PH;
    if (ph > MAX_PH) return MAX_PH;
    return ph;
}
#endif

AdaptivePHController::AdaptivePHController()
    : kAcid(1e-8f), kBase(1e-8f), validLearningCycles(0) {}

float AdaptivePHController::toH(float ph) {
    if (!isPlausiblePhReading(ph)) return 0.0f;
#if HIDRO_DEV_RELAX_SENSORS
    ph = clampPhForHDomain(ph);
    if (!isfinite(ph)) return 0.0f;
#endif
    return powf(10.0f, -ph);
}

float AdaptivePHController::errorH(float phSetpoint, float phMeasured) {
#if HIDRO_DEV_RELAX_SENSORS
    if (isfinite(phMeasured) && isfinite(phSetpoint) && !isValidPhReading(phMeasured)) {
        const float hSp = toH(phSetpoint);
        const float sign = (phMeasured < phSetpoint) ? 1.0f : -1.0f;
        return sign * fabsf(hSp > 0.0f ? hSp : 1e-6f) * 0.1f;
    }
#endif
    const float err = toH(phMeasured) - toH(phSetpoint);
    return isfinite(err) ? err : 0.0f;
}

float AdaptivePHController::seedKFromMlPerPhUnit(float phSetpoint, float mlPerPhUnit) {
    if (mlPerPhUnit < 0.01f || !isPlausiblePhReading(phSetpoint)) {
        return 1e-8f;
    }
    const float H = toH(phSetpoint);
    // ErroH para ~1 unidade pH abaixo do setpoint
    const float erroHOneUnit = H * (10.0f - 1.0f);
    if (erroHOneUnit < 1e-15f) return 1e-8f;
    return clampK(erroHOneUnit / mlPerPhUnit);
}

void AdaptivePHController::setSeedFromMlPerPhUnit(float phSetpoint, float mlPerPhUnit) {
    setSeedFromMlPerPhUnit(phSetpoint, mlPerPhUnit, mlPerPhUnit);
}

void AdaptivePHController::setSeedFromMlPerPhUnit(float phSetpoint, float mlPerPhUnitAcid, float mlPerPhUnitBase) {
    kAcid = seedKFromMlPerPhUnit(phSetpoint, mlPerPhUnitAcid > 0.01f ? mlPerPhUnitAcid : 2.0f);
    kBase = seedKFromMlPerPhUnit(phSetpoint, mlPerPhUnitBase > 0.01f ? mlPerPhUnitBase : 2.0f);
    validLearningCycles = 0;
}

void AdaptivePHController::loadFromNVS() {
    float loadedAcid = kAcid;
    float loadedBase = kBase;
    if (PreferencesManager::loadConfigFloat("ph_k_acid", loadedAcid)) {
        kAcid = clampK(loadedAcid);
    }
    if (PreferencesManager::loadConfigFloat("ph_k_base", loadedBase)) {
        kBase = clampK(loadedBase);
    }
    int cycles = 0;
    int32_t loadedCycles = 0;
    if (PreferencesManager::loadConfigInt("ph_k_cycles", loadedCycles)) {
        cycles = loadedCycles >= 0 ? (int)loadedCycles : 0;
    }
    validLearningCycles = cycles;
}

void AdaptivePHController::saveToNVS() const {
    PreferencesManager::saveConfigFloat("ph_k_acid", kAcid);
    PreferencesManager::saveConfigFloat("ph_k_base", kBase);
    PreferencesManager::saveConfigInt("ph_k_cycles", validLearningCycles);
}

void AdaptivePHController::setKAcid(float k) { kAcid = clampK(k); }
void AdaptivePHController::setKBase(float k) { kBase = clampK(k); }

bool AdaptivePHController::isPlausiblePhReading(float ph) {
    return isfinite(ph);
}

bool AdaptivePHController::needsAdjustment(float phSetpoint, float phMeasured, float tolerancePh) const {
    if (!isPlausiblePhReading(phSetpoint) || !isPlausiblePhReading(phMeasured)) {
        return false;
    }
    return fabsf(phMeasured - phSetpoint) > tolerancePh;
}

PhCorrectionPath AdaptivePHController::selectPath(float phSetpoint, float phMeasured, float tolerancePh) const {
    if (!needsAdjustment(phSetpoint, phMeasured, tolerancePh)) {
        return PH_PATH_NONE;
    }
#if HIDRO_DEV_RELAX_SENSORS
    if (isfinite(phMeasured) && isfinite(phSetpoint) && !isValidPhReading(phMeasured)) {
        const float d = phMeasured - phSetpoint;
        if (d < -tolerancePh) return PH_PATH_BASE;
        if (d > tolerancePh) return PH_PATH_ACID;
        return PH_PATH_NONE;
    }
#endif
    const float err = errorH(phSetpoint, phMeasured);
    if (err > 0.0f) return PH_PATH_BASE;
    if (err < 0.0f) return PH_PATH_ACID;
    return PH_PATH_NONE;
}

float AdaptivePHController::pickK(PhCorrectionPath path) const {
    if (path == PH_PATH_BASE) return kBase > K_MIN ? kBase : K_MIN;
    if (path == PH_PATH_ACID) return kAcid > K_MIN ? kAcid : K_MIN;
    return K_MIN;
}

float AdaptivePHController::clampK(float k) {
    if (!isfinite(k) || k < K_MIN) return K_MIN;
    if (k > K_MAX) return K_MAX;
    return k;
}

float AdaptivePHController::clampAggressiveness(float a) {
    if (!isfinite(a)) return 0.5f;
    if (a < 0.05f) return 0.05f;
    if (a > 1.0f) return 1.0f;
    return a;
}

PhDosePlan AdaptivePHController::planDose(float phSetpoint, float phMeasured, float tolerancePh,
                                         float aggressiveness, float flowRateMlPerSec,
                                         float maxDoseMl, float maxPulseSec,
                                         bool commissioning) const {
    PhDosePlan plan = {};
    plan.valid = false;

    const PhCorrectionPath path = selectPath(phSetpoint, phMeasured, tolerancePh);
    if (path == PH_PATH_NONE) return plan;

    const float errH = errorH(phSetpoint, phMeasured);
    if (!isfinite(errH)) return plan;
    const float k = pickK(path);
    plan.path = path;
    plan.errorH = errH;
    plan.kUsed = k;

    plan.doseIdealMl = fabsf(errH) / k;
    float a = clampAggressiveness(aggressiveness);
    if (commissioning || validLearningCycles < COMMISSIONING_CYCLES) {
        a = fminf(a, COMMISSIONING_A);
    }
    plan.doseRealMl = a * plan.doseIdealMl;

    if (maxDoseMl > 0.0f && plan.doseRealMl > maxDoseMl) {
        plan.doseRealMl = maxDoseMl;
    }
    if (plan.doseRealMl < MIN_DOSE_ML) return plan;

    if (flowRateMlPerSec > 0.01f) {
        plan.durationSec = plan.doseRealMl / flowRateMlPerSec;
    } else {
        return plan;
    }

    if (maxPulseSec > 0 && plan.durationSec > (float)maxPulseSec) {
        plan.durationSec = (float)maxPulseSec;
        plan.doseRealMl = plan.durationSec * flowRateMlPerSec;
    }

    plan.valid = plan.durationSec >= MIN_PULSE_SEC;
    return plan;
}

bool AdaptivePHController::updateGainAfterDose(PhCorrectionPath path, float hBefore, float hAfter,
                                               float mlApplied, float alpha,
                                               float minDeltaPh) {
    if (path == PH_PATH_NONE || mlApplied < MIN_DOSE_ML) return false;
    if (hBefore <= 0.0f || hAfter <= 0.0f) return false;

    const float phBefore = -log10f(hBefore);
    const float phAfter = -log10f(hAfter);
    if (fabsf(phAfter - phBefore) < minDeltaPh) return false;

    const float deltaH = fabsf(hAfter - hBefore);
    const float kNew = deltaH / mlApplied;
    if (!isfinite(kNew) || kNew < K_MIN) return false;

    float a = alpha;
    if (a < 0.05f) a = 0.05f;
    if (a > 0.5f) a = 0.5f;

    if (path == PH_PATH_BASE) {
        kBase = clampK(a * kNew + (1.0f - a) * kBase);
    } else if (path == PH_PATH_ACID) {
        kAcid = clampK(a * kNew + (1.0f - a) * kAcid);
    } else {
        return false;
    }

    validLearningCycles++;
    saveToNVS();
    return true;
}
