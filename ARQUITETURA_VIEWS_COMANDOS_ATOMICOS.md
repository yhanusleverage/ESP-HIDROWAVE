# 🏗️ ARQUITETURA: VIEWS + COMANDOS ATÔMICOS + MOTOR DE DECISÃO

## 📋 **VISÃO GERAL**

Este documento define a arquitetura completa de **comandos atômicos idempotentes** usando **views** no Supabase, integrada ao **Motor de Decisão** e com interface na página de automação.

---

## 🗄️ **1. VIEWS IDEMPOTENTES (LEITURA SEM SIDE EFFECTS)**

### **View 1: `v_active_decision_rules` (Regras Ativas)**

```sql
-- ✅ VIEW: Regras ativas prontas para o ESP32
CREATE OR REPLACE VIEW v_active_decision_rules AS
SELECT 
  dr.id,
  dr.device_id,
  dr.rule_id,
  dr.rule_name,
  dr.rule_description,
  dr.rule_json,
  dr.enabled,
  dr.priority,
  dr.created_at,
  dr.updated_at,
  ds.is_online,
  ds.decision_engine_enabled,
  ds.dry_run_mode,
  ds.emergency_mode,
  ds.manual_override
FROM decision_rules dr
INNER JOIN device_status ds ON dr.device_id = ds.device_id
WHERE dr.enabled = true
  AND ds.is_online = true
  AND ds.decision_engine_enabled = true
  AND ds.emergency_mode = false
  AND ds.manual_override = false
ORDER BY dr.priority DESC, dr.created_at ASC;
```

**Uso no ESP32:**
```cpp
// GET simples - sem side effects
GET /rest/v1/v_active_decision_rules?device_id=eq.ESP32_HIDRO_F44738
```

---

### **View 2: `v_slave_status_with_commands` (Estado + Comandos Pendentes)**

```sql
-- ✅ VIEW: Estado atual do slave + comandos pendentes
CREATE OR REPLACE VIEW v_slave_status_with_commands AS
SELECT 
  rs.device_id,
  rs.slave_mac_address,
  rs.master_device_id,
  rs.master_mac_address,
  rs.relay_states,
  rs.relay_has_timers,
  rs.relay_remaining_times,
  rs.relay_names,
  rs.last_update,
  rs.updated_at,
  
  -- Status do dispositivo
  ds.is_online,
  ds.last_seen,
  ds.firmware_version,
  
  -- Comandos pendentes
  COUNT(CASE WHEN rcs.status = 'pending' THEN 1 END) as pending_commands_count,
  COUNT(CASE WHEN rcs.status = 'processing' THEN 1 END) as processing_commands_count,
  MAX(CASE WHEN rcs.status = 'pending' THEN rcs.priority END) as max_pending_priority,
  MAX(CASE WHEN rcs.status = 'pending' THEN rcs.created_at END) as oldest_pending_command,
  
  -- Comandos por tipo
  COUNT(CASE WHEN rcs.status = 'pending' AND rcs.command_type = 'manual' THEN 1 END) as pending_manual,
  COUNT(CASE WHEN rcs.status = 'pending' AND rcs.command_type = 'rule' THEN 1 END) as pending_rules,
  COUNT(CASE WHEN rcs.status = 'pending' AND rcs.command_type = 'peristaltic' THEN 1 END) as pending_peristaltic
  
FROM relay_slaves rs
LEFT JOIN device_status ds ON rs.device_id = ds.device_id
LEFT JOIN relay_commands_slave rcs ON rs.device_id = rcs.slave_device_id 
  AND rcs.status IN ('pending', 'processing')
GROUP BY 
  rs.device_id, rs.slave_mac_address, rs.master_device_id, 
  rs.master_mac_address, rs.relay_states, rs.relay_has_timers, 
  rs.relay_remaining_times, rs.relay_names, rs.last_update, 
  rs.updated_at, ds.is_online, ds.last_seen, ds.firmware_version;
```

**Uso no Frontend:**
```typescript
// GET simples - sem side effects
const { data } = await supabase
  .from('v_slave_status_with_commands')
  .select('*')
  .eq('master_device_id', masterDeviceId);
```

---

### **View 3: `v_rule_execution_history` (Histórico de Execuções)**

