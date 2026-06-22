#ifndef EC_DILUTION_CONTROLLER_H
#define EC_DILUTION_CONTROLLER_H

#include <Arduino.h>

/** Diluição EC modo A: dreno parcial + reposição (V = V_tanque × (1 − SP/EC)). */
class EcDilutionController {
public:
    bool needsDilution(float ecSetpoint, float ecActual, float tolerance) const;
    float calculateDrainVolumeLiters(float ecSetpoint, float ecActual, float tankVolumeL) const;
    float calculateOvershootUs(float ecSetpoint, float ecActual) const;
    float calculateReplaceFraction(float ecSetpoint, float ecActual) const;
};

#endif
