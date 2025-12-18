#ifndef THINGSPEAKMANAGER_H
#define THINGSPEAKMANAGER_H

// ThingSpeak temporariamente desabilitado
// Classe vazia para evitar erros de compilação

class ThingSpeakManager {
public:
    ThingSpeakManager() {}
    void begin() {}
    void sendData(float temperature, float ph, float tds, float ec, bool waterLevelOk = true) {}
};

#endif 