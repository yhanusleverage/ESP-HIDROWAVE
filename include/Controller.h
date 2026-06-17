#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <Arduino.h>

class ECController {
public:
    ECController();
    
    // Configuração dos parâmetros
    void setParameters(float baseDose, float flowRate, float volume, float totalMl);
    
    // Controle proporcional
    float calculateDosage(float ecSetpoint, float ecActual);
    
    // Getters e Setters
    void setBaseDose(float dose) { baseDose = dose; }
    void setFlowRate(float rate) { flowRate = rate; }
    void setVolume(float vol) { volume = vol; }
    void setTotalMl(float ml) { totalMl = ml; }
    void setKp(float kp) { Kp = kp; }
    
    float getBaseDose() const { return baseDose; }
    float getFlowRate() const { return flowRate; }
    float getVolume() const { return volume; }
    float getTotalMl() const { return totalMl; }
    float getKp() const { return Kp; }
    
    // Função para calcular o tempo de dosagem em segundos
    float calculateDosageTime(float dosageML);
    
    // Função para verificar se precisa de ajuste
    bool needsAdjustment(float ecSetpoint, float ecActual, float tolerance = 50.0);

    float getKValue() const { return calculateK(); }

private:
    float baseDose;     // EC base em µS/cm (1525)
    float flowRate;     // Taxa de vazão peristáltica em ml/s (0.974)
    float volume;       // Volume do reservatório em L (100)
    float totalMl;      // Mililitros totais para a dose base (4.1)
    float Kp;           // Ganho proporcional (1.0)
    
    // Função para calcular k
    float calculateK() const;
};

/** Controlador proporcional bidirecional para pH (pH+ / pH-). */
class PHController {
public:
    PHController();

    float calculateDosageMl(float phSetpoint, float phActual, float mlPerPhUnit, float kp = 1.0f);
    float calculateDosageTime(float dosageML, float flowRateMlPerSec);
    bool needsAdjustment(float phSetpoint, float phActual, float tolerance = 0.2f);
    /** 1 = subir pH (pH+), -1 = baixar (pH-), 0 = dentro da tolerância */
    int getDirection(float phSetpoint, float phActual, float tolerance = 0.2f);
};

#endif

