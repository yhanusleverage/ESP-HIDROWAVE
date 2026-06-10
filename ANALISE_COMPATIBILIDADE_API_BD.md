# 📊 Análise de Compatibilidade: Frontend ↔ ESP-HIDROWAVE ↔ Banco de Dados

## ✅ RESUMO EXECUTIVO

### Status Geral
- **APIs do ESP**: ✅ Funcionais e compatíveis com frontend
- **Banco de Dados**: ✅ Schema completo e operacional
- **Sincronização ESP ↔ BD**: ⚠️ **FALTA IMPLEMENTAR**

---

## 🔌 APIs DO EC CONTROLLER (ESP-HIDROWAVE)

### 1. GET `/api/ec-controller/config`
**Status**: ✅ Implementado

**Resposta JSON**:
```json
{
  "baseDose": 1525.0,
  "flowRate": 0.974,
  "volume": 100.0,
  "totalMl": 4.1,
  "kp": 1.0,
  "ecSetpoint": 1500.0,
  "ecActual": 1450.0,
  "autoEnabled": true
}
```

**Compatibilidade com BD**: ✅ **100% COMPATÍVEL**
- Todos os campos correspondem à tabela `ec_controller_config`

---

### 2. POST `/api/ec-controller/config`
**Status**: ✅ Implementado

**Request Body** (todos os campos são opcionais):
```json
{
  "baseDose": 1525.0,
  "flowRate": 0.974,
  "volume": 100.0,
  "totalMl": 4.1,
  "kp": 1.0,
  "ecSetpoint": 1500.0,
  "autoEnabled": true
}
```

**Compatibilidade com BD**: ✅ **100% COMPATÍVEL**
- Campos correspondem exatamente à tabela `ec_controller_config`

**⚠️ PROBLEMA IDENTIFICADO**: 
- A configuração é salva apenas no **NVS local** (memória do ESP)
- **NÃO sincroniza automaticamente com Supabase**
- Frontend precisa fazer chamada separada para salvar no BD

---

### 3. POST `/api/ec-controller/nutrient-proportions`
**Status**: ✅ Implementado

**Request Body**:
```json
{
  "nutrients": [
    {
      "name": "Grow",
      "relay": 0,
      "mlPerLiter": 2.5,
      "proportion": 0.4
    },
    {
      "name": "Micro",
      "relay": 1,
      "mlPerLiter": 1.5,
      "proportion": 0.3
    }
  ]
}
```

**Compatibilidade com BD**: ✅ **IMPLEMENTADO (código)** — ver [`HIDROWAVE-main/docs/HANDOFF_ULTIMA_DOSAGEM_E2E.md`](../../HIDROWAVE-main/docs/HANDOFF_ULTIMA_DOSAGEM_E2E.md)
- INSERT em `nutrient_dosages` após cada nutriente (`SupabaseClient::insertNutrientDosage`)
- UI lê SUM(ml) via `useLastDosage`
- **Prod:** executar SQL + flash + validar KPI

---

## 🗄️ COMPATIBILIDADE COM BANCO DE DADOS

### Tabela: `ec_controller_config`

| Campo BD | Campo API ESP | Status | Observações |
|----------|---------------|--------|-------------|
| `device_id` | ❌ Não enviado | ⚠️ **FALTA** | Precisa ser adicionado na sincronização |
| `base_dose` | `baseDose` | ✅ Compatível | Nomes diferentes (snake_case vs camelCase) |
| `flow_rate` | `flowRate` | ✅ Compatível | Nomes diferentes |
| `volume` | `volume` | ✅ Compatível | Idêntico |
| `total_ml` | `totalMl` | ✅ Compatível | Nomes diferentes |
| `kp` | `kp` | ✅ Compatível | Idêntico |
| `ec_setpoint` | `ecSetpoint` | ✅ Compatível | Nomes diferentes |
| `auto_enabled` | `autoEnabled` | ✅ Compatível | Nomes diferentes |
| `created_at` | ❌ Não enviado | ⚠️ Auto-gerado | BD gera automaticamente |
| `updated_at` | ❌ Não enviado | ⚠️ Auto-gerado | BD gera automaticamente |
| `created_by` | ❌ Não enviado | ⚠️ Default 'system' | BD usa default |

**Ação Necessária**: 
- Implementar função de sincronização que converte `camelCase` → `snake_case`
- Adicionar `device_id` ao payload

---

### Tabela: `ec_controller_metrics`

**Status**: ❌ **NÃO IMPLEMENTADO**

**Campos necessários**:
- `device_id`
- `ec_setpoint`
- `ec_actual`
- `ec_error`
- `k_value` (calculado)
- `dosage_ml` (calculado)
- `dosage_time_seconds` (calculado)
- `physical_gain`
- `ec_final_predicted`
- `base_dose`, `flow_rate`, `volume`, `total_ml`, `kp`
- `auto_enabled`
- `adjustment_needed`
- `adjustment_applied`
- `timestamp`