```sql
-- ✅ VIEW: Histórico de execuções de regras
CREATE OR REPLACE VIEW v_rule_execution_history AS
SELECT 
  re.id,
  re.device_id,
  re.rule_id,
  re.rule_name,
  re.action_type,
  re.action_details,
  re.success,
  re.error_message,
  re.execution_time_ms,
  re.timestamp,
  re.created_at,
  
  -- Dados da regra atual
  dr.rule_json,
  dr.priority as rule_priority,
  dr.enabled as rule_enabled,
  
  -- Comandos gerados
  COUNT(rcs.id) as commands_generated,
  COUNT(CASE WHEN rcs.status = 'completed' THEN 1 END) as commands_completed,
  COUNT(CASE WHEN rcs.status = 'failed' THEN 1 END) as commands_failed
  
FROM rule_executions re
LEFT JOIN decision_rules dr ON re.device_id = dr.device_id AND re.rule_id = dr.rule_id
LEFT JOIN relay_commands_slave rcs ON re.device_id = rcs.master_device_id 
  AND rcs.rule_id = re.rule_id
  AND rcs.created_at >= re.created_at
  AND rcs.created_at <= re.created_at + INTERVAL '1 minute'
GROUP BY 
  re.id, re.device_id, re.rule_id, re.rule_name, re.action_type,
  re.action_details, re.success, re.error_message, re.execution_time_ms,
  re.timestamp, re.created_at, dr.rule_json, dr.priority, dr.enabled
ORDER BY re.created_at DESC;
```

---

## 🔄 **2. ARQUITETURA DE COMANDOS ATÔMICOS**

### **Fluxo Completo com Views:**

```
┌─────────────────────────────────────────────────────────────┐
│  FRONTEND (Página Automação)                                │
│  ─────────────────────────────────────────────────────────  │
│  1. Lê v_active_decision_rules (GET - sem side effects)     │
│  2. Lê v_slave_status_with_commands (GET - sem side effects)│
│  3. Usuário cria/edita regra → INSERT/UPDATE decision_rules│
└─────────────────────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────────┐
│  SUPABASE (Triggers)                                         │
│  ─────────────────────────────────────────────────────────  │
│  ✅ Trigger após INSERT/UPDATE decision_rules:              │
│     → Valida rule_json                                      │
│     → Atualiza device_status.total_rules                    │
│  ✅ Trigger após INSERT relay_commands_slave:               │
│     → Valida arrays (relay_numbers, actions)                │
│     → Define expires_at se necessário                       │
└─────────────────────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────────┐
│  ESP32: Motor de Decisão                                     │
│  ─────────────────────────────────────────────────────────  │
│  1. GET v_active_decision_rules (a cada 30s)                │
│  2. Avalia condições (sensor < 6.0)                         │
│  3. Se condição atendida:                                   │
│     → POST relay_commands_slave (comando atômico)           │
│  4. GET get_and_lock_slave_commands (RPC - com lock)       │
│  5. Processa comandos (manual + rule + peristaltic)        │
│  6. Atualiza relay_slaves (estado atual)                    │
└─────────────────────────────────────────────────────────────┘
```

---

## 🧠 **3. INTEGRAÇÃO NO MOTOR DE DECISÃO**

### **Estrutura no ESP32:**

```cpp
// DecisionEngine.h
class DecisionEngine {
private:
    // ✅ Regras carregadas da view
    std::vector<DecisionRule> activeRules;
    
    // ✅ Estado do sistema (sensores)
    SystemState currentState;
    
    // ✅ Cache de última execução
    std::map<String, unsigned long> lastExecutionTime;
    
public:
    // ✅ Carregar regras da view
    bool loadRulesFromView();
    
    // ✅ Avaliar todas as regras
    void evaluateAllRules();
    
    // ✅ Executar ações de uma regra
    bool executeRuleActions(const DecisionRule& rule);
    
    // ✅ Criar comando atômico
    bool createAtomicCommand(const RuleAction& action, const String& ruleId);
};
```

### **Implementação:**

