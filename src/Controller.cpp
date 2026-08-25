#include "Controller.h"
#include <cmath>

ECController::ECController() {
    // Valores zerados - removidos valores padrão
    baseDose = 0.0;       // EC base em µS/cm - removido valor padrão
    flowRate = 0.0;       // Taxa de vazão em ml/s - removido valor padrão  
    volume = 0.0;         // Volume em L - removido valor padrão
    totalMl = 0.0;        // Mililitros totais para dose base - removido valor padrão
    Kp = 1.0;             // Ganho proporcional
    kLearned = 0.0f;
}

void ECController::setParameters(float baseDose, float flowRate, float volume, float totalMl) {
    const float oldBase = this->baseDose;
    const float oldTotal = this->totalMl;
    this->baseDose = baseDose;
    this->flowRate = flowRate;
    this->volume = volume;
    this->totalMl = totalMl;
    invalidateLearnedKIfRecipeChanged(oldBase, oldTotal);
}

float ECController::recipeK() const {
    if (totalMl > 0.0f && baseDose > 0.0f) {
        return baseDose / totalMl;
    }
    return 1.0f;
}

float ECController::calculateK() const {
    if (kLearned > 1e-9f) {
        return kLearned;
    }
    return recipeK();
}

void ECController::invalidateLearnedKIfRecipeChanged(float oldBase, float oldTotal) {
    const float db = fabsf(baseDose - oldBase);
    const float dt = fabsf(totalMl - oldTotal);
    if (db > 0.5f || dt > 0.05f) {
        kLearned = 0.0f;
    }
}

void ECController::setBaseDose(float dose) {
    const float old = baseDose;
    baseDose = dose;
    invalidateLearnedKIfRecipeChanged(old, totalMl);
}

void ECController::setTotalMl(float ml) {
    const float old = totalMl;
    totalMl = ml;
    invalidateLearnedKIfRecipeChanged(baseDose, old);
}

void ECController::setLearnedK(float k) {
    kLearned = (isfinite(k) && k > 1e-9f) ? k : 0.0f;
}

bool ECController::updateGainAfterDose(float deltaEc, float mlApplied, float alpha) {
    if (mlApplied < 0.2f || deltaEc < 5.0f) {
        return false;
    }
    if (volume <= 0.01f || flowRate < 0.01f) {
        return false;
    }
    if (!isfinite(deltaEc) || !isfinite(mlApplied)) {
        return false;
    }

    // Planta G = ΔEC/ml. Lei u = V·e/(k·q) ⇒ k ≈ (V/q)·G  (q permanece na equação)
    const float kObs = (volume / flowRate) * (deltaEc / mlApplied);
    if (!isfinite(kObs) || kObs < 1e-9f) {
        return false;
    }

    float a = alpha;
    if (a < 0.05f) a = 0.05f;
    if (a > 0.5f) a = 0.5f;

    const float kPrev = calculateK();
    float kNew = a * kObs + (1.0f - a) * kPrev;
    const float kSeed = recipeK();
    const float kMin = kSeed * 0.05f;
    const float kMax = kSeed * 20.0f;
    if (kNew < kMin) kNew = kMin;
    if (kNew > kMax) kNew = kMax;

    kLearned = kNew;
    Serial.printf(
        "📈 [EC k] ΔEC=%.1f ml=%.3f G=%.4f µS/ml  k: %.4f → %.4f (obs=%.4f α=%.2f q=%.3f)\n",
        deltaEc, mlApplied, deltaEc / mlApplied, kPrev, kLearned, kObs, a, flowRate);
    return true;
}

float ECController::calculateDosage(float ecSetpoint, float ecActual) {
    // e = (ECsetpoint - ECatual)
    float error = ecSetpoint - ecActual;
    
    // k = EC base / mililitros totais
    float k = calculateK();
    
    // u(t) = (V / k * q) * e
    // Resposta em ml/s
    float dosage = 0.0;
    
    if (k > 0 && flowRate > 0) {
        dosage = (volume / (k * flowRate)) * error * Kp;
    }
    
    // Garantir que a dosagem seja positiva (só adicionar nutrientes)
    if (dosage < 0) {
        dosage = 0;
    }
    
    return dosage;
}

float ECController::calculateDosageTime(float dosageML) {
    // Tempo = Volume / Taxa de vazão
    if (flowRate > 0) {
        return dosageML / flowRate;
    }
    return 0.0;
}

bool ECController::needsAdjustment(float ecSetpoint, float ecActual, float tolerance) {
    float deficit = ecSetpoint - ecActual;
    return deficit > tolerance;
}

// ===== PH Controller =====

PHController::PHController() {}

float PHController::calculateDosageMl(float phSetpoint, float phActual, float mlPerPhUnit, float kp) {
    float error = phSetpoint - phActual;
    float ml = fabs(error) * mlPerPhUnit * kp;
    return ml > 0.05f ? ml : 0.0f;
}

float PHController::calculateDosageTime(float dosageML, float flowRateMlPerSec) {
    if (flowRateMlPerSec > 0.0f) {
        return dosageML / flowRateMlPerSec;
    }
    return 0.0f;
}

bool PHController::needsAdjustment(float phSetpoint, float phActual, float tolerance) {
    return fabs(phSetpoint - phActual) > tolerance;
}

int PHController::getDirection(float phSetpoint, float phActual, float tolerance) {
    float error = phSetpoint - phActual;
    if (fabs(error) <= tolerance) return 0;
    return error > 0 ? 1 : -1;
}

