-- =====================================================
-- SCRIPT COMPLETO DE CONFIGURAÇÃO SUPABASE
-- Sistema Hidropônico ESP32 + Next.js
-- =====================================================

-- Limpar tabelas existentes (se necessário)
DROP TABLE IF EXISTS relay_commands CASCADE;
DROP TABLE IF EXISTS device_status CASCADE;
DROP TABLE IF EXISTS environment_data CASCADE;
DROP TABLE IF EXISTS hydro_measurements CASCADE;

-- =====================================================
-- 1. TABELA DE DADOS AMBIENTAIS
-- =====================================================
CREATE TABLE environment_data (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id TEXT NOT NULL,
  temperature FLOAT NOT NULL,
  humidity FLOAT NOT NULL,
  created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Índices para performance
CREATE INDEX idx_environment_created_at ON environment_data (created_at);
CREATE INDEX idx_environment_device_id ON environment_data (device_id);
CREATE INDEX idx_environment_device_date ON environment_data (device_id, created_at);

-- =====================================================
-- 2. TABELA DE MEDIÇÕES HIDROPÔNICAS
-- =====================================================
CREATE TABLE hydro_measurements (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id TEXT NOT NULL,
  temperature FLOAT NOT NULL,
  ph FLOAT NOT NULL,
  tds FLOAT NOT NULL,
  water_level_ok BOOLEAN NOT NULL,
  created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Índices para performance
CREATE INDEX idx_hydro_created_at ON hydro_measurements (created_at);
CREATE INDEX idx_hydro_device_id ON hydro_measurements (device_id);
CREATE INDEX idx_hydro_device_date ON hydro_measurements (device_id, created_at);

-- =====================================================
-- 3. TABELA DE COMANDOS DE RELÉS
-- =====================================================
CREATE TABLE relay_commands (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id TEXT NOT NULL,
  relay_number INTEGER NOT NULL CHECK (relay_number >= 0 AND relay_number <= 15),
  action TEXT NOT NULL CHECK (action IN ('on', 'off')),
  duration_seconds INTEGER,
  status TEXT DEFAULT 'pending' CHECK (status IN ('pending', 'sent', 'completed', 'failed')),
  created_at TIMESTAMPTZ DEFAULT NOW(),
  sent_at TIMESTAMPTZ,
  completed_at TIMESTAMPTZ,
  created_by TEXT DEFAULT 'web_interface',
  error_message TEXT
);

-- Índices para performance
CREATE INDEX idx_relay_commands_device_status ON relay_commands(device_id, status);
CREATE INDEX idx_relay_commands_created_at ON relay_commands(created_at);
CREATE INDEX idx_relay_commands_device_pending ON relay_commands(device_id, status) WHERE status = 'pending';

-- =====================================================
-- 4. TABELA DE STATUS DOS DISPOSITIVOS
-- =====================================================
CREATE TABLE device_status (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id TEXT UNIQUE NOT NULL,
  last_seen TIMESTAMPTZ DEFAULT NOW(),
  wifi_rssi INTEGER,
  free_heap INTEGER,
  uptime_seconds INTEGER,
  relay_states BOOLEAN[] DEFAULT ARRAY[false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false],
  is_online BOOLEAN DEFAULT false,
  firmware_version TEXT,
  ip_address INET,
  created_at TIMESTAMPTZ DEFAULT NOW(),
  updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- Índices para performance
CREATE INDEX idx_device_status_device_id ON device_status(device_id);
CREATE INDEX idx_device_status_online ON device_status(is_online);
CREATE INDEX idx_device_status_last_seen ON device_status(last_seen);

-- =====================================================
-- 5. POLÍTICAS DE SEGURANÇA (RLS)
-- =====================================================

-- Habilitar RLS em todas as tabelas
ALTER TABLE environment_data ENABLE ROW LEVEL SECURITY;
ALTER TABLE hydro_measurements ENABLE ROW LEVEL SECURITY;
ALTER TABLE relay_commands ENABLE ROW LEVEL SECURITY;
ALTER TABLE device_status ENABLE ROW LEVEL SECURITY;

-- Políticas para environment_data
CREATE POLICY "Enable insert for anon" ON environment_data
  FOR INSERT WITH CHECK (true);
CREATE POLICY "Enable read for anon" ON environment_data
  FOR SELECT USING (true);

-- Políticas para hydro_measurements
CREATE POLICY "Enable insert for anon" ON hydro_measurements
  FOR INSERT WITH CHECK (true);
CREATE POLICY "Enable read for anon" ON hydro_measurements
  FOR SELECT USING (true);

-- Políticas para relay_commands
CREATE POLICY "Enable insert for anon" ON relay_commands
  FOR INSERT WITH CHECK (true);
CREATE POLICY "Enable select for anon" ON relay_commands
  FOR SELECT USING (true);
CREATE POLICY "Enable update for anon" ON relay_commands
  FOR UPDATE USING (true);

-- Políticas para device_status
CREATE POLICY "Enable insert for anon" ON device_status
  FOR INSERT WITH CHECK (true);
CREATE POLICY "Enable select for anon" ON device_status
  FOR SELECT USING (true);
CREATE POLICY "Enable update for anon" ON device_status
  FOR UPDATE USING (true);

-- =====================================================
-- 6. FUNÇÕES AUXILIARES
-- =====================================================

-- Função para limpar comandos antigos (executar periodicamente)
CREATE OR REPLACE FUNCTION cleanup_old_commands()
RETURNS void AS $$
BEGIN
  -- Remover comandos completados/falhados com mais de 24 horas
  DELETE FROM relay_commands 
  WHERE status IN ('completed', 'failed') 
    AND created_at < NOW() - INTERVAL '24 hours';
    
  -- Remover comandos pendentes com mais de 1 hora (expirados)
  UPDATE relay_commands 
  SET status = 'failed', error_message = 'Comando expirado'
  WHERE status = 'pending' 
    AND created_at < NOW() - INTERVAL '1 hour';
END;
$$ LANGUAGE plpgsql;

-- Função para atualizar status online dos dispositivos
CREATE OR REPLACE FUNCTION update_device_online_status()
RETURNS void AS $$
BEGIN
  -- Marcar dispositivos como offline se não enviaram dados há mais de 2 minutos
  UPDATE device_status 
  SET is_online = false 
  WHERE last_seen < NOW() - INTERVAL '2 minutes' 
    AND is_online = true;
END;
$$ LANGUAGE plpgsql;

-- =====================================================
-- 7. DADOS INICIAIS
-- =====================================================

-- Inserir dispositivo padrão
INSERT INTO device_status (device_id, firmware_version, is_online) 
VALUES ('ESP32_HIDRO_001', '2.1.0', false)
ON CONFLICT (device_id) DO NOTHING;

-- =====================================================
-- 8. VIEWS ÚTEIS
-- =====================================================

-- View para últimos dados dos sensores
CREATE OR REPLACE VIEW latest_sensor_data AS
SELECT DISTINCT ON (device_id)
  device_id,
  temperature as env_temperature,
  humidity as env_humidity,
  created_at
FROM environment_data
ORDER BY device_id, created_at DESC;

-- View para últimas medições hidropônicas
CREATE OR REPLACE VIEW latest_hydro_data AS
SELECT DISTINCT ON (device_id)
  device_id,
  temperature as water_temperature,
  ph,
  tds,
  water_level_ok,
  created_at
FROM hydro_measurements
ORDER BY device_id, created_at DESC;

-- View para status completo dos dispositivos
CREATE OR REPLACE VIEW device_full_status AS
SELECT 
  ds.*,
  lsd.env_temperature,
  lsd.env_humidity,
  lhd.water_temperature,
  lhd.ph,
  lhd.tds,
  lhd.water_level_ok,
  (SELECT COUNT(*) FROM relay_commands rc WHERE rc.device_id = ds.device_id AND rc.status = 'pending') as pending_commands
FROM device_status ds
LEFT JOIN latest_sensor_data lsd ON ds.device_id = lsd.device_id
LEFT JOIN latest_hydro_data lhd ON ds.device_id = lhd.device_id;

-- =====================================================
-- 9. TRIGGERS PARA MANUTENÇÃO AUTOMÁTICA
-- =====================================================

-- Trigger para atualizar updated_at automaticamente
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
  NEW.updated_at = NOW();
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER update_device_status_updated_at
  BEFORE UPDATE ON device_status
  FOR EACH ROW
  EXECUTE FUNCTION update_updated_at_column();

-- =====================================================
-- 10. COMENTÁRIOS PARA DOCUMENTAÇÃO
-- =====================================================

COMMENT ON TABLE environment_data IS 'Dados ambientais (temperatura e umidade do ar)';
COMMENT ON TABLE hydro_measurements IS 'Medições hidropônicas (água, pH, TDS, nível)';
COMMENT ON TABLE relay_commands IS 'Comandos para controle dos relés ESP32';
COMMENT ON TABLE device_status IS 'Status em tempo real dos dispositivos ESP32';

COMMENT ON COLUMN relay_commands.status IS 'pending: aguardando envio, sent: enviado, completed: executado, failed: falhou';
COMMENT ON COLUMN device_status.relay_states IS 'Array de 16 booleanos representando estado dos relés (0-15)';

-- =====================================================
-- SCRIPT CONCLUÍDO
-- =====================================================

-- Verificar se tudo foi criado corretamente
SELECT 
  'environment_data' as table_name, 
  COUNT(*) as row_count 
FROM environment_data
UNION ALL
SELECT 
  'hydro_measurements' as table_name, 
  COUNT(*) as row_count 
FROM hydro_measurements
UNION ALL
SELECT 
  'relay_commands' as table_name, 
  COUNT(*) as row_count 
FROM relay_commands
UNION ALL
SELECT 
  'device_status' as table_name, 
  COUNT(*) as row_count 
FROM device_status;

-- Mostrar estrutura das tabelas
SELECT 
  table_name,
  column_name,
  data_type,
  is_nullable
FROM information_schema.columns 
WHERE table_name IN ('environment_data', 'hydro_measurements', 'relay_commands', 'device_status')
ORDER BY table_name, ordinal_position; 