#include "Controller.h"

ECController::ECController() {
    // Valores zerados - removidos valores padrão
    baseDose = 0.0;       // EC base em µS/cm - removido valor padrão
    flowRate = 0.0;       // Taxa de vazão em ml/s - removido valor padrão  
    volume = 0.0;         // Volume em L - removido valor padrão
    totalMl = 0.0;        // Mililitros totais para dose base - removido valor padrão
    Kp = 1.0;             // Ganho proporcional
}

void ECController::setParameters(float baseDose, float flowRate, float volume, float totalMl) {
    this->baseDose = baseDose;
    this->flowRate = flowRate;
    this->volume = volume;
    this->totalMl = totalMl;
}

float ECController::calculateK() {
    // k = EC base / mililitros totais
    if (totalMl > 0) {
        return baseDose / totalMl;
    }
    return 1.0; // Valor padrão para evitar divisão por zero
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
    float error = abs(ecSetpoint - ecActual);
    return error > tolerance;
}

