#include "ThingSpeakManager.h"

// Definição das constantes estáticas
const unsigned long ThingSpeakManager::CHANNEL_ID = 2753377;
const char* ThingSpeakManager::WRITE_API_KEY = "3U4G0CYEC3WMCYP9";

ThingSpeakManager::ThingSpeakManager() : lastUpdate(0) {}

void ThingSpeakManager::begin() {
    ThingSpeak.begin(client);
}

void ThingSpeakManager::sendData(float temperature, float ph, float ec) {
    unsigned long currentTime = millis();
    
    // Verifica se já passou o intervalo mínimo entre atualizações
    if (currentTime - lastUpdate >= UPDATE_INTERVAL) {
        // Configura os campos
        ThingSpeak.setField(1, temperature);
        ThingSpeak.setField(2, ph);
        ThingSpeak.setField(3, ec);
        
        // Envia os dados
        int response = ThingSpeak.writeFields(CHANNEL_ID, WRITE_API_KEY);
        
        if (response == 200) {
            Serial.println("Dados enviados com sucesso para o ThingSpeak");
        } else {
            Serial.println("Erro ao enviar dados para o ThingSpeak");
        }
        
        lastUpdate = currentTime;
    }
} 