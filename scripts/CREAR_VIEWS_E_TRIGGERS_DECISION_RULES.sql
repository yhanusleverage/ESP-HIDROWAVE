-- ============================================================
-- VIEWS E TRIGGERS PARA DECISION_RULES + COMANDOS ATÔMICOS
-- ============================================================

-- ============================================================
-- 1. VIEWS IDEMPOTENTES (LEITURA SEM SIDE EFFECTS)
-- ============================================================

-- ✅ VIEW 1: Regras ativas prontas para o ESP32
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

-- ✅ VIEW 2: Estado atual do slave + comandos pendentes
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

-- ✅ VIEW 3: Histórico de execuções de regras
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

-- ============================================================
-- 2. TRIGGERS (VALIDAÇÃO E CONTADORES)
-- ============================================================

-- ✅ TRIGGER 1: Validar rule_json antes de INSERT/UPDATE
-- ✅ Suporta: Regra Mãe com N condicionais filhas (composite conditions)
CREATE OR REPLACE FUNCTION validate_decision_rule_json()
RETURNS TRIGGER AS $$
DECLARE
  cond_type text;
  child_count int;
  i int;
  child jsonb;
BEGIN
  -- ✅ NOVO: Suporta dois tipos de regras:
  -- 1. "composite" ou "simple": Regra mãe com condicionais
  -- 2. "sequential_script": Script sequencial com instruções (WHILE, IF, etc.)
  
  -- Verificar tipo de regra
  IF NEW.rule_json ? 'script' THEN
    -- ✅ TIPO: Sequential Script (script sequencial)
    IF jsonb_typeof(NEW.rule_json->'script') != 'object' THEN
      RAISE EXCEPTION 'script deve ser um objeto';
    END IF;
    
    IF NOT (NEW.rule_json->'script' ? 'instructions') THEN
      RAISE EXCEPTION 'script deve conter "instructions"';
    END IF;
    
    IF jsonb_typeof(NEW.rule_json->'script'->'instructions') != 'array' THEN
      RAISE EXCEPTION 'instructions deve ser um array';
    END IF;
    
    -- Validar cada instrução
    FOR i IN 0..jsonb_array_length(NEW.rule_json->'script'->'instructions') - 1 LOOP
      DECLARE
        instr jsonb := NEW.rule_json->'script'->'instructions'->i;
        instr_type text := instr->>'type';
      BEGIN
        -- Validar tipo de instrução
        IF NOT (instr_type = ANY(ARRAY['while', 'if', 'relay_action', 'delay', 'return', 'break', 'continue'])) THEN
          RAISE EXCEPTION 'instructions[%].type deve ser: while, if, relay_action, delay, return, break, ou continue', i;
        END IF;
        
        -- Validar instruções que requerem condition
        IF instr_type IN ('while', 'if') THEN
          IF NOT (instr ? 'condition') THEN
            RAISE EXCEPTION 'instructions[%] do tipo % deve conter "condition"', i, instr_type;
          END IF;
          
          IF NOT (instr->'condition' ? 'sensor' AND instr->'condition' ? 'operator' AND instr->'condition' ? 'value') THEN
            RAISE EXCEPTION 'instructions[%].condition deve conter: sensor, operator, value', i;
          END IF;
        END IF;
        
        -- Validar relay_action
        IF instr_type = 'relay_action' THEN
          IF NOT (instr ? 'relay_number' AND instr ? 'action') THEN
            RAISE EXCEPTION 'instructions[%] do tipo relay_action deve conter: relay_number, action', i;
          END IF;
          
          IF NOT (instr->>'action' = ANY(ARRAY['on', 'off', 'toggle'])) THEN
            RAISE EXCEPTION 'instructions[%].action deve ser: on, off, ou toggle', i;
          END IF;
        END IF;
        
        -- Validar delay
        IF instr_type = 'delay' THEN
          IF NOT (instr ? 'duration_ms') THEN
            RAISE EXCEPTION 'instructions[%] do tipo delay deve conter "duration_ms"', i;
          END IF;
        END IF;
      END;
    END LOOP;
    
    -- Validação de sequential_script concluída
    RETURN NEW;
  END IF;
  
  -- ✅ TIPO: Composite ou Simple (regra mãe com condicionais)
  -- Validar estrutura básica
  IF NOT (NEW.rule_json ? 'conditions' AND NEW.rule_json ? 'actions') THEN
    RAISE EXCEPTION 'rule_json deve conter "conditions" e "actions" (ou "script" para sequential_script)';
  END IF;
  
  -- Validar que actions é array
  IF jsonb_typeof(NEW.rule_json->'actions') != 'array' THEN
    RAISE EXCEPTION 'actions deve ser um array';
  END IF;
  
  -- Validar que conditions é objeto
  IF jsonb_typeof(NEW.rule_json->'conditions') != 'object' THEN
    RAISE EXCEPTION 'conditions deve ser um objeto';
  END IF;
  
  -- ✅ Validar estrutura de conditions (composite ou simples)
  cond_type := NEW.rule_json->'conditions'->>'type';
  
  IF cond_type = 'composite' THEN
    -- ✅ Regra Mãe: Validar estrutura composite
    IF NOT (NEW.rule_json->'conditions' ? 'children') THEN
      RAISE EXCEPTION 'conditions do tipo composite deve conter "children"';
    END IF;
    
    IF jsonb_typeof(NEW.rule_json->'conditions'->'children') != 'array' THEN
      RAISE EXCEPTION 'children deve ser um array';
    END IF;
    
    -- Validar operator (AND, OR, NAND, NOR)
    IF NOT (NEW.rule_json->'conditions'->>'operator' = ANY(ARRAY['AND', 'OR', 'NAND', 'NOR'])) THEN
      RAISE EXCEPTION 'operator deve ser: AND, OR, NAND, ou NOR';
    END IF;
    
    -- Validar cada child (condicional filha)
    child_count := jsonb_array_length(NEW.rule_json->'conditions'->'children');
    
    IF child_count = 0 THEN
      RAISE EXCEPTION 'children deve conter pelo menos uma condicional';
    END IF;
    
    IF child_count > 10 THEN
      RAISE EXCEPTION 'children deve conter no máximo 10 condicionais (limite MVP)';
    END IF;
    
    FOR i IN 0..child_count - 1 LOOP
      child := NEW.rule_json->'conditions'->'children'->i;
      
      -- Validar campos obrigatórios do child
      IF NOT (child ? 'sensor' AND child ? 'operator' AND child ? 'value') THEN
        RAISE EXCEPTION 'Cada child[%] deve conter: sensor, operator, value', i;
      END IF;
      
      -- Validar operator do child
      IF NOT (child->>'operator' = ANY(ARRAY['<', '>', '<=', '>=', '==', '!='])) THEN
        RAISE EXCEPTION 'child[%].operator deve ser: <, >, <=, >=, ==, !=', i;
      END IF;
      
      -- Validar que value é numérico
      IF jsonb_typeof(child->'value') != 'number' THEN
        RAISE EXCEPTION 'child[%].value deve ser um número', i;
      END IF;
      
      -- Validar sensor (valores permitidos)
      IF NOT (child->>'sensor' = ANY(ARRAY['ph', 'ec', 'tds', 'temperature', 'temp_water', 'temp_env', 'humidity', 'level'])) THEN
        RAISE EXCEPTION 'child[%].sensor deve ser: ph, ec, tds, temperature, temp_water, temp_env, humidity, ou level', i;
      END IF;
    END LOOP;
    
  ELSIF cond_type IS NULL OR cond_type = 'simple' THEN
    -- ✅ Condição simples: Validar campos básicos
    IF NOT (NEW.rule_json->'conditions' ? 'sensor' AND NEW.rule_json->'conditions' ? 'operator' AND NEW.rule_json->'conditions' ? 'value') THEN
      RAISE EXCEPTION 'conditions simples deve conter: sensor, operator, value';
    END IF;
  ELSE
    RAISE EXCEPTION 'conditions.type deve ser: "composite" ou "simple" (ou NULL para simples)';
  END IF;
  
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trigger_validate_decision_rule_json ON decision_rules;
CREATE TRIGGER trigger_validate_decision_rule_json
  BEFORE INSERT OR UPDATE ON decision_rules
  FOR EACH ROW
  EXECUTE FUNCTION validate_decision_rule_json();

