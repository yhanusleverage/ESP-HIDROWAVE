-- =====================================================
-- CORREÇÕES DE CONSISTÊNCIA DO SCHEMA SUPABASE
-- Data: 2024
-- Objetivo: Alinhar schema com demandas do frontend e embarcado
-- =====================================================

-- =====================================================
-- 1. ADICIONAR CAMPO slave_mac_address EM relay_commands
-- =====================================================
-- Problema: Frontend envia slave_mac_address mas não é armazenado
-- Solução: Adicionar campo para rastreamento por MAC

ALTER TABLE relay_commands 
ADD COLUMN IF NOT EXISTS slave_mac_address TEXT;

COMMENT ON COLUMN relay_commands.slave_mac_address IS 
'MAC address do slave ESP-NOW (ex: "14:33:5C:38:BF:60"). NULL para comandos locais.';

-- Índice para performance
CREATE INDEX IF NOT EXISTS idx_relay_commands_slave_mac 
ON relay_commands(slave_mac_address) 
WHERE slave_mac_address IS NOT NULL;

-- =====================================================
-- 2. ADICIONAR CAMPO current_state EM relay_commands
-- =====================================================
-- Problema: ACK contém currentState mas não é armazenado
-- Solução: Armazenar estado final do relé após ACK

ALTER TABLE relay_commands 
ADD COLUMN IF NOT EXISTS current_state BOOLEAN;

COMMENT ON COLUMN relay_commands.current_state IS 
'Estado final do relé após execução do comando (true=ON, false=OFF). NULL se ainda não executado.';

-- =====================================================
-- 3. CRIAR TABELA slave_relay_states PARA PERSISTÊNCIA
-- =====================================================
-- Problema: Estados de relés de slaves são apenas em memória
-- Solução: Criar tabela para persistir estados

CREATE TABLE IF NOT EXISTS slave_relay_states (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  master_device_id TEXT NOT NULL,
  slave_mac_address TEXT NOT NULL,
  slave_device_id TEXT NOT NULL,  -- ESP32_SLAVE_XX_XX_XX_XX_XX_XX
  relay_number INTEGER NOT NULL CHECK (relay_number >= 0 AND relay_number <= 7),
  state BOOLEAN NOT NULL,
  has_timer BOOLEAN DEFAULT false,
  remaining_time INTEGER DEFAULT 0,
  relay_name TEXT,  -- Nome do relé (opcional)
  last_update TIMESTAMPTZ DEFAULT NOW(),
  updated_at TIMESTAMPTZ DEFAULT NOW(),
  
  -- Constraints
  UNIQUE(master_device_id, slave_mac_address, relay_number),
  
  -- Foreign keys
  CONSTRAINT fk_slave_relay_states_master 
    FOREIGN KEY (master_device_id) REFERENCES device_status(device_id),
  CONSTRAINT fk_slave_relay_states_slave 
    FOREIGN KEY (slave_device_id) REFERENCES device_status(device_id)
);

COMMENT ON TABLE slave_relay_states IS 
'Tabela para persistir estados de relés de slaves ESP-NOW. Atualizada quando Master recebe ACK ou status do slave.';

-- Índices para performance
CREATE INDEX IF NOT EXISTS idx_slave_relay_states_master_slave 
ON slave_relay_states(master_device_id, slave_mac_address);

CREATE INDEX IF NOT EXISTS idx_slave_relay_states_device 
ON slave_relay_states(slave_device_id);

CREATE INDEX IF NOT EXISTS idx_slave_relay_states_master 
ON slave_relay_states(master_device_id);

-- =====================================================
-- 4. ADICIONAR COLUNA updated_at EM relay_commands
-- =====================================================
-- Problema: VIEW precisa de updated_at mas coluna não existe
-- Solução: Adicionar coluna updated_at antes de criar trigger

ALTER TABLE relay_commands 
ADD COLUMN IF NOT EXISTS updated_at TIMESTAMPTZ DEFAULT NOW();

COMMENT ON COLUMN relay_commands.updated_at IS 
'Timestamp da última atualização do registro. Atualizado automaticamente por trigger.';

