# 🧠 Filosofia de Controle: Sistema Proporcional Adaptativo com Bomba Peristáltica

> **Canónico 2026-08:** las leyes de Auto EC/pH están en [`docs/engineering/EQUACOES_AUTO_EC_PH.md`](docs/engineering/EQUACOES_AUTO_EC_PH.md).  
> Este archivo es histórico: **no** uses `u = V/(k·q)` ni `max_pulse` / `flow_rate` global.

---


## 📚 **1. Memória Heap = RAM? Sim, mas com nuances...**

### **✅ Resposta Direta:**
- **Heap = RAM dinâmica** (não toda a RAM, mas a parte gerenciada dinamicamente)
- **Stack = RAM estática** (para variáveis locais e chamadas de função)

### **🔍 Uso de Memória com Tasks, Mutexes e Queues:**

**SIM, usamos mais heap quando criamos tasks/mutexes/queues:**

```cpp
// ✅ CUSTO DE MEMÓRIA (aproximado):

// 1. TASK
xTaskCreatePinnedToCore(..., 8192, ...);  // 8KB de stack (heap)
// Total: ~8KB por task

// 2. MUTEX
xSemaphoreCreateMutex();  // ~40 bytes (heap)
// Total: ~40 bytes por mutex

// 3. QUEUE
xQueueCreate(10, sizeof(ControllerCommand));  // 10 * sizeof(ControllerCommand) + overhead
// Total: ~(10 * 64 bytes) + 100 bytes = ~740 bytes por queue

// ✅ EXEMPLO REAL (Controller):
// - 1 Task (8KB)
// - 4 Mutexes (160 bytes)
// - 2 Queues (1.5KB)
// TOTAL: ~9.7KB de heap
```

### **⚠️ Mas atenção:**
- **Stack de tasks** é alocado na criação (não cresce depois)
- **Queues** têm tamanho fixo (não cresce dinamicamente)
- **Mutexes** são pequenos (40 bytes cada)

**Conclusão:** Sim, usamos mais heap, mas é **previsível e controlado**. O ESP32 tem ~300KB de heap, então 10KB para o Controller é aceitável.

---

## 🎯 **2. Sistema de Prioridades - Sua Proposta é PERFEITA!**

### **✅ Prioridade Baixa/Média (20-50):**
- **Relés on_permanent**: Bomba de recirculação, aeração
- **Timers longos**: Ciclos de 30min, 1h, etc.
- **Características**: Podem ser interrompidos, não críticos

### **✅ Prioridade Máxima (80-100):**
- **Doser (dosagem milimétrica)**: Não pode ser interrompido
- **Controle proporcional adaptativo**: Deve iniciar e terminar completamente
- **Características**: Críticos, precisão temporal, não podem ser interrompidos

### **💡 Minha Sugestão de Implementação:**

```cpp
enum PriorityLevel {
    LOW = 20,           // Bomba recirculação, timers longos
    MEDIUM = 50,        // Operações normais
    HIGH = 80,          // Dosagem precisa (mas pode esperar na queue)
    CRITICAL = 100      // Dosagem milimétrica (bypass de queue)
};

// ✅ REGRA: Comandos CRITICAL não vão para queue, executam direto
// ✅ REGRA: Comandos HIGH vão para queue prioritária
// ✅ REGRA: Comandos LOW/MEDIUM vão para queue normal
```

---

## 🔬 **3. Controle Proporcional Adaptativo com Bomba Peristáltica**

### **🎯 Conceito Base: Dosagem por Pulsos**

A bomba peristáltica funciona por **pulsos de tempo** (PWM). Cada pulso injeta uma quantidade fixa de líquido.

### **📐 Parâmetros Fundamentais:**

#### **A. Volume por Pulso (ml/pulso)**
```cpp
struct PeristalticPumpConfig {
    float ml_per_pulse;        // Volume exato por pulso (ex: 0.1 ml/pulso)
    float flow_rate_ml_per_s;  // Taxa de vazão (ml/s)
    float pulse_duration_ms;   // Duração de cada pulso (ms)
    float rest_duration_ms;    // Tempo de descanso entre pulsos (ms)
    float homogenization_time_ms; // Tempo de homogeneização após dosagem (ms)
};
```