-- ✅ TRIGGER 2: Atualizar contador de regras em device_status
CREATE OR REPLACE FUNCTION update_device_rules_count()
RETURNS TRIGGER AS $$
BEGIN
  -- Atualizar total_rules para o device_id afetado
  UPDATE device_status
  SET total_rules = (
    SELECT COUNT(*) FROM decision_rules
    WHERE device_id = COALESCE(NEW.device_id, OLD.device_id)
      AND enabled = true
  ),
  updated_at = now()
  WHERE device_id = COALESCE(NEW.device_id, OLD.device_id);
  
  RETURN COALESCE(NEW, OLD);
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trigger_update_device_rules_count ON decision_rules;
CREATE TRIGGER trigger_update_device_rules_count
  AFTER INSERT OR UPDATE OR DELETE ON decision_rules
  FOR EACH ROW
  EXECUTE FUNCTION update_device_rules_count();

-- ✅ TRIGGER 3: Validar arrays em relay_commands_slave
CREATE OR REPLACE FUNCTION validate_relay_commands_arrays()
RETURNS TRIGGER AS $$
BEGIN
  -- Validar que arrays têm o mesmo tamanho
  IF array_length(NEW.relay_numbers, 1) != array_length(NEW.actions, 1) THEN
    RAISE EXCEPTION 'relay_numbers e actions devem ter o mesmo tamanho';
  END IF;
  
  -- Validar que duration_seconds tem tamanho compatível (ou vazio)
  IF array_length(NEW.duration_seconds, 1) IS NOT NULL 
     AND array_length(NEW.duration_seconds, 1) != array_length(NEW.relay_numbers, 1) THEN
    RAISE EXCEPTION 'duration_seconds deve ter o mesmo tamanho de relay_numbers ou ser vazio';
  END IF;
  
  -- Validar valores de actions
  IF NOT (NEW.actions <@ ARRAY['on', 'off', 'toggle']::text[]) THEN
    RAISE EXCEPTION 'actions deve conter apenas: on, off, toggle';
  END IF;
  
  -- Validar valores de relay_numbers (0-7 para slaves)
  IF EXISTS (
    SELECT 1 FROM unnest(NEW.relay_numbers) AS relay_num
    WHERE relay_num < 0 OR relay_num > 7
  ) THEN
    RAISE EXCEPTION 'relay_numbers deve conter valores entre 0 e 7';
  END IF;
  
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trigger_validate_relay_commands_arrays ON relay_commands_slave;
CREATE TRIGGER trigger_validate_relay_commands_arrays
  BEFORE INSERT OR UPDATE ON relay_commands_slave
  FOR EACH ROW
  EXECUTE FUNCTION validate_relay_commands_arrays();

