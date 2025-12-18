# 🔬 RELAÇÃO VREF E FATOR DE CALIBRAÇÃO

## 📊 RELAÇÃO VREF → EC (É PROPORCIONAL DIRETO)

### Fórmula Completa:
```
1. V_medido = ADC_reading × (VREF / 4095.0)
2. V_comp = V_medido / (1 + 0.02 × (T - 25))
3. TDS = (133.42 × V_comp³ - 255.86 × V_comp² + 857.39 × V_comp) × 0.5 × KValue
4. EC = TDS × 2
```

### Análise:
- **VREF ↑** → **V_medido ↑** → **V_comp ↑** → **TDS ↑** → **EC ↑**
- **É PROPORCIONAL DIRETO** (não inverso)

### Exemplo Prático:
```
ADC_reading = 1000
VREF = 3.3V → V_medido = 1000 × (3.3/4095) = 0.805V → TDS = 400 ppm → EC = 800 µS/cm
VREF = 3.6V → V_medido = 1000 × (3.6/4095) = 0.878V → TDS = 450 ppm → EC = 900 µS/cm
```

**Conclusão**: Quanto MAIOR o VREF, MAIOR será o EC (proporcional direto)

---

## 🎯 FATOR DE CALIBRAÇÃO COMO AJUSTE PROPORCIONAL

### ✅ SIM, você pode usar o fator de calibração como ajuste proporcional ao erro!

### Fórmula:
```
KValue = Valor Esperado / Valor Medido
```

### Exemplo do seu caso:
- **Esperado**: 120 EC (60 PPM)
- **Medido**: 40 EC (20 PPM)
- **Erro**: 120 - 40 = 80 EC (ou 60 - 20 = 40 PPM)

**Cálculo do KValue:**
```
KValue = 120 / 40 = 3.0
ou
KValue = 60 / 20 = 3.0
```

### Aplicação:
Depois de aplicar KValue = 3.0:
- Sensor mede: 40 EC
- Com KValue: 40 × 3.0 = **120 EC** ✅

---

## 🔧 COMO AJUSTAR

### Opção 1: Ajustar VREF (se o problema for tensão de referência)
```
Se medindo 40 EC mas deveria ser 120 EC:
- Fator necessário: 120/40 = 3.0
- VREF atual: 3.3V
- Novo VREF: 3.3 × 3.0 = 9.9V ❌ (impossível, ESP32 max = 3.3V)
```

**Conclusão**: VREF não resolve se o erro é muito grande (3x)

### Opção 2: Ajustar KValue (RECOMENDADO)
```
KValue = Valor Esperado / Valor Medido
KValue = 120 / 40 = 3.0
```

**Aplicar:**
```cpp
tdsSensor->setCalibrationFactor(3.0);
```

---

## 📐 RELAÇÃO MATEMÁTICA

### Se o erro é proporcional:
```
Valor_Correto = Valor_Medido × KValue
KValue = Valor_Correto / Valor_Medido
```

### Se o erro é fixo (offset):
```
Valor_Correto = Valor_Medido + Offset
Offset = Valor_Correto - Valor_Medido
```

**No seu caso (40 → 120)**: É erro proporcional, então KValue funciona perfeitamente!

---

## ⚠️ IMPORTANTE

### VREF deve ser a tensão REAL do circuito:
- ESP32: 3.3V (padrão)
- Se medir com multímetro e for diferente (ex: 3.28V), ajuste VREF

### KValue ajusta a proporção:
- Use quando o erro for proporcional
- Não use se o erro for offset fixo

### Calibração ideal:
1. Medir VREF real com multímetro
2. Ajustar VREF no código
3. Calibrar com solução padrão 1413 µS/cm
4. Calcular KValue automaticamente

---

## 🎯 RECOMENDAÇÃO PARA SEU CASO

### Passo 1: Verificar VREF real
```cpp
// Medir com multímetro no pino 3.3V do ESP32
// Se for 3.28V, ajustar:
tdsSensor->setVRef(3.28);
```

### Passo 2: Aplicar KValue proporcional
```cpp
// Se medindo 40 EC mas deveria ser 120 EC:
float kValue = 120.0 / 40.0;  // = 3.0
tdsSensor->setCalibrationFactor(kValue);
```

### Passo 3: Salvar no NVS
```cpp
// Já implementado - salva automaticamente
saveTDSCalibration();
```

---

## 📊 TABELA DE RELAÇÃO

| VREF | ADC=1000 | TDS (K=1.0) | EC |
|------|---------|-------------|-----|
| 3.0V | 0.732V  | 350 ppm     | 700 µS/cm |
| 3.3V | 0.805V  | 400 ppm     | 800 µS/cm |
| 3.6V | 0.878V  | 450 ppm     | 900 µS/cm |

| KValue | TDS Medido | TDS Corrigido | EC Corrigido |
|--------|------------|---------------|--------------|
| 1.0    | 40 ppm     | 40 ppm        | 80 µS/cm     |
| 2.0    | 40 ppm     | 80 ppm        | 160 µS/cm    |
| 3.0    | 40 ppm     | 120 ppm       | 240 µS/cm    |

---

## ✅ CONCLUSÃO

1. **VREF ↑ = EC ↑** (proporcional direto)
2. **KValue funciona como ajuste proporcional** ✅
3. **Para seu caso (40→120)**: Use KValue = 3.0
4. **VREF só ajusta se a tensão real for diferente de 3.3V**