#### **B. Cálculo de Pulsos Necessários**
```cpp
int calculatePulses(float target_ml, float ml_per_pulse) {
    return (int)ceil(target_ml / ml_per_pulse);  // Arredondar para cima
}

// Exemplo: 2.5ml com 0.1ml/pulso = 25 pulsos
```

#### **C. Tempo Total de Dosagem**
```cpp
float calculateTotalTime(float target_ml, PeristalticPumpConfig& config) {
    int pulses = calculatePulses(target_ml, config.ml_per_pulse);
    
    float dosing_time = pulses * (config.pulse_duration_ms + config.rest_duration_ms);
    float total_time = dosing_time + config.homogenization_time_ms;
    
    return total_time;  // ms
}

// Exemplo:
// - 25 pulsos
// - 100ms pulso + 50ms descanso = 150ms por pulso
// - 25 * 150ms = 3750ms (3.75s de dosagem)
// - + 2000ms homogeneização = 5750ms total
```

---

## 🧪 **4. Sistema de Controle Proporcional Adaptativo**

### **🎯 Conceito:**
O sistema ajusta **automaticamente** os parâmetros baseado em:
- **Performance histórica** (erro acumulado)
- **Condições do sistema** (temperatura, pH, EC)
- **Feedback do sensor** (EC real vs EC esperado)

### **📊 Estrutura de Controle:**

```cpp
class AdaptiveProportionalController {
private:
    // ✅ PARÂMETROS ADAPTATIVOS
    float kp;              // Ganho proporcional (ajustado automaticamente)
    float ki;              // Ganho integral (ajustado automaticamente)
    float kd;              // Ganho derivativo (ajustado automaticamente)
    
    // ✅ ESTADO DO CONTROLE
    float setpoint;        // EC desejado (uS/cm)
    float current_value;  // EC atual (uS/cm)
    float error;          // Erro atual (setpoint - current_value)
    float integral_error; // Erro integral acumulado
    float last_error;     // Erro anterior (para derivativo)
    
    // ✅ HISTÓRICO DE PERFORMANCE
    struct PerformanceHistory {
        float error_sum;           // Soma de erros
        float error_squared_sum;   // Soma de erros ao quadrado
        int sample_count;          // Número de amostras
        float avg_error;           // Erro médio
        float std_deviation;       // Desvio padrão
    } history;
    
    // ✅ CONFIGURAÇÃO DA BOMBA
    PeristalticPumpConfig pump_config;
    
public:
    // ✅ MÉTODO PRINCIPAL: Calcular dosagem necessária
    PrecisionDoseCommand calculateDose() {
        // 1. Calcular erro
        error = setpoint - current_value;
        
        // 2. Atualizar histórico
        updatePerformanceHistory(error);
        
        // 3. Ajustar ganhos automaticamente (auto-tuning)
        adaptGains();
        
        // 4. Calcular correção proporcional
        float proportional_term = kp * error;
        
        // 5. Calcular correção integral
        integral_error += error;
        float integral_term = ki * integral_error;
        
        // 6. Calcular correção derivativa
        float derivative_term = kd * (error - last_error);
        last_error = error;
        
        // 7. Calcular dosagem total
        float correction_ml = proportional_term + integral_term + derivative_term;
        
        // 8. Limitar dosagem (safety)
        correction_ml = constrain(correction_ml, 0.0, MAX_DOSE_ML);
        
        // 9. Criar comando de dosagem
        PrecisionDoseCommand dose;
        dose.target_ml = correction_ml;
        dose.actuator_id = getActuatorForNutrient();
        dose.priority = 100;  // CRITICAL - não pode ser interrompido
        dose.flow_rate_ml_per_s = pump_config.flow_rate_ml_per_s;
        
        return dose;
    }
    
    // ✅ AUTO-TUNING: Ajustar ganhos automaticamente
    void adaptGains() {
        // Se erro médio é alto, aumentar Kp
        if (history.avg_error > 10.0) {
            kp *= 1.1;  // Aumentar 10%
        } else if (history.avg_error < 2.0) {
            kp *= 0.95;  // Diminuir 5% (mais estável)
        }
        
        // Se oscilação (desvio padrão alto), diminuir Ki
        if (history.std_deviation > 5.0) {
            ki *= 0.9;  // Reduzir ganho integral
        }
        
        // Limitar ganhos
        kp = constrain(kp, 0.1, 10.0);
        ki = constrain(ki, 0.0, 1.0);
        kd = constrain(kd, 0.0, 0.5);
    }
    
    // ✅ Atualizar histórico de performance
    void updatePerformanceHistory(float error) {
        history.error_sum += error;
        history.error_squared_sum += error * error;
        history.sample_count++;
        
        history.avg_error = history.error_sum / history.sample_count;
        
        // Calcular desvio padrão
        float variance = (history.error_squared_sum / history.sample_count) - 
                        (history.avg_error * history.avg_error);
        history.std_deviation = sqrt(variance);
    }
};
```