-- ✅ TRIGGER 4: Definir expires_at automaticamente se não fornecido
CREATE OR REPLACE FUNCTION set_command_expires_at()
RETURNS TRIGGER AS $$
BEGIN
  -- Se expires_at não foi fornecido, definir como 5 minutos no futuro
  IF NEW.expires_at IS NULL THEN
    NEW.expires_at = now() + INTERVAL '5 minutes';
  END IF;
  
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trigger_set_command_expires_at ON relay_commands_slave;
CREATE TRIGGER trigger_set_command_expires_at
  BEFORE INSERT ON relay_commands_slave
  FOR EACH ROW
  EXECUTE FUNCTION set_command_expires_at();

-- ============================================================
-- 3. ÍNDICES PARA PERFORMANCE
-- ============================================================

-- ✅ Índice para v_active_decision_rules
CREATE INDEX IF NOT EXISTS idx_decision_rules_device_enabled 
  ON decision_rules(device_id, enabled) 
  WHERE enabled = true;

-- ✅ Índice para v_slave_status_with_commands
CREATE INDEX IF NOT EXISTS idx_relay_commands_slave_status 
  ON relay_commands_slave(slave_device_id, status) 
  WHERE status IN ('pending', 'processing');

-- ✅ Índice para rule_executions
CREATE INDEX IF NOT EXISTS idx_rule_executions_device_rule 
  ON rule_executions(device_id, rule_id, created_at DESC);

-- ============================================================
-- 4. PERMISSÕES (RLS - Row Level Security)
-- ============================================================

-- ✅ Habilitar RLS nas views (se necessário)
-- Nota: Views herdam permissões das tabelas base

-- ✅ Política para v_active_decision_rules
-- (Usuários só veem regras de seus próprios dispositivos)
-- Isso deve ser configurado via RLS nas tabelas base

-- ============================================================
-- 5. COMENTÁRIOS (DOCUMENTAÇÃO)
-- ============================================================

COMMENT ON VIEW v_active_decision_rules IS 
  'View idempotente: Regras ativas prontas para o ESP32. GET sem side effects.';

COMMENT ON VIEW v_slave_status_with_commands IS 
  'View idempotente: Estado atual do slave + comandos pendentes. GET sem side effects.';

COMMENT ON VIEW v_rule_execution_history IS 
  'View idempotente: Histórico de execuções de regras com estatísticas. GET sem side effects.';

COMMENT ON FUNCTION validate_decision_rule_json() IS 
  'Valida estrutura do rule_json antes de INSERT/UPDATE em decision_rules. Suporta regras mãe com N condicionais filhas (composite conditions).';

COMMENT ON FUNCTION update_device_rules_count() IS 
  'Atualiza total_rules em device_status quando regras são criadas/atualizadas/deletadas.';

COMMENT ON FUNCTION validate_relay_commands_arrays() IS 
  'Valida arrays (relay_numbers, actions, duration_seconds) em relay_commands_slave.';

COMMENT ON FUNCTION set_command_expires_at() IS 
  'Define expires_at automaticamente como 5 minutos no futuro se não fornecido.';

-- ============================================================
-- FIM DO SCRIPT
-- ============================================================