-- =====================================================
-- 5. TRIGGER PARA updated_at AUTOMÁTICO
-- =====================================================
-- Problema: updated_at pode não ser atualizado automaticamente
-- Solução: Criar trigger para atualizar automaticamente

-- Função para atualizar updated_at
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- Aplicar trigger em relay_commands
DROP TRIGGER IF EXISTS update_relay_commands_updated_at ON relay_commands;
CREATE TRIGGER update_relay_commands_updated_at
BEFORE UPDATE ON relay_commands
FOR EACH ROW
EXECUTE FUNCTION update_updated_at_column();

-- Aplicar trigger em slave_relay_states
DROP TRIGGER IF EXISTS update_slave_relay_states_updated_at ON slave_relay_states;
CREATE TRIGGER update_slave_relay_states_updated_at
BEFORE UPDATE ON slave_relay_states
FOR EACH ROW
EXECUTE FUNCTION update_updated_at_column();

-- =====================================================
-- 6. POLÍTICAS RLS (Row Level Security)
-- =====================================================
-- Habilitar RLS na nova tabela
ALTER TABLE slave_relay_states ENABLE ROW LEVEL SECURITY;

-- Política: Usuários podem ver apenas seus próprios slaves
CREATE POLICY "Users can view their own slave relay states"
ON slave_relay_states
FOR SELECT
USING (
  EXISTS (
    SELECT 1 FROM device_status ds
    WHERE ds.device_id = slave_relay_states.master_device_id
    AND ds.user_email = current_setting('request.jwt.claims', true)::json->>'email'
  )
);

-- Política: Sistema pode inserir/atualizar (via service role)
CREATE POLICY "Service role can manage slave relay states"
ON slave_relay_states
FOR ALL
USING (true)
WITH CHECK (true);

-- =====================================================
-- 7. VIEW PARA FACILITAR CONSULTAS
-- =====================================================
-- View combinando relay_commands com slave_relay_states

CREATE OR REPLACE VIEW relay_commands_with_states AS
SELECT 
  rc.id,
  rc.device_id,
  rc.target_device_id,
  rc.slave_mac_address,
  rc.relay_number,
  rc.action,
  rc.status,
  rc.current_state,
  rc.duration_seconds,
  rc.created_at,
  rc.updated_at,
  rc.completed_at,
  srs.state as slave_current_state,
  srs.has_timer as slave_has_timer,
  srs.remaining_time as slave_remaining_time,
  srs.last_update as slave_last_update
FROM relay_commands rc
LEFT JOIN slave_relay_states srs ON 
  rc.device_id = srs.master_device_id 
  AND rc.slave_mac_address = srs.slave_mac_address
  AND rc.relay_number = srs.relay_number
WHERE rc.target_device_id IS NOT NULL 
  AND rc.target_device_id != '';

COMMENT ON VIEW relay_commands_with_states IS 
'View combinando relay_commands com estados atuais dos slaves para facilitar consultas do frontend.';

-- =====================================================
-- VERIFICAÇÃO FINAL
-- =====================================================
-- Verificar se todas as alterações foram aplicadas

DO $$
BEGIN
  -- Verificar campos em relay_commands
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.columns 
    WHERE table_name = 'relay_commands' 
    AND column_name = 'slave_mac_address'
  ) THEN
    RAISE EXCEPTION 'Campo slave_mac_address não foi criado em relay_commands';
  END IF;
  
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.columns 
    WHERE table_name = 'relay_commands' 
    AND column_name = 'current_state'
  ) THEN
    RAISE EXCEPTION 'Campo current_state não foi criado em relay_commands';
  END IF;
  
  -- ✅ NOVO: Verificar se updated_at foi criado
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.columns 
    WHERE table_name = 'relay_commands' 
    AND column_name = 'updated_at'
  ) THEN
    RAISE EXCEPTION 'Campo updated_at não foi criado em relay_commands';
  END IF;
  
  -- Verificar tabela slave_relay_states
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.tables 
    WHERE table_name = 'slave_relay_states'
  ) THEN
    RAISE EXCEPTION 'Tabela slave_relay_states não foi criada';
  END IF;
  
  RAISE NOTICE '✅ Todas as correções foram aplicadas com sucesso!';
END $$;

