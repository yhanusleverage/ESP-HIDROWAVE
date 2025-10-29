#include "StatusLED.h"

StatusLED::StatusLED(int pin) {
    ledPin = pin;
    currentPattern = LED_OFF;
    lastUpdate = 0;
    ledState = false;
    blinkCount = 0;
    enabled = true;
    brightness = 255;
}

void StatusLED::begin() {
    pinMode(ledPin, OUTPUT);
    setLED(false);
    Serial.println("💡 LED de status inicializado no pino " + String(ledPin));
}

void StatusLED::update() {
    if (!enabled) return;
    
    updatePattern();
}

void StatusLED::setPattern(LEDPattern pattern) {
    currentPattern = pattern;
    lastUpdate = millis();
    blinkCount = 0;
}

void StatusLED::setConnecting() {
    setPattern(LED_SLOW_BLINK);
}

void StatusLED::setConnected() {
    setPattern(LED_PULSE);
}

void StatusLED::setError() {
    setPattern(LED_FAST_BLINK);
}

void StatusLED::setSendingData() {
    setPattern(LED_DOUBLE_BLINK);
}

void StatusLED::setConfigMode() {
    setPattern(LED_FAST_BLINK);
}

void StatusLED::setOff() {
    setPattern(LED_OFF);
}

void StatusLED::setBrightness(int brightness) {
    this->brightness = constrain(brightness, 0, 255);
}

void StatusLED::setEnabled(bool enabled) {
    this->enabled = enabled;
    if (!enabled) {
        setLED(false);
    }
}

void StatusLED::updatePattern() {
    unsigned long currentTime = millis();
    
    switch (currentPattern) {
        case LED_OFF:
            setLED(false);
            break;
            
        case LED_ON:
            setLED(true);
            break;
            
        case LED_SLOW_BLINK: // 1 Hz - 500ms on, 500ms off
            if (currentTime - lastUpdate >= 500) {
                ledState = !ledState;
                setLED(ledState);
                lastUpdate = currentTime;
            }
            break;
            
        case LED_FAST_BLINK: // 5 Hz - 100ms on, 100ms off
            if (currentTime - lastUpdate >= 100) {
                ledState = !ledState;
                setLED(ledState);
                lastUpdate = currentTime;
            }
            break;
            
        case LED_PULSE: // Efeito respiração
            {
                float phase = (currentTime % 2000) / 2000.0; // Ciclo de 2 segundos
                float intensity = (sin(phase * 2 * PI) + 1) / 2; // 0 a 1
                int pwmValue = intensity * brightness;
                setLEDPWM(pwmValue);
            }
            break;
            
        case LED_DOUBLE_BLINK: // Duplo piscar a cada segundo
            {
                unsigned long cycleTime = currentTime % 1000;
                
                if (cycleTime < 100) {
                    setLED(true);
                } else if (cycleTime < 200) {
                    setLED(false);
                } else if (cycleTime < 300) {
                    setLED(true);
                } else {
                    setLED(false);
                }
            }
            break;
    }
}

void StatusLED::setLED(bool state) {
    ledState = state;
    if (state) {
        digitalWrite(ledPin, HIGH);
    } else {
        digitalWrite(ledPin, LOW);
    }
}

void StatusLED::setLEDPWM(int value) {
    // Usar PWM para controle de brilho
    analogWrite(ledPin, value);
} 