---

## ⚙️ **5. Estratégia de Pulsos com Tempo de Homogeneização**

### **🎯 Conceito:**
Após cada dosagem, o sistema precisa **aguardar** para que o nutriente se **homogeneize** na solução antes de medir novamente.

### **📐 Estrutura de Execução:**

```cpp
class PeristalticDoseExecutor {
private:
    PeristalticPumpConfig pump_config;
    AdaptiveProportionalController controller;
    
public:
    // ✅ EXECUTAR DOSAGEM COM HOMOGENEIZAÇÃO
    Result executeDoseWithHomogenization(const PrecisionDoseCommand& dose) {
        // 1. Calcular número de pulsos
        int pulses = calculatePulses(dose.target_ml, pump_config.ml_per_pulse);
        
        // 2. Executar pulsos (não pode ser interrompido)
        for (int i = 0; i < pulses; i++) {
            // Pulso ON
            setActuatorHardware(dose.actuator_id, true);
            vTaskDelay(pdMS_TO_TICKS(pump_config.pulse_duration_ms));
            
            // Descanso entre pulsos
            setActuatorHardware(dose.actuator_id, false);
            vTaskDelay(pdMS_TO_TICKS(pump_config.rest_duration_ms));
        }
        
        // 3. Aguardar homogeneização (CRÍTICO: não pode ser interrompido)
        vTaskDelay(pdMS_TO_TICKS(pump_config.homogenization_time_ms));
        
        // 4. Medir EC atual (feedback)
        float new_ec = readECSensor();
        
        // 5. Atualizar controller com feedback
        controller.updateCurrentValue(new_ec);
        
        return Result::SUCCESS;
    }
    
    // ✅ EXECUTAR CICLO COMPLETO DE CONTROLE
    Result executeControlCycle() {
        // 1. Ler EC atual
        float current_ec = readECSensor();
        controller.setCurrentValue(current_ec);
        
        // 2. Calcular dosagem necessária
        PrecisionDoseCommand dose = controller.calculateDose();
        
        // 3. Se dosagem > threshold mínimo, executar
        if (dose.target_ml > 0.1) {  // Threshold mínimo: 0.1ml
            return executeDoseWithHomogenization(dose);
        }
        
        return Result::NO_ACTION_NEEDED;
    }
};
```

---

## 🔄 **6. Tempo de Descanso Entre Pulsos - Por Quê?**

### **🎯 Motivos Técnicos:**

1. **Precisão**: Evita "overdose" por inércia da bomba
2. **Vida útil**: Reduz desgaste do motor
3. **Consumo**: Economiza energia
4. **Controle**: Permite feedback entre pulsos (opcional)

### **📊 Exemplo Prático:**

```cpp
// ✅ CONFIGURAÇÃO OTIMIZADA:
PeristalticPumpConfig config;
config.ml_per_pulse = 0.1;           // 0.1ml por pulso
config.pulse_duration_ms = 100;       // 100ms ligado
config.rest_duration_ms = 50;        // 50ms descanso
config.flow_rate_ml_per_s = 0.67;    // 0.67 ml/s (0.1ml / 0.15s)
config.homogenization_time_ms = 2000; // 2s para homogeneizar

// ✅ DOSAGEM DE 2.5ML:
// - 25 pulsos necessários
// - Tempo por pulso: 100ms + 50ms = 150ms
// - Tempo de dosagem: 25 * 150ms = 3750ms (3.75s)
// - Tempo de homogeneização: 2000ms (2s)
// - TEMPO TOTAL: 5750ms (5.75s)
```

