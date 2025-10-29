#include <Arduino.h>
#include "HydroControl.h"
#include <WiFi.h>
#include "WebServerManager.h"

// Credenciais WiFi
const char* ssid = "Yago 2.4G";
const char* password = "fox8gqyb34";

// Objetos principais
HydroControl hydroControl;
WebServerManager webServer;

void setup() {
    Serial.begin(115200);
    
    // Inicializar o sistema hidropônico
    hydroControl.begin();
    
    // Conectar ao WiFi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Conectando ao WiFi...");
        hydroControl.showMessage("Conectando WiFi..");
    }
    
    Serial.println("Conectado ao WiFi");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    
    // Mostrar IP no LCD
    hydroControl.showMessage("IP: " + WiFi.localIP().toString());
    delay(2000);

    // Inicializar servidor web
    webServer.begin();
    webServer.setupServer(
        hydroControl.getTemperature(),
        hydroControl.getpH(),
        hydroControl.getTDS(),
        hydroControl.getRelayStates(),
        [](int relay, int seconds) {
            hydroControl.toggleRelay(relay, seconds);
        }
    );

    Serial.println("Sistema inicializado!");
}

void loop() {
    // Atualiza sensores, display e timers dos relés
    hydroControl.update();
    
    // Delay para não sobrecarregar
    delay(1000);
}