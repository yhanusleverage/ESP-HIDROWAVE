#ifndef STATUSLED_H
#define STATUSLED_H

#include <Arduino.h>

enum LEDPattern {
    LED_OFF,
    LED_ON,
    LED_SLOW_BLINK,    // 1 Hz - Conectando WiFi
    LED_FAST_BLINK,    // 5 Hz - Erro de conexão
    LED_PULSE,         // Respiração - Funcionamento normal
    LED_DOUBLE_BLINK   // Duplo piscar - Enviando dados
};

class StatusLED {
public:
    StatusLED(int pin = 2); // GPIO2 é o LED interno do ESP32
    
    void begin();
    void update(); // Chamar no loop principal
    
    // Definir padrão do LED
    void setPattern(LEDPattern pattern);
    
    // Padrões específicos para estados do sistema
    void setConnecting();     // Piscar lento
    void setConnected();      // Respiração suave
    void setError();          // Piscar rápido
    void setSendingData();    // Duplo piscar
    void setConfigMode();     // Piscar muito rápido
    void setOff();           // Desligado
    
    // Configurações
    void setBrightness(int brightness); // 0-255 (apenas para LEDs PWM)
    void setEnabled(bool enabled);
    
private:
    int ledPin;
    LEDPattern currentPattern;
    unsigned long lastUpdate;
    bool ledState;
    int blinkCount;
    bool enabled;
    int brightness;
    
    // Funções auxiliares
    void updatePattern();
    void setLED(bool state);
    void setLEDPWM(int value); // Para efeito de respiração
};

#endif // STATUSLED_H 