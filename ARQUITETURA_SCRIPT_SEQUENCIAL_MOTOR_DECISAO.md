# 🎯 ARQUITETURA: SCRIPT SEQUENCIAL - MOTOR DE DECISÃO

## 📋 **CONCEITO**

O usuário cria uma **"função"** ou **"script"** que o ESP32 executa continuamente em loop. O usuário define a **ordem dos acontecimentos** como uma sequência de instruções.

---

## 🔄 **EXEMPLO: DRENO (DRAIN)**

### **O que o usuário quer:**

```
Drain --> Rules (função com task própria)

WHILE sensor_nivel4(vazio) != "on" DO
    relé5(valvula_motorizada) = "ON"
END WHILE

IF nivel4 == "vazio" THEN
    relé5(valvula_motorizada) = "OFF"
    RETURN
END IF
```

### **Tradução:**
- **Loop contínuo:** Enquanto o nível 4 não está vazio, mantém a válvula motorizada ligada
- **Condição de parada:** Quando nível 4 fica vazio, desliga a válvula e retorna

---

## 🏗️ **ESTRUTURA: SCRIPT SEQUENCIAL**

### **Formato JSON (Sequência de Instruções):**

```json
{
  "rule_id": "RULE_DRAIN_001",
  "rule_name": "Dreno Automático",
  "rule_type": "sequential_script",
  "enabled": true,
  "priority": 80,
  "script": {
    "instructions": [
      {
        "type": "while",
        "condition": {
          "sensor": "level_4",
          "operator": "!=",
          "value": "vazio"
        },
        "body": [
          {
            "type": "relay_action",
            "relay_number": 5,
            "action": "on",
            "target": "slave",
            "slave_mac": "14:33:5C:38:BF:60"
          }
        ],
        "delay_ms": 1000
      },
      {
        "type": "if",
        "condition": {
          "sensor": "level_4",
          "operator": "==",
          "value": "vazio"
        },
        "then": [
          {
            "type": "relay_action",
            "relay_number": 5,
            "action": "off",
            "target": "slave",
            "slave_mac": "14:33:5C:38:BF:60"
          },
          {
            "type": "return"
          }
        ]
      }
    ],
    "loop_interval_ms": 5000,
    "max_iterations": 0
  }
}
```

---

## 📐 **TIPOS DE INSTRUÇÕES SUPORTADAS**

### **1. WHILE (Loop)**

```json
{
  "type": "while",
  "condition": {
    "sensor": "level_4",
    "operator": "!=",
    "value": "vazio"
  },
  "body": [
    {
      "type": "relay_action",
      "relay_number": 5,
      "action": "on"
    }
  ],
  "delay_ms": 1000,
  "max_iterations": 100
}
```

**Comportamento:**
- Executa `body` enquanto `condition` for verdadeira
- `delay_ms`: Pausa entre iterações
- `max_iterations`: Limite de iterações (0 = infinito)

---

### **2. IF/ELSE (Condicional)**

```json
{
  "type": "if",
  "condition": {
    "sensor": "level_4",
    "operator": "==",
    "value": "vazio"
  },
  "then": [
    {
      "type": "relay_action",
      "relay_number": 5,
      "action": "off"
    }
  ],
  "else": [
    {
      "type": "relay_action",
      "relay_number": 5,
      "action": "on"
    }
  ]
}
```

---

### **3. RELAY_ACTION (Ação de Relé)**

```json
{
  "type": "relay_action",
  "relay_number": 5,
  "action": "on",
  "target": "slave",
  "slave_mac": "14:33:5C:38:BF:60",
  "duration_seconds": 30
}
```

**Ações possíveis:**
- `"on"`: Liga relé
- `"off"`: Desliga relé
- `"toggle"`: Alterna estado

---

### **4. DELAY (Pausa)**

```json
{
  "type": "delay",
  "duration_ms": 2000
}
```

---

### **5. RETURN (Retornar)**

```json
{
  "type": "return"
}
```

**Comportamento:**
- Sai do script atual
- Retorna ao início do loop (se houver)

---

### **6. BREAK (Sair do Loop)**

```json
{
  "type": "break"
}
```

**Comportamento:**
- Sai do loop `while` atual
- Continua com próxima instrução

---

### **7. CONTINUE (Próxima Iteração)**

```json
{
  "type": "continue"
}
```

**Comportamento:**
- Pula para próxima iteração do loop `while`

---

## 🔧 **IMPLEMENTAÇÃO NO ESP32**

### **Estrutura C++:**

