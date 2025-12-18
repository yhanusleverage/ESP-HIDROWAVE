# 🔧 Explicação: Cálculo do Fator de Calibração

## 📊 Situação Real

### Após Dosagem:
- **Sensor embarcado lê**: 3354 µS/cm
- **Medidor profissional lê**: 1040 µS/cm
- **Razão**: 3354 / 1040 = **3.23x** (sensor lê 3.23x mais alto)

---

## 🔍 Lógica de Calibração no Código

### Fórmula (TDSReaderSerial.cpp linha 192):
```cpp
float newCalibrationFactor = standardValue / measuredValue;
```

**Onde**:
- `standardValue` = Valor real (medidor profissional) = 1040 µS/cm
- `measuredValue` = Valor medido (sensor embarcado) = 3354 µS/cm

### Cálculo Correto:
```
Fator = 1040 / 3354 = 0.31
```

### Aplicação (linha 81):
```cpp
rawTDS = (fórmula) * 0.5 * _calibrationFactor;
```

**Resultado**:
- Sensor lê: 3354 µS/cm
- Com fator 0.31: 3354 × 0.31 = **1040 µS/cm** ✅

---

## ⚠️ Confusão: 3.23 vs 0.31

### 3.23 é a RAZÃO (quanto o sensor lê a mais):
```
Razão = 3354 / 1040 = 3.23x
```

### 0.31 é o FATOR DE CALIBRAÇÃO (para corrigir):
```
Fator = 1040 / 3354 = 0.31
```

**Relação**: Fator = 1 / Razão = 1 / 3.23 = 0.31 ✅

---

## 🎯 Conclusão

**O cálculo CORRETO do fator de calibração é**:
```
Fator = Valor_Real / Valor_Medido = 1040 / 3354 = 0.31
```

**NÃO é**:
```
Fator = Valor_Medido / Valor_Real = 3354 / 1040 = 3.23 ❌
```

**Razão**: O fator é aplicado como **multiplicação** no código, então precisa ser < 1.0 para reduzir valores altos.

---

## 💡 Problema: Fator 3.0 vs 0.31

**Água de canilla**:
- Sensor lê: 40 µS/cm
- Real: 150 µS/cm
- Fator usado: 3.0
- Resultado: 40 × 3.0 = 120 µS/cm ✅ (funciona)

**Após dosagem**:
- Sensor lê: 3354 µS/cm
- Real: 1040 µS/cm
- Fator necessário: 0.31
- Resultado: 3354 × 0.31 = 1040 µS/cm ✅ (correto)

**Problema**: O fator 3.0 funciona para água pura (sensor lê baixo), mas NÃO funciona para soluções concentradas (sensor lê alto).

**Solução**: Calibração multi-ponto ou usar solução padrão 1413 µS/cm.


