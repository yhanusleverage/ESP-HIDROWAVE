#ifndef ADAPTIVE_PH_CONTROLLER_H
#define ADAPTIVE_PH_CONTROLLER_H

#include <Arduino.h>

/**
 * Controlador adaptativo de pH em domínio H = 10^(-pH).
 * Convenção: ErroH = Hmedido - Hsetpoint
 *   ErroH > 0 → pH baixo → base (pH+)
 *   ErroH < 0 → pH alto  → ácido (pH-)
 * DoseIdeal = |ErroH| / K ; DoseReal = A × DoseIdeal
 */
enum PhCorrectionPath {
    PH_PATH_NONE = 0,
    PH_PATH_BASE = 1,  // subir pH
    PH_PATH_ACID = -1    // baixar pH
};

struct PhDosePlan {
    PhCorrectionPath path;
    float doseIdealMl;
    float doseRealMl;
    float durationSec;
    float kUsed;
    float errorH;
    bool valid;
};

class AdaptivePHController {
public:
    AdaptivePHController();

    static float toH(float ph);
    static float errorH(float phSetpoint, float phMeasured);
    static float seedKFromMlPerPhUnit(float phSetpoint, float mlPerPhUnit);

    void setSeedFromMlPerPhUnit(float phSetpoint, float mlPerPhUnit);
    void setSeedFromMlPerPhUnit(float phSetpoint, float mlPerPhUnitAcid, float mlPerPhUnitBase);
    void loadFromNVS();
    void saveToNVS() const;

    void setKAcid(float k);
    void setKBase(float k);
    float getKAcid() const { return kAcid; }
    float getKBase() const { return kBase; }
    int getValidLearningCycles() const { return validLearningCycles; }

    bool needsAdjustment(float phSetpoint, float phMeasured, float tolerancePh) const;
    PhCorrectionPath selectPath(float phSetpoint, float phMeasured, float tolerancePh) const;

    PhDosePlan planDose(float phSetpoint, float phMeasured, float tolerancePh,
                        float aggressiveness, float flowRateMlPerSec,
                        float maxDoseMl, float maxPulseSec,
                        bool commissioning = false) const;

    bool updateGainAfterDose(PhCorrectionPath path, float hBefore, float hAfter,
                             float mlApplied, float alpha,
                             float minDeltaPh = 0.03f);

    static bool isPlausiblePhReading(float ph);

private:
    float kAcid;
    float kBase;
    int validLearningCycles;

    float pickK(PhCorrectionPath path) const;
    static float clampK(float k);
    static float clampAggressiveness(float a);
};

#endif
