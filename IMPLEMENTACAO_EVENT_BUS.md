# 🔧 Implementação: Global Event Bus

## 📋 **INTEGRAÇÃO COM SISTEMA EXISTENTE**

### **1. Integrar GlobalEventBus com AdminWebSocketServer**

```cpp
// AdminWebSocketServer.cpp - Modificar onWebSocketEvent()
void AdminWebSocketServer::onWebSocketEvent(...) {
    switch (type) {
        case WS_EVT_DATA: {
            // ... código existente ...
            
            // ✅ NOVO: Processar via GlobalEventBus
            if (globalEventBus) {
                String message = String((char*)data).substring(0, len);
                globalEventBus->processWebSocketMessage(message);
            }
            
            break;
        }
    }
}
```

---

### **2. Integrar com MasterSlaveManager**

```cpp
// MasterSlaveManager.cpp - processRelayCommandAck()
void MasterSlaveManager::processRelayCommandAck(const RelayCommandAck& ack, const uint8_t* senderMac) {
    // ... código existente ...
    
    // ✅ NOVO: Publicar evento via GlobalEventBus
    if (globalEventBus && ack.success) {
        DynamicJsonDocument eventDoc(256);
        eventDoc["slave_mac"] = ESPNowController::macToString(senderMac);
        eventDoc["relay_number"] = ack.relayNumber;
        eventDoc["state"] = (ack.currentState == 1);
        eventDoc["source"] = "ack";
        eventDoc["timestamp"] = millis();
        
        globalEventBus->publish(
            GlobalEventType::RELAY_STATE_CHANGED, 
            eventDoc.as<JsonObject>(), 
            "esp32"
        );
    }
}
```

---

### **3. Loop Principal Integrado**

```cpp
// main.cpp ou HydroSystemCore.cpp
void loop() {
    // ✅ LOOP PRIMÁRIO: Processamento sistemático
    
    // 1. Processar eventos WebSocket
    if (adminWebSocketServer) {
        adminWebSocketServer->loop();
    }
    
    // 2. Processar Event Bus
    if (globalEventBus) {
        globalEventBus->loop();
    }
    
    // 3. ✅ LOOP DE DECISÃO: Avaliar regras automaticamente
    if (decisionEngineLoop) {
        decisionEngineLoop->loop();
    }
    
    // 4. Processar comandos ESP-NOW
    if (masterManager) {
        masterManager->update();
    }
    
    // 5. Processar sensores
    hydroControl.loop();
    
    // 6. Atualizar cache do sistema
    hydroCore.loop();
    
    vTaskDelay(pdMS_TO_TICKS(10)); // 10ms
}
```

---

### **4. Inicialização**

```cpp
// main.cpp - setup()
void setup() {
    // ... código existente ...
    
    // ✅ INICIALIZAR: GlobalEventBus
    globalEventBus = new GlobalEventBus();
    globalEventBus->begin(masterManager, adminWebSocketServer->getWebSocket());
    
    // ✅ INICIALIZAR: DecisionEngineLoop
    decisionEngineLoop = new DecisionEngineLoop();
    decisionEngineLoop->begin(masterManager, globalEventBus);
    
    // ✅ EXEMPLO: Adicionar regra de decisão
    DecisionRule rule;
    rule.id = "temp_high";
    rule.condition = "temperature > 25";
    rule.slave_mac = "14:33:5C:38:BF:60";
    rule.relay_number = 0;
    rule.action = "on";
    rule.duration = 0;
    rule.enabled = true;
    rule.cooldownSeconds = 60; // 1 minuto entre execuções
    
    decisionEngineLoop->addRule(rule);
}
```

---

## 🎯 **FRONTEND - INTEGRAÇÃO**

### **1. Conectar ao WebSocket**

```typescript
// automacao/page.tsx
useEffect(() => {
  if (selectedDeviceIP) {
    // ✅ CONECTAR: Ao WebSocket do ESP32 Master
    globalEventBus.connect(selectedDeviceIP);
    
    // ✅ SUBSCRIBER: Escutar mudanças de estado
    const unsubscribe = globalEventBus.subscribe(
      EventType.RELAY_STATE_CHANGED,
      (event: RelayStateChangedEvent) => {
        const { slave_mac, relay_number, state } = event.payload;
        
        // ✅ ATUALIZAR: Estado local imediatamente
        setRelayStates(prev => {
          const newMap = new Map(prev);
          newMap.set(`${slave_mac}-${relay_number}`, state);
          return newMap;
        });
      }
    );
    
    return () => {
      unsubscribe();
      globalEventBus.disconnect();
    };
  }
}, [selectedDeviceIP]);
```

---

### **2. Usar GlobalEventBus para Comandos**

```typescript
// SimpleSlaveRelays.tsx
const handleRelayClick = async (slaveMac: string, relayId: number, currentState: boolean) => {
  const newState = !currentState;
  
  // ✅ ENVIAR: Via GlobalEventBus (WebSocket)
  globalEventBus.setRelayState(slaveMac, relayId, newState);
  
  // ✅ ESTADO OPTIMISTA: Já atualizado pelo GlobalEventBus
  // Não precisa fazer nada mais - o estado já foi atualizado
};
```

---

## 🧠 **LOOP PRIMÁRIO DE DECISÃO**

### **Exemplo de Regra**

```cpp
// Adicionar regra: Se temperatura > 25°C, ligar ventilador
DecisionRule rule;
rule.id = "ventilador_auto";
rule.condition = "temperature > 25";
rule.slave_mac = "14:33:5C:38:BF:60";  // Slave do ventilador
rule.relay_number = 2;  // Relé do ventilador
rule.action = "on";
rule.duration = 0;  // Permanente
rule.enabled = true;
rule.cooldownSeconds = 300;  // 5 minutos entre execuções

decisionEngineLoop->addRule(rule);
```

---

## ✅ **BENEFÍCIOS**

1. ✅ **Latência < 50ms** (WebSocket)
2. ✅ **Sincronização quase instantânea**
3. ✅ **Decisão automática** baseada em regras
4. ✅ **Single Source of Truth** (GlobalEventBus)
5. ✅ **Desacoplado** (componentes independentes)
6. ✅ **Escalável** (fácil adicionar novos eventos)

---

## 🚀 **PRÓXIMOS PASSOS**

1. ✅ Integrar GlobalEventBus com AdminWebSocketServer
2. ✅ Integrar com MasterSlaveManager (ACK)
3. ✅ Criar DecisionEngineLoop
4. ✅ Integrar no loop principal
5. ✅ Conectar frontend ao WebSocket
6. ✅ Testar fluxo completo