**Ação Necessária**: 
- Criar endpoint `/api/ec-controller/metrics` (POST)
- Enviar métricas periodicamente para Supabase
- Implementar no `HydroControl::checkAutoEC()`

---

### Tabela: `ec_controller_history`

**Status**: ❌ **NÃO IMPLEMENTADO**

**Uso**: Registrar histórico de mudanças na configuração

**Ação Necessária**:
- Criar trigger no Supabase OU
- Implementar no POST `/api/ec-controller/config` antes de atualizar

---

### Tabela: `nutrition_plans`

**Status**: ⚠️ **PARCIAL**

**Compatibilidade**:
- Campo `nutrients` (JSONB) ✅ Compatível com formato do ESP
- Campo `device_id` ⚠️ Precisa ser adicionado
- Campos `pump_flow_rate`, `total_volume`, `total_ml` ✅ Disponíveis no ESP

**Ação Necessária**:
- Sincronizar após POST `/api/ec-controller/nutrient-proportions`
- Criar/atualizar registro na tabela `nutrition_plans`

---

### Tabela: `nutrient_dosages`

**Status**: ✅ **IMPLEMENTADO (firmware + frontend)** · ⏳ **SQL prod + bancada**

**Uso**: Registrar cada dosagem executada

**Implementado**:
- [`SupabaseClient::insertNutrientDosage`](src/SupabaseClient.cpp) — HTTPS POST
- Hook em [`HydroControl::emitNutrientDoseEvent`](src/HydroControl.cpp)
- SQL: [`HIDROWAVE-main/scripts/CRIAR_TABELA_NUTRIENT_DOSAGES.sql`](../../HIDROWAVE-main/scripts/CRIAR_TABELA_NUTRIENT_DOSAGES.sql)
- UI: [`useLastDosage.ts`](../../HIDROWAVE-main/src/hooks/useLastDosage.ts)

---

## 🔄 FLUXO DE SINCRONIZAÇÃO RECOMENDADO

### 1. Configuração do EC Controller

```
Frontend → POST /api/ec-controller/config
    ↓
ESP salva no NVS local
    ↓
ESP envia para Supabase (UPSERT em ec_controller_config)
    ↓
ESP registra histórico em ec_controller_history
```

### 2. Proporções Nutricionais

```
Frontend → POST /api/ec-controller/nutrient-proportions
    ↓
ESP atualiza proporções locais
    ↓
ESP sincroniza com nutrition_plans (UPSERT)
```

### 3. Métricas do Controller

```
ESP (periodicamente) → Calcula métricas
    ↓
ESP → POST para Supabase (ec_controller_metrics)
    ↓
Frontend consulta métricas para gráficos
```

---

## ✅ CHECKLIST DE IMPLEMENTAÇÃO

### Prioridade ALTA
- [ ] Implementar sincronização POST `/api/ec-controller/config` → Supabase
- [ ] Adicionar conversão `camelCase` ↔ `snake_case`
- [ ] Incluir `device_id` em todas as requisições

### Prioridade MÉDIA
- [ ] Criar endpoint para enviar métricas (`ec_controller_metrics`)
- [ ] Implementar registro de histórico (`ec_controller_history`)
- [ ] Sincronizar proporções nutricionais com `nutrition_plans`

### Prioridade BAIXA
- [ ] Criar endpoint GET para histórico de métricas
- [ ] Implementar paginação para consultas grandes
- [x] Registrar dosagens em `nutrient_dosages` (código — ver HANDOFF)
- [ ] Validar dosagens em prod (SQL + flash + KPI)

---

## 📝 NOTAS TÉCNICAS

### Conversão de Nomes
```cpp
// ESP usa camelCase
{
  "baseDose": 1525.0,
  "flowRate": 0.974,
  "ecSetpoint": 1500.0
}

// BD usa snake_case
{
  "base_dose": 1525.0,
  "flow_rate": 0.974,
  "ec_setpoint": 1500.0
}
```

### Device ID
- Obtido via `SupabaseClient::getDeviceID()`
- Formato: MAC address sem dois pontos
- Exemplo: `AABBCCDDEEFF`

### Timestamps
- ESP usa `millis()` para timestamps locais
- BD usa `timestamp with time zone` (PostgreSQL)
- Converter: `millis() / 1000` para segundos Unix

---

## 🎯 CONCLUSÃO

### Status Atual
✅ **APIs funcionais e compatíveis com frontend**
✅ **Schema do BD completo e bem estruturado**
⚠️ **Falta implementar sincronização ESP ↔ BD**

### Próximos Passos
1. Implementar função de sincronização em `SupabaseClient`
2. Adicionar chamadas de sincronização após salvar no NVS
3. Criar endpoint para métricas periódicas
4. Testar fluxo completo Frontend → ESP → BD

---

**Data da Análise**: 2024
**Versão do Firmware**: 2.2.0
**Versão do Schema BD**: 1.0