```cpp
// DecisionEngine.cpp
bool DecisionEngine::loadRulesFromView() {
    // ✅ GET da view (sem side effects)
    String endpoint = "v_active_decision_rules";
    String query = "?device_id=eq." + getDeviceID();
    
    // Usar makeRequest("GET", endpoint + query, "")
    // Parsear JSON array de regras
    // Popular activeRules
}

void DecisionEngine::evaluateAllRules() {
    // Para cada regra ativa:
    for (auto& rule : activeRules) {
        // 1. Verificar cooldown
        if (isInCooldown(rule)) {
            continue;
        }
        
        // 2. Avaliar condições
        if (evaluateRuleConditions(rule, currentState)) {
            // 3. Verificar segurança
            if (checkSafetyConstraints(rule, currentState)) {
                // 4. Executar ações
                executeRuleActions(rule);
                
                // 5. Atualizar cache
                lastExecutionTime[rule.rule_id] = millis();
            }
        }
    }
}

bool DecisionEngine::createAtomicCommand(const RuleAction& action, const String& ruleId) {
    // ✅ POST para relay_commands_slave (comando atômico)
    DynamicJsonDocument doc(1024);
    doc["master_device_id"] = getDeviceID();
    doc["slave_device_id"] = action.target_device_id;
    doc["slave_mac_address"] = action.target_mac;
    doc["relay_numbers"] = action.relay_numbers;  // Array
    doc["actions"] = action.actions;  // Array
    doc["duration_seconds"] = action.durations;  // Array
    doc["command_type"] = "rule";
    doc["rule_id"] = ruleId;
    doc["rule_name"] = getRuleName(ruleId);
    doc["priority"] = getRulePriority(ruleId);
    doc["status"] = "pending";
    doc["expires_at"] = "now() + interval '5 minutes'";
    
    String payload;
    serializeJson(doc, payload);
    
    return supabase.makeRequest("POST", "relay_commands_slave", payload);
}
```

---

## 🎨 **4. UI: PÁGINA AUTOMAÇÃO - MENU LATERAL**

### **Estrutura do Card "Motor de Decisão":**

```tsx
// app/automacao/page.tsx
import { useState } from 'react';
import { DecisionEngineCard } from '@/components/DecisionEngineCard';
import { RuleSetupSidebar } from '@/components/RuleSetupSidebar';

export default function AutomacaoPage() {
  const [showRuleSetup, setShowRuleSetup] = useState(false);
  const [selectedRule, setSelectedRule] = useState<string | null>(null);

  return (
    <div className="flex h-screen">
      {/* ✅ Menu Lateral (oculto por padrão) */}
      <RuleSetupSidebar
        isOpen={showRuleSetup}
        onClose={() => setShowRuleSetup(false)}
        selectedRule={selectedRule}
        onRuleCreated={(ruleId) => {
          setShowRuleSetup(false);
          // Refresh regras
        }}
      />

      {/* ✅ Conteúdo Principal */}
      <div className="flex-1 overflow-y-auto p-6">
        <h1 className="text-3xl font-bold mb-6">Automação</h1>

        {/* Card Motor de Decisão */}
        <DecisionEngineCard
          onSetupClick={() => setShowRuleSetup(true)}
          onEditRule={(ruleId) => {
            setSelectedRule(ruleId);
            setShowRuleSetup(true);
          }}
        />
      </div>
    </div>
  );
}
```

### **Componente: DecisionEngineCard**

```tsx
// components/DecisionEngineCard.tsx
'use client';

import { useState, useEffect } from 'react';
import { supabase } from '@/lib/supabase';
import { Button } from '@/components/ui/button';
import { Card, CardHeader, CardTitle, CardContent } from '@/components/ui/card';
import { Settings, Play, Pause, Plus } from 'lucide-react';

interface DecisionEngineCardProps {
  onSetupClick: () => void;
  onEditRule: (ruleId: string) => void;
}

export function DecisionEngineCard({ onSetupClick, onEditRule }: DecisionEngineCardProps) {
  const [rules, setRules] = useState<any[]>([]);
  const [isEnabled, setIsEnabled] = useState(false);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    loadRules();
  }, []);

  const loadRules = async () => {
    try {
      // ✅ GET da view (sem side effects)
      const { data, error } = await supabase
        .from('v_active_decision_rules')
        .select('*')
        .eq('device_id', 'ESP32_HIDRO_F44738')
        .order('priority', { ascending: false });

      if (error) throw error;
      setRules(data || []);
    } catch (error) {
      console.error('Erro ao carregar regras:', error);
    } finally {
      setLoading(false);
    }
  };

  return (
    <Card className="w-full">
      <CardHeader className="flex flex-row items-center justify-between">
        <CardTitle className="flex items-center gap-2">
          <Settings className="w-5 h-5" />
          Motor de Decisão
        </CardTitle>
        <div className="flex gap-2">
          <Button
            variant="outline"
            size="sm"
            onClick={onSetupClick}
            className="flex items-center gap-2"
          >
            <Plus className="w-4 h-4" />
            Nova Regra
          </Button>
          <Button
            variant={isEnabled ? "destructive" : "default"}
            size="sm"
            onClick={() => setIsEnabled(!isEnabled)}
          >
            {isEnabled ? <Pause className="w-4 h-4" /> : <Play className="w-4 h-4" />}
            {isEnabled ? 'Pausar' : 'Iniciar'}
          </Button>
        </div>
      </CardHeader>
      <CardContent>
        {loading ? (
          <p>Carregando regras...</p>
        ) : (
          <div className="space-y-2">
            <p className="text-sm text-gray-600">
              {rules.length} regra(s) ativa(s)
            </p>
            {rules.map((rule) => (
              <div
                key={rule.id}
                className="p-3 border rounded-lg cursor-pointer hover:bg-gray-50"
                onClick={() => onEditRule(rule.id)}
              >
                <div className="flex justify-between items-start">
                  <div>
                    <h4 className="font-semibold">{rule.rule_name}</h4>
                    <p className="text-sm text-gray-500">{rule.rule_description}</p>
                  </div>
                  <span className="text-xs bg-blue-100 text-blue-800 px-2 py-1 rounded">
                    Prioridade: {rule.priority}
                  </span>
                </div>
              </div>
            ))}
          </div>
        )}
      </CardContent>
    </Card>
  );
}
```

