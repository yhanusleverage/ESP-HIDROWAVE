#include "EcDilutionController.h"
#include <math.h>

bool EcDilutionController::needsDilution(float ecSetpoint, float ecActual, float tolerance) const {
    if (!isfinite(ecSetpoint) || !isfinite(ecActual) || ecSetpoint <= 0.0f || ecActual <= 0.0f) {
        return false;
    }
    return (ecActual - ecSetpoint) > tolerance;
}

float EcDilutionController::calculateDrainVolumeLiters(float ecSetpoint, float ecActual, float tankVolumeL) const {
    if (tankVolumeL <= 0.0f || ecActual <= ecSetpoint || ecSetpoint <= 0.0f || ecActual <= 0.0f) {
        return 0.0f;
    }
    const float fraction = 1.0f - (ecSetpoint / ecActual);
    if (fraction <= 0.0f) {
        return 0.0f;
    }
    return tankVolumeL * fraction;
}

float EcDilutionController::calculateOvershootUs(float ecSetpoint, float ecActual) const {
    return ecActual - ecSetpoint;
}

float EcDilutionController::calculateReplaceFraction(float ecSetpoint, float ecActual) const {
    if (ecActual <= 0.0f || ecSetpoint <= 0.0f || ecActual <= ecSetpoint) {
        return 0.0f;
    }
    return 1.0f - (ecSetpoint / ecActual);
}