```cpp
// DecisionEngine.h
enum InstructionType {
    INSTR_WHILE,
    INSTR_IF,
    INSTR_RELAY_ACTION,
    INSTR_DELAY,
    INSTR_RETURN,
    INSTR_BREAK,
    INSTR_CONTINUE
};

struct Condition {
    String sensor;
    String operator;  // "==", "!=", "<", ">", "<=", ">="
    String value;      // Valor de comparação (string ou número)
};

struct Instruction {
    InstructionType type;
    Condition condition;           // Para WHILE e IF
    std::vector<Instruction> body; // Para WHILE e IF (then/else)
    std::vector<Instruction> else_body; // Para IF
    int relay_number;
    String action;                // "on", "off", "toggle"
    String target;                 // "master", "slave"
    String slave_mac;
    int duration_seconds;
    unsigned long delay_ms;
    int max_iterations;
};

struct SequentialScript {
    String rule_id;
    String rule_name;
    std::vector<Instruction> instructions;
    unsigned long loop_interval_ms;
    int max_iterations;
    unsigned long last_execution;
};

class DecisionEngine {
private:
    std::vector<SequentialScript> activeScripts;
    
public:
    bool loadScriptsFromView();
    void executeScripts();
    bool evaluateCondition(const Condition& cond, const SystemState& state);
    void executeInstruction(const Instruction& instr, SystemState& state);
    void executeInstructionSequence(const std::vector<Instruction>& seq, SystemState& state);
};
```

### **Implementação:**

```cpp
// DecisionEngine.cpp
void DecisionEngine::executeScripts() {
    unsigned long now = millis();
    
    for (auto& script : activeScripts) {
        // Verificar intervalo do loop
        if (now - script.last_execution < script.loop_interval_ms) {
            continue;
        }
        
        script.last_execution = now;
        
        // Executar sequência de instruções
        SystemState state = getCurrentSystemState();
        executeInstructionSequence(script.instructions, state);
    }
}

void DecisionEngine::executeInstructionSequence(
    const std::vector<Instruction>& seq, 
    SystemState& state
) {
    for (const auto& instr : seq) {
        switch (instr.type) {
            case INSTR_WHILE: {
                int iterations = 0;
                while (evaluateCondition(instr.condition, state)) {
                    if (instr.max_iterations > 0 && iterations >= instr.max_iterations) {
                        break; // Limite de iterações atingido
                    }
                    
                    executeInstructionSequence(instr.body, state);
                    
                    if (instr.delay_ms > 0) {
                        vTaskDelay(pdMS_TO_TICKS(instr.delay_ms));
                    }
                    
                    state = getCurrentSystemState(); // Atualizar estado
                    iterations++;
                }
                break;
            }
            
            case INSTR_IF: {
                if (evaluateCondition(instr.condition, state)) {
                    executeInstructionSequence(instr.body, state);
                } else if (!instr.else_body.empty()) {
                    executeInstructionSequence(instr.else_body, state);
                }
                break;
            }
            
            case INSTR_RELAY_ACTION: {
                if (instr.target == "slave") {
                    // Enviar comando para slave via ESP-NOW
                    sendRelayCommandToSlave(
                        instr.slave_mac,
                        instr.relay_number,
                        instr.action,
                        instr.duration_seconds
                    );
                } else {
                    // Ação no master
                    setMasterRelay(instr.relay_number, instr.action == "on");
                }
                break;
            }
            
            case INSTR_DELAY: {
                vTaskDelay(pdMS_TO_TICKS(instr.delay_ms));
                break;
            }
            
            case INSTR_RETURN: {
                return; // Sai da função atual
            }
            
            case INSTR_BREAK: {
                // Implementar break (sair do loop while mais próximo)
                // Requer stack de loops
                break;
            }
            
            case INSTR_CONTINUE: {
                // Implementar continue (próxima iteração do while)
                // Requer stack de loops
                break;
            }
        }
    }
}

bool DecisionEngine::evaluateCondition(const Condition& cond, const SystemState& state) {
    String sensor_value = getSensorValueAsString(cond.sensor, state);
    
    if (cond.operator == "==") {
        return (sensor_value == cond.value);
    } else if (cond.operator == "!=") {
        return (sensor_value != cond.value);
    } else if (cond.operator == "<") {
        return (getSensorValueAsFloat(cond.sensor, state) < cond.value.toFloat());
    } else if (cond.operator == ">") {
        return (getSensorValueAsFloat(cond.sensor, state) > cond.value.toFloat());
    } else if (cond.operator == "<=") {
        return (getSensorValueAsFloat(cond.sensor, state) <= cond.value.toFloat());
    } else if (cond.operator == ">=") {
        return (getSensorValueAsFloat(cond.sensor, state) >= cond.value.toFloat());
    }
    
    return false;
}
```

---

## 🎨 **UI: EDITOR DE SCRIPT SEQUENCIAL**

### **Componente React:**