### **Componente: RuleSetupSidebar**

```tsx
// components/RuleSetupSidebar.tsx
'use client';

import { useState, useEffect } from 'react';
import { X, Save } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Textarea } from '@/components/ui/textarea';
import { supabase } from '@/lib/supabase';

interface RuleSetupSidebarProps {
  isOpen: boolean;
  onClose: () => void;
  selectedRule: string | null;
  onRuleCreated: (ruleId: string) => void;
}

export function RuleSetupSidebar({
  isOpen,
  onClose,
  selectedRule,
  onRuleCreated
}: RuleSetupSidebarProps) {
  const [ruleName, setRuleName] = useState('');
  const [ruleDescription, setRuleDescription] = useState('');
  const [ruleJson, setRuleJson] = useState('');
  const [priority, setPriority] = useState(50);
  const [enabled, setEnabled] = useState(true);

  useEffect(() => {
    if (selectedRule) {
      loadRule(selectedRule);
    } else {
      resetForm();
    }
  }, [selectedRule]);

  const loadRule = async (ruleId: string) => {
    // Carregar regra existente
    const { data, error } = await supabase
      .from('decision_rules')
      .select('*')
      .eq('id', ruleId)
      .single();

    if (data) {
      setRuleName(data.rule_name);
      setRuleDescription(data.rule_description || '');
      setRuleJson(JSON.stringify(data.rule_json, null, 2));
      setPriority(data.priority);
      setEnabled(data.enabled);
    }
  };

  const resetForm = () => {
    setRuleName('');
    setRuleDescription('');
    setRuleJson('{\n  "conditions": {},\n  "actions": []\n}');
    setPriority(50);
    setEnabled(true);
  };

  const handleSave = async () => {
    try {
      const ruleData = {
        device_id: 'ESP32_HIDRO_F44738',
        rule_id: selectedRule || `RULE_${Date.now()}`,
        rule_name: ruleName,
        rule_description: ruleDescription,
        rule_json: JSON.parse(ruleJson),
        enabled,
        priority
      };

      if (selectedRule) {
        // UPDATE
        await supabase
          .from('decision_rules')
          .update(ruleData)
          .eq('id', selectedRule);
      } else {
        // INSERT
        const { data, error } = await supabase
          .from('decision_rules')
          .insert(ruleData)
          .select()
          .single();

        if (error) throw error;
        onRuleCreated(data.id);
      }

      onClose();
    } catch (error) {
      console.error('Erro ao salvar regra:', error);
      alert('Erro ao salvar regra');
    }
  };

  if (!isOpen) return null;

  return (
    <div className="fixed inset-y-0 right-0 w-96 bg-white shadow-xl z-50 flex flex-col">
      <div className="flex items-center justify-between p-4 border-b">
        <h2 className="text-xl font-semibold">
          {selectedRule ? 'Editar Regra' : 'Nova Regra'}
        </h2>
        <Button variant="ghost" size="sm" onClick={onClose}>
          <X className="w-4 h-4" />
        </Button>
      </div>

      <div className="flex-1 overflow-y-auto p-4 space-y-4">
        <div>
          <label className="block text-sm font-medium mb-1">Nome da Regra</label>
          <Input
            value={ruleName}
            onChange={(e) => setRuleName(e.target.value)}
            placeholder="Ex: Ajustar pH quando baixo"
          />
        </div>

        <div>
          <label className="block text-sm font-medium mb-1">Descrição</label>
          <Textarea
            value={ruleDescription}
            onChange={(e) => setRuleDescription(e.target.value)}
            placeholder="Descrição opcional da regra"
            rows={3}
          />
        </div>

        <div>
          <label className="block text-sm font-medium mb-1">Prioridade (0-100)</label>
          <Input
            type="number"
            min="0"
            max="100"
            value={priority}
            onChange={(e) => setPriority(parseInt(e.target.value))}
          />
        </div>

        <div>
          <label className="block text-sm font-medium mb-1">Rule JSON</label>
          <Textarea
            value={ruleJson}
            onChange={(e) => setRuleJson(e.target.value)}
            placeholder='{"conditions": {}, "actions": []}'
            rows={10}
            className="font-mono text-sm"
          />
        </div>

        <div className="flex items-center gap-2">
          <input
            type="checkbox"
            id="enabled"
            checked={enabled}
            onChange={(e) => setEnabled(e.target.checked)}
          />
          <label htmlFor="enabled" className="text-sm">Regra ativa</label>
        </div>
      </div>

      <div className="p-4 border-t flex gap-2">
        <Button variant="outline" onClick={onClose} className="flex-1">
          Cancelar
        </Button>
        <Button onClick={handleSave} className="flex-1">
          <Save className="w-4 h-4 mr-2" />
          Salvar
        </Button>
      </div>
    </div>
  );
}
```