---

## 🧠 **7. Filosofia: Por Que Não Pode Ser Interrompido?**

### **💡 Analogia:**
Imagine que você está **dosando 2.5ml de nutriente**. Se interromper no meio:
- ❌ **Dosagem incompleta** → EC não atinge setpoint
- ❌ **Medição incorreta** → Controller calcula próxima dosagem errada
- ❌ **Feedback falso** → Sistema fica instável

### **✅ Solução:**
1. **Prioridade CRITICAL (100)**: Bypass de queue, execução direta
2. **Mutex de hardware**: Garante acesso exclusivo ao PCF8574
3. **Task dedicada**: Não pode ser preemptada por outras tasks
4. **Flag de "em execução"**: Outros comandos aguardam

---

## 📊 **8. Resumo: Parâmetros que Devem Ser Configuráveis**

```cpp
struct PeristalticDoseConfig {
    // ✅ PARÂMETROS FÍSICOS
    float ml_per_pulse;              // Volume por pulso (ml)
    float pulse_duration_ms;          // Duração do pulso (ms)
    float rest_duration_ms;           // Descanso entre pulsos (ms)
    
    // ✅ PARÂMETROS DE CONTROLE
    float homogenization_time_ms;     // Tempo de homogeneização (ms)
    float min_dose_ml;               // Dosagem mínima (ml)
    float max_dose_ml;               // Dosagem máxima (ml)
    float tolerance_ml;                // Tolerância aceitável (ml)
    
    // ✅ PARÂMETROS ADAPTATIVOS
    float kp_initial;                 // Ganho proporcional inicial
    float ki_initial;                 // Ganho integral inicial
    float kd_initial;                 // Ganho derivativo inicial
    float auto_tune_enabled;          // Auto-tuning habilitado
    
    // ✅ PARÂMETROS DE SEGURANÇA
    unsigned long max_duration_ms;    // Tempo máximo de dosagem (ms)
    int max_retries;                  // Tentativas máximas em caso de erro
};
```

---

## 🎯 **9. Minha Proposta de Implementação**

### **Fase 1: Configuração Básica**
1. ✅ Estrutura `PeristalticPumpConfig`
2. ✅ Cálculo de pulsos e tempo total
3. ✅ Execução de pulsos sequenciais

### **Fase 2: Controle Proporcional**
4. ✅ Controller PID básico
5. ✅ Cálculo de dosagem baseado em erro
6. ✅ Limites de segurança

### **Fase 3: Adaptação**
7. ✅ Auto-tuning de ganhos
8. ✅ Histórico de performance
9. ✅ Ajuste automático baseado em feedback

### **Fase 4: Integração**
10. ✅ Integração com Controller (prioridade CRITICAL)
11. ✅ Bypass de queue para dosagem
12. ✅ Feedback em tempo real

---

## 💭 **10. Filosofia Final: Por Que Isso é Importante?**

### **🎯 Dosagem Milimétrica = Precisão = Confiança**

Um sistema de hidroponia precisa de:
- ✅ **Precisão**: 0.1ml de erro pode afetar o EC
- ✅ **Confiabilidade**: Não pode falhar no meio da dosagem
- ✅ **Adaptabilidade**: Ajustar-se às condições do sistema
- ✅ **Rastreabilidade**: Saber exatamente quanto foi dosado

### **🧠 Pensamento:**
O **Controller** não é só um "executor de comandos". É um **sistema de controle inteligente** que:
- Aprende com o tempo (auto-tuning)
- Adapta-se às condições (feedback)
- Garante precisão (não pode ser interrompido)
- Otimiza performance (tempo de homogeneização)

---

**Versão:** 1.0  
**Data:** 2025-01-XX  
**Foco:** Filosofia de controle proporcional adaptativo com bomba peristáltica  
**Próximo passo:** Implementar no Controller com prioridade CRITICAL