```tsx
// components/SequentialScriptEditor.tsx
'use client';

import { useState } from 'react';
import { Button } from '@/components/ui/button';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { Input } from '@/components/ui/input';
import { Plus, Trash2, ArrowRight } from 'lucide-react';

export function SequentialScriptEditor({ script, onChange }) {
  const [instructions, setInstructions] = useState(script?.instructions || []);

  const addInstruction = (type) => {
    const newInstr = {
      type,
      condition: { sensor: 'level_4', operator: '!=', value: 'vazio' },
      body: [],
      relay_number: 5,
      action: 'on',
      delay_ms: 1000
    };
    setInstructions([...instructions, newInstr]);
  };

  const removeInstruction = (index) => {
    setInstructions(instructions.filter((_, i) => i !== index));
  };

  return (
    <div className="space-y-4">
      <div className="flex gap-2 mb-4">
        <Button onClick={() => addInstruction('while')} variant="outline" size="sm">
          <Plus className="w-4 h-4 mr-2" />
          WHILE
        </Button>
        <Button onClick={() => addInstruction('if')} variant="outline" size="sm">
          <Plus className="w-4 h-4 mr-2" />
          IF
        </Button>
        <Button onClick={() => addInstruction('relay_action')} variant="outline" size="sm">
          <Plus className="w-4 h-4 mr-2" />
          RELAY
        </Button>
        <Button onClick={() => addInstruction('delay')} variant="outline" size="sm">
          <Plus className="w-4 h-4 mr-2" />
          DELAY
        </Button>
      </div>

      <div className="space-y-2">
        {instructions.map((instr, index) => (
          <div key={index} className="border rounded-lg p-4">
            <div className="flex items-center justify-between mb-2">
              <span className="font-mono text-sm font-semibold">
                {index + 1}. {instr.type.toUpperCase()}
              </span>
              <Button
                variant="ghost"
                size="sm"
                onClick={() => removeInstruction(index)}
              >
                <Trash2 className="w-4 h-4" />
              </Button>
            </div>

            {instr.type === 'while' && (
              <div className="space-y-2">
                <div className="flex gap-2">
                  <Select
                    value={instr.condition.sensor}
                    onValueChange={(v) => {
                      const newInstrs = [...instructions];
                      newInstrs[index].condition.sensor = v;
                      setInstructions(newInstrs);
                    }}
                  >
                    <SelectTrigger className="w-32">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="level_4">Nível 4</SelectItem>
                      <SelectItem value="ph">pH</SelectItem>
                      <SelectItem value="ec">EC</SelectItem>
                    </SelectContent>
                  </Select>
                  <Select
                    value={instr.condition.operator}
                    onValueChange={(v) => {
                      const newInstrs = [...instructions];
                      newInstrs[index].condition.operator = v;
                      setInstructions(newInstrs);
                    }}
                  >
                    <SelectTrigger className="w-20">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="==">==</SelectItem>
                      <SelectItem value="!=">!=</SelectItem>
                      <SelectItem value="<">&lt;</SelectItem>
                      <SelectItem value=">">&gt;</SelectItem>
                    </SelectContent>
                  </Select>
                  <Input
                    value={instr.condition.value}
                    onChange={(e) => {
                      const newInstrs = [...instructions];
                      newInstrs[index].condition.value = e.target.value;
                      setInstructions(newInstrs);
                    }}
                    className="w-32"
                  />
                </div>
                <div className="text-xs text-gray-500 ml-4">
                  <ArrowRight className="w-3 h-3 inline mr-1" />
                  Loop: delay {instr.delay_ms}ms
                </div>
              </div>
            )}

            {instr.type === 'relay_action' && (
              <div className="flex gap-2">
                <Input
                  type="number"
                  value={instr.relay_number}
                  onChange={(e) => {
                    const newInstrs = [...instructions];
                    newInstrs[index].relay_number = parseInt(e.target.value);
                    setInstructions(newInstrs);
                  }}
                  className="w-20"
                  placeholder="Relé"
                />
                <Select
                  value={instr.action}
                  onValueChange={(v) => {
                    const newInstrs = [...instructions];
                    newInstrs[index].action = v;
                    setInstructions(newInstrs);
                  }}
                >
                  <SelectTrigger className="w-24">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="on">ON</SelectItem>
                    <SelectItem value="off">OFF</SelectItem>
                    <SelectItem value="toggle">TOGGLE</SelectItem>
                  </SelectContent>
                </Select>
              </div>
            )}
          </div>
        ))}
      </div>
    </div>
  );
}
```

---

## ✅ **VANTAGENS DESTA ABORDAGEM**

1. ✅ **Ordem Definida pelo Usuário:** O usuário controla a sequência exata
2. ✅ **Loops:** Suporte a `WHILE` para tarefas contínuas
3. ✅ **Condicionais:** `IF/ELSE` para lógica
4. ✅ **Simples:** Estrutura JSON clara e legível
5. ✅ **Flexível:** Pode combinar múltiplas instruções

---

## 🎯 **PRÓXIMOS PASSOS**

1. ✅ Atualizar SQL para validar `sequential_script`
2. ✅ Implementar parser de instruções no ESP32
3. ✅ Criar editor visual no frontend
4. ✅ Testar com exemplo de dreno

---

**Estado:** 📋 **ARQUITETURA DEFINIDA** - Pronto para implementação