---

## 🔧 **5. TRIGGERS NO SUPABASE (NÃO NO ESP32)**

### **Por que Triggers no Supabase?**

✅ **ESP32 tem IP privado** - não pode receber webhooks  
✅ **Triggers no Supabase** - executam no servidor  
✅ **Views são idempotentes** - GET não causa side effects  

### **Trigger 1: Validar rule_json**

```sql
-- ✅ Trigger: Validar rule_json antes de INSERT/UPDATE
CREATE OR REPLACE FUNCTION validate_decision_rule_json()
RETURNS TRIGGER AS $$
BEGIN
  -- Validar estrutura básica
  IF NOT (NEW.rule_json ? 'conditions' AND NEW.rule_json ? 'actions') THEN
    RAISE EXCEPTION 'rule_json deve conter "conditions" e "actions"';
  END IF;
  
  -- Validar que actions é array
  IF jsonb_typeof(NEW.rule_json->'actions') != 'array' THEN
    RAISE EXCEPTION 'actions deve ser um array';
  END IF;
  
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trigger_validate_decision_rule_json
  BEFORE INSERT OR UPDATE ON decision_rules
  FOR EACH ROW
  EXECUTE FUNCTION validate_decision_rule_json();
```

### **Trigger 2: Atualizar contador de regras**

```sql
-- ✅ Trigger: Atualizar total_rules em device_status
CREATE OR REPLACE FUNCTION update_device_rules_count()
RETURNS TRIGGER AS $$
BEGIN
  UPDATE device_status
  SET total_rules = (
    SELECT COUNT(*) FROM decision_rules
    WHERE device_id = NEW.device_id AND enabled = true
  )
  WHERE device_id = NEW.device_id;
  
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trigger_update_device_rules_count
  AFTER INSERT OR UPDATE OR DELETE ON decision_rules
  FOR EACH ROW
  EXECUTE FUNCTION update_device_rules_count();
```

---

## ✅ **6. BENEFÍCIOS DESTA ARQUITETURA**

### **1. Views Idempotentes**
- ✅ **GET sem side effects** - pode chamar quantas vezes quiser
- ✅ **Frontend lê views** - não precisa fazer JOINs complexos
- ✅ **ESP32 lê views** - simplifica código

### **2. Comandos Atômicos**
- ✅ **RPC com lock** - previne processamento duplicado
- ✅ **Status tracking** - rastreabilidade completa
- ✅ **Arrays batch** - múltiplos relés por comando

### **3. Motor de Decisão Integrado**
- ✅ **Toda lógica no ESP32** - avalia condições e cria comandos
- ✅ **Views facilitam leitura** - não precisa fazer JOINs
- ✅ **Triggers no Supabase** - validação e contadores automáticos

### **4. UI Simplificada**
- ✅ **Menu lateral** - não polui a página principal
- ✅ **GET de views** - carrega rápido
- ✅ **Setup guiado** - facilita criação de regras

---

## 🎯 **PRÓXIMOS PASSOS**

1. ✅ **Criar views no Supabase** (`v_active_decision_rules`, `v_slave_status_with_commands`)
2. ✅ **Criar triggers no Supabase** (validação, contadores)
3. ✅ **Implementar `loadRulesFromView()`** no ESP32
4. ✅ **Implementar `createAtomicCommand()`** no ESP32
5. ✅ **Criar componentes React** (DecisionEngineCard, RuleSetupSidebar)
6. ✅ **Integrar na página de automação**

---

**Estado:** 📋 **ARQUITETURA COMPLETA DEFINIDA** - Pronto para implementação


