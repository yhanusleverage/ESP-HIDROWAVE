-- =====================================================
-- SETUP COMPLETO DO BANCO - SISTEMA HIDROPÔNICO ESP32
-- Projeto: ESP32 + Next.js + Supabase
-- Device: DINÁMICO (ESP32_HIDRO_XXXXXX) | Firmware: 2.1.0
-- Arquitetura: Global State + Command Queue + Múltiples Dispositivos
-- =====================================================

-- =====================================================
-- PARTE 1: TABELAS PRINCIPAIS (MÚLTIPLES DISPOSITIVOS)
-- =====================================================

-- Limpar estruturas existentes se necessário
DROP TABLE IF EXISTS relay_commands CASCADE;
DROP TABLE IF EXISTS device_status CASCADE;
DROP TABLE IF EXISTS environment_data CASCADE;
DROP TABLE IF EXISTS hydro_measurements CASCADE;

-- 1. DADOS AMBIENTAIS (DHT22 - Pino 15) - SIN DEFAULT HARDCODEADO
CREATE TABLE environment_data (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id TEXT NOT NULL, -- ❌ REMOVIDO DEFAULT hardcodeado
  temperature FLOAT NOT NULL CHECK (temperature >= 0.0 AND temperature <= 50.0),
  humidity FLOAT NOT NULL CHECK (humidity >= 0.0 AND humidity <= 100.0),
  created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Índices otimizados para múltiples dispositivos
CREATE INDEX idx_environment_created_at ON environment_data (created_at DESC);
CREATE INDEX idx_environment_device_id ON environment_data (device_id);
CREATE INDEX idx_environment_device_date ON environment_data (device_id, created_at DESC);

-- 2. MEDIÇÕES HIDROPÔNICAS - SIN DEFAULT HARDCODEADO
CREATE TABLE hydro_measurements (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id TEXT NOT NULL, -- ❌ REMOVIDO DEFAULT hardcodeado
  temperature FLOAT NOT NULL CHECK (temperature >= 0.0 AND temperature <= 50.0),
  ph FLOAT NOT NULL CHECK (ph >= 0.0 AND ph <= 14.0),
  tds FLOAT NOT NULL CHECK (tds >= 0.0 AND tds <= 5000.0),
  water_level_ok BOOLEAN NOT NULL,
  created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Índices para performance com múltiples dispositivos
CREATE INDEX idx_hydro_created_at ON hydro_measurements (created_at DESC);
CREATE INDEX idx_hydro_device_id ON hydro_measurements (device_id);
CREATE INDEX idx_hydro_device_date ON hydro_measurements (device_id, created_at DESC);

-- 3. COMANDOS DE RELÉS - SIN DEFAULT HARDCODEADO
CREATE TABLE relay_commands (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id TEXT NOT NULL, -- ❌ REMOVIDO DEFAULT hardcodeado
  relay_number INTEGER NOT NULL CHECK (relay_number >= 0 AND relay_number <= 15),
  action TEXT NOT NULL CHECK (action IN ('on', 'off')),
  duration_seconds INTEGER CHECK (duration_seconds IS NULL OR (duration_seconds > 0 AND duration_seconds <= 86400)),
  status TEXT DEFAULT 'pending' CHECK (status IN ('pending', 'sent', 'completed', 'failed')),
  created_at TIMESTAMPTZ DEFAULT NOW(),
  sent_at TIMESTAMPTZ,
  completed_at TIMESTAMPTZ,
  created_by TEXT DEFAULT 'web_interface',
  error_message TEXT
);

-- Índices críticos para polling de múltiples ESP32
CREATE INDEX idx_relay_commands_device_status ON relay_commands(device_id, status);
CREATE INDEX idx_relay_commands_created_at ON relay_commands(created_at);
CREATE INDEX idx_relay_commands_device_pending ON relay_commands(device_id, status) WHERE status = 'pending';

-- 4. STATUS DO DISPOSITIVO - TABLA PRINCIPAL PARA MÚLTIPLES DISPOSITIVOS
CREATE TABLE device_status (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id TEXT UNIQUE NOT NULL, -- ❌ REMOVIDO DEFAULT hardcodeado
  last_seen TIMESTAMPTZ DEFAULT NOW(),
  wifi_rssi INTEGER,
  free_heap INTEGER,
  uptime_seconds INTEGER,
  relay_states BOOLEAN[] DEFAULT ARRAY[false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false],
  is_online BOOLEAN DEFAULT false,
  firmware_version TEXT DEFAULT '2.1.0',
  ip_address INET,
  
  -- ✅ NUEVOS CAMPOS PARA MÚLTIPLES DISPOSITIVOS
  mac_address TEXT,
  device_name TEXT,
  location TEXT,
  device_type TEXT DEFAULT 'ESP32_HYDROPONIC',
  
  created_at TIMESTAMPTZ DEFAULT NOW(),
  updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- Índices para monitoramento de múltiples dispositivos
CREATE INDEX idx_device_status_device_id ON device_status(device_id);
CREATE INDEX idx_device_status_online ON device_status(is_online);
CREATE INDEX idx_device_status_last_seen ON device_status(last_seen);
CREATE INDEX idx_device_status_mac ON device_status(mac_address); -- ✅ NUEVO ÍNDICE

-- =====================================================
-- PARTE 2: VIEWS AVANÇADAS PARA MÚLTIPLES DISPOSITIVOS
-- =====================================================

-- 1. DEVICE_FULL_STATUS actualizada para múltiples dispositivos
CREATE VIEW device_full_status AS
SELECT 
  ds.device_id,
  ds.device_name,
  ds.location,
  ds.device_type,
  ds.mac_address, -- ✅ NUEVO CAMPO
  ds.last_seen,
  ds.is_online,
  CASE 
    WHEN ds.last_seen > NOW() - INTERVAL '5 minutes' THEN true
    ELSE false
  END as recently_seen,
  EXTRACT(EPOCH FROM (NOW() - ds.last_seen))/60 as minutes_offline,
  ds.wifi_rssi,
  ds.free_heap,
  ds.uptime_seconds,
  ds.ip_address,
  ds.firmware_version,
  ds.relay_states,
  
  -- Contar relés ativos
  (
    SELECT COUNT(*) 
    FROM unnest(ds.relay_states) AS relay_state 
    WHERE relay_state = true
  ) as active_relays_count,
  
  -- Estadísticas de comandos recientes (últimas 24h)
  COALESCE(recent_stats.total_commands, 0) as commands_24h,
  COALESCE(recent_stats.pending_commands, 0) as pending_commands,
  COALESCE(recent_stats.sent_commands, 0) as sent_commands,
  COALESCE(recent_stats.completed_commands, 0) as completed_commands,
  COALESCE(recent_stats.failed_commands, 0) as failed_commands,
  
  -- Último comando
  recent_stats.last_command_at,
  recent_stats.last_command_action,
  recent_stats.last_command_relay
  
FROM device_status ds
LEFT JOIN (
  SELECT 
    device_id,
    COUNT(*) as total_commands,
    COUNT(CASE WHEN status = 'pending' THEN 1 END) as pending_commands,
    COUNT(CASE WHEN status = 'sent' THEN 1 END) as sent_commands,
    COUNT(CASE WHEN status = 'completed' THEN 1 END) as completed_commands,
    COUNT(CASE WHEN status = 'failed' THEN 1 END) as failed_commands,
    MAX(created_at) as last_command_at,
    (SELECT action FROM relay_commands rc2 WHERE rc2.device_id = rc.device_id AND rc2.created_at = MAX(rc.created_at) LIMIT 1) as last_command_action,
    (SELECT relay_number FROM relay_commands rc3 WHERE rc3.device_id = rc.device_id AND rc3.created_at = MAX(rc.created_at) LIMIT 1) as last_command_relay
  FROM relay_commands rc
  WHERE rc.created_at > NOW() - INTERVAL '24 hours'
  GROUP BY device_id
) recent_stats ON ds.device_id = recent_stats.device_id;

-- 2. RELAY_CURRENT_STATE para múltiples dispositivos
CREATE VIEW relay_current_state AS
SELECT 
  ds.device_id,
  ds.device_name, -- ✅ NUEVO CAMPO
  relay_num.relay_number,
  ds.relay_states[relay_num.relay_number + 1] as is_active,
  
  -- Nomes específicos do projeto
  CASE relay_num.relay_number
    WHEN 0 THEN '💧 Bomba Principal'
    WHEN 1 THEN '🧪 Bomba Nutrientes'
    WHEN 2 THEN '⚗️ Bomba pH'
    WHEN 3 THEN '💨 Ventilador'
    WHEN 4 THEN '💡 Luz UV'
    WHEN 5 THEN '🔥 Aquecedor'
    WHEN 6 THEN '🌊 Bomba Circulação'
    WHEN 7 THEN '🫧 Bomba Oxigenação'
    WHEN 8 THEN '🚪 Válvula Entrada'
    WHEN 9 THEN '🚪 Válvula Saída'
    WHEN 10 THEN '🔄 Sensor Agitador'
    WHEN 11 THEN '🌱 Luz LED Crescimento'
    WHEN 12 THEN '📱 Reserva 1'
    WHEN 13 THEN '📱 Reserva 2'
    WHEN 14 THEN '📱 Reserva 3'
    WHEN 15 THEN '📱 Reserva 4'
  END as relay_name,
  
  -- Último comando para este relé
  last_cmd.last_command_at,
  last_cmd.last_command_action,
  last_cmd.last_command_status,
  
  -- Comando ativo (pending/sent)
  active_cmd.active_command_id,
  active_cmd.active_command_action,
  active_cmd.active_command_status,
  active_cmd.active_command_duration,
  active_cmd.active_command_created_at,
  
  ds.last_seen as device_last_seen

FROM device_status ds
CROSS JOIN generate_series(0, 15) as relay_num(relay_number)
LEFT JOIN (
  -- Último comando por relé
  SELECT DISTINCT ON (device_id, relay_number)
    device_id,
    relay_number,
    created_at as last_command_at,
    action as last_command_action,
    status as last_command_status
  FROM relay_commands
  ORDER BY device_id, relay_number, created_at DESC
) last_cmd ON ds.device_id = last_cmd.device_id AND relay_num.relay_number = last_cmd.relay_number
LEFT JOIN (
  -- Comando ativo (pending ou sent)
  SELECT DISTINCT ON (device_id, relay_number)
    device_id,
    relay_number,
    id as active_command_id,
    action as active_command_action,
    status as active_command_status,
    duration_seconds as active_command_duration,
    created_at as active_command_created_at
  FROM relay_commands
  WHERE status IN ('pending', 'sent')
  ORDER BY device_id, relay_number, created_at ASC
) active_cmd ON ds.device_id = active_cmd.device_id AND relay_num.relay_number = active_cmd.relay_number;

-- ✅ 3. NUEVA VIEW para estadísticas globales de múltiples dispositivos
CREATE VIEW multi_device_stats AS
SELECT 
  COUNT(*) as total_devices,
  COUNT(CASE WHEN is_online = true THEN 1 END) as online_devices,
  COUNT(CASE WHEN is_online = false THEN 1 END) as offline_devices,
  COUNT(CASE WHEN last_seen > NOW() - INTERVAL '5 minutes' THEN 1 END) as recently_active,
  AVG(free_heap) as avg_heap_usage,
  MIN(free_heap) as min_heap_usage,
  MAX(free_heap) as max_heap_usage,
  COUNT(DISTINCT location) as unique_locations,
  COUNT(DISTINCT device_type) as unique_device_types
FROM device_status;

-- ✅ 4. VIEW para descubrimiento de dispositivos
CREATE VIEW device_discovery AS
SELECT 
  ds.device_id,
  ds.device_name,
  ds.location,
  ds.device_type,
  ds.mac_address,
  ds.ip_address,
  ds.firmware_version,
  ds.is_online,
  ds.last_seen,
  CASE 
    WHEN ds.last_seen > NOW() - INTERVAL '2 minutes' THEN 'online'
    WHEN ds.last_seen > NOW() - INTERVAL '10 minutes' THEN 'recent'
    ELSE 'offline'
  END as connection_status,
  
  -- Contadores de actividad
  (SELECT COUNT(*) FROM environment_data ed WHERE ed.device_id = ds.device_id AND ed.created_at > NOW() - INTERVAL '24 hours') as sensor_readings_24h,
  (SELECT COUNT(*) FROM relay_commands rc WHERE rc.device_id = ds.device_id AND rc.created_at > NOW() - INTERVAL '24 hours') as commands_24h,
  
  ds.created_at as first_seen,
  ds.updated_at as last_updated
FROM device_status ds
ORDER BY ds.last_seen DESC;

-- Resto de las views existentes...
CREATE VIEW automation_analysis AS
SELECT 
  device_id,
  
  -- Estado atual
  (SELECT COUNT(*) FROM unnest(relay_states) AS state WHERE state = true) as active_relays,
  
  -- Atividade nas últimas horas
  (SELECT COUNT(*) FROM relay_commands WHERE device_id = ds.device_id AND created_at > NOW() - INTERVAL '1 hour') as commands_last_hour,
  (SELECT COUNT(*) FROM relay_commands WHERE device_id = ds.device_id AND created_at > NOW() - INTERVAL '6 hours') as commands_last_6h,
  (SELECT COUNT(*) FROM relay_commands WHERE device_id = ds.device_id AND created_at > NOW() - INTERVAL '24 hours') as commands_last_24h,
  
  -- Comandos pendentes/ativos
  (SELECT COUNT(*) FROM relay_commands WHERE device_id = ds.device_id AND status = 'pending') as pending_count,
  (SELECT COUNT(*) FROM relay_commands WHERE device_id = ds.device_id AND status = 'sent') as sent_count,
  
  -- Análise de sobrecarga
  CASE 
    WHEN (SELECT COUNT(*) FROM relay_commands WHERE device_id = ds.device_id AND status IN ('pending', 'sent')) > 5 THEN 'OVERLOADED'
    WHEN (SELECT COUNT(*) FROM relay_commands WHERE device_id = ds.device_id AND status IN ('pending', 'sent')) > 3 THEN 'BUSY'
    WHEN (SELECT COUNT(*) FROM relay_commands WHERE device_id = ds.device_id AND status IN ('pending', 'sent')) > 0 THEN 'ACTIVE'
    ELSE 'IDLE'
  END as system_load,
  
  -- Recomendação para automação
  CASE 
    WHEN (SELECT COUNT(*) FROM relay_commands WHERE device_id = ds.device_id AND status IN ('pending', 'sent')) > 3 THEN false
    WHEN NOT ds.is_online THEN false
    WHEN ds.free_heap < 20000 THEN false
    ELSE true
  END as automation_ready,
  
  last_seen,
  is_online
  
FROM device_status ds;

-- VIEW para métricas de saúde
CREATE VIEW system_health_metrics AS
SELECT 
  ds.device_id,
  ds.device_name, -- ✅ NUEVO CAMPO
  ds.location,    -- ✅ NUEVO CAMPO
  
  -- Status geral
  ds.is_online,
  ds.last_seen,
  EXTRACT(EPOCH FROM (NOW() - ds.last_seen))/60 as minutes_offline,
  
  -- Performance ESP32
  ds.wifi_rssi,
  ds.free_heap,
  ds.uptime_seconds,
  CASE 
    WHEN ds.free_heap > 180000 THEN 'EXCELLENT'
    WHEN ds.free_heap > 100000 THEN 'GOOD'
    WHEN ds.free_heap > 50000 THEN 'FAIR'
    WHEN ds.free_heap > 20000 THEN 'WARNING'
    ELSE 'CRITICAL'
  END as memory_status,
  
  -- Análise de comandos (últimas 24h)
  COALESCE(cmd_stats.total_commands, 0) as commands_24h,
  COALESCE(cmd_stats.success_rate, 100) as success_rate_percent,
  COALESCE(cmd_stats.avg_completion_time, 0) as avg_completion_seconds,
  
  -- Estado dos relés
  (SELECT COUNT(*) FROM unnest(ds.relay_states) AS state WHERE state = true) as active_relays,
  
  -- Score de saúde geral (0-100)
  CASE 
    WHEN NOT ds.is_online THEN 0
    WHEN ds.last_seen < NOW() - INTERVAL '10 minutes' THEN 25
    WHEN ds.free_heap < 20000 THEN 30
    WHEN ds.wifi_rssi < -80 THEN 40
    WHEN COALESCE(cmd_stats.success_rate, 100) < 80 THEN 60
    WHEN ds.free_heap < 50000 THEN 80
    ELSE 100
  END as health_score
  
FROM device_status ds
LEFT JOIN (
  SELECT 
    device_id,
    COUNT(*) as total_commands,
    ROUND(
      (COUNT(CASE WHEN status = 'completed' THEN 1 END) * 100.0 / COUNT(*)), 
      1
    ) as success_rate,
    AVG(EXTRACT(EPOCH FROM (completed_at - sent_at))) as avg_completion_time
  FROM relay_commands 
  WHERE created_at > NOW() - INTERVAL '24 hours'
    AND status IN ('completed', 'failed')
  GROUP BY device_id
) cmd_stats ON ds.device_id = cmd_stats.device_id;

-- =====================================================
-- PARTE 3: POLÍTICAS DE SEGURANÇA (RLS) - SIN CAMBIOS
-- =====================================================

-- Habilitar RLS en todas las tabelas
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
-- PARTE 4: FUNÇÕES AUXILIARES PARA MÚLTIPLES DISPOSITIVOS
-- =====================================================

-- ✅ NUEVA FUNCIÓN: Auto-registro de dispositivos
CREATE OR REPLACE FUNCTION auto_register_device(
    p_device_id TEXT,
    p_mac_address TEXT,
    p_ip_address INET DEFAULT NULL,
    p_device_name TEXT DEFAULT NULL,
    p_location TEXT DEFAULT NULL
)
RETURNS TEXT AS $$
DECLARE
    result_message TEXT;
BEGIN
    -- Intentar insertar o actualizar dispositivo
    INSERT INTO device_status (
        device_id, 
        mac_address, 
        ip_address, 
        device_name, 
        location,
        last_seen,
        is_online
    ) VALUES (
        p_device_id,
        p_mac_address,
        p_ip_address,
        COALESCE(p_device_name, 'ESP32 - ' || RIGHT(p_mac_address, 8)),
        COALESCE(p_location, 'Ubicación no especificada'),
        NOW(),
        true
    )
    ON CONFLICT (device_id) DO UPDATE SET
        mac_address = EXCLUDED.mac_address,
        ip_address = COALESCE(EXCLUDED.ip_address, device_status.ip_address),
        device_name = COALESCE(EXCLUDED.device_name, device_status.device_name),
        location = COALESCE(EXCLUDED.location, device_status.location),
        last_seen = NOW(),
        is_online = true,
        updated_at = NOW();
    
    result_message := 'Dispositivo ' || p_device_id || ' registrado/actualizado exitosamente';
    
    RAISE NOTICE '%', result_message;
    RETURN result_message;
END;
$$ LANGUAGE plpgsql;

-- ✅ NUEVA FUNCIÓN: Obtener dispositivos por patrón MAC
CREATE OR REPLACE FUNCTION find_devices_by_mac_pattern(mac_pattern TEXT)
RETURNS TABLE (
    device_id TEXT,
    device_name TEXT,
    mac_address TEXT,
    is_online BOOLEAN,
    last_seen TIMESTAMPTZ
) AS $$
BEGIN
    RETURN QUERY
    SELECT 
        ds.device_id,
        ds.device_name,
        ds.mac_address,
        ds.is_online,
        ds.last_seen
    FROM device_status ds
    WHERE ds.mac_address ILIKE '%' || mac_pattern || '%'
    ORDER BY ds.last_seen DESC;
END;
$$ LANGUAGE plpgsql;

-- Función para limpar comandos antigos (sin cambios)
CREATE OR REPLACE FUNCTION cleanup_old_commands()
RETURNS void AS $$
BEGIN
  DELETE FROM relay_commands 
  WHERE status IN ('completed', 'failed') 
    AND created_at < NOW() - INTERVAL '24 hours';
    
  UPDATE relay_commands 
  SET status = 'failed', error_message = 'Comando expirado - timeout 1h'
  WHERE status = 'pending' 
    AND created_at < NOW() - INTERVAL '1 hour';
    
  RAISE NOTICE 'Limpeza automática executada em %', NOW();
END;
$$ LANGUAGE plpgsql;

-- Función para actualizar status online (sin cambios)
CREATE OR REPLACE FUNCTION update_device_online_status()
RETURNS void AS $$
BEGIN
  UPDATE device_status 
  SET is_online = false 
  WHERE last_seen < NOW() - INTERVAL '2 minutes' 
    AND is_online = true;
END;
$$ LANGUAGE plpgsql;

-- =====================================================
-- PARTE 5: DATOS INICIALES - ❌ REMOVIDO HARDCODING
-- =====================================================

-- ❌ NO insertar dispositivo hardcodeado
-- Los dispositivos se auto-registrarán usando auto_register_device()

-- =====================================================
-- PARTE 6: VIEWS ÚTEIS PARA INTERFACE WEB (ACTUALIZADAS)
-- =====================================================

-- View para últimos dados dos sensores (por dispositivo)
CREATE OR REPLACE VIEW latest_sensor_data AS
SELECT DISTINCT ON (device_id)
  device_id,
  temperature as env_temperature,
  humidity as env_humidity,
  created_at
FROM environment_data
ORDER BY device_id, created_at DESC;

-- View para últimas medições hidropônicas (por dispositivo)
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

-- =====================================================
-- PARTE 7: TRIGGERS (SIN CAMBIOS)
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
-- PARTE 8: COMENTÁRIOS PARA DOCUMENTAÇÃO
-- =====================================================

COMMENT ON TABLE environment_data IS 'Dados ambientais DHT22 - Múltiples dispositivos';
COMMENT ON TABLE hydro_measurements IS 'Medições hidropônicas - Múltiples dispositivos';
COMMENT ON TABLE relay_commands IS 'Comandos para relés - Múltiples dispositivos';
COMMENT ON TABLE device_status IS 'Status de múltiples dispositivos ESP32 - Auto-registro';

COMMENT ON COLUMN device_status.device_id IS 'ID único generado automáticamente: ESP32_HIDRO_XXXXXX';
COMMENT ON COLUMN device_status.mac_address IS 'MAC address del ESP32 para identificación única';
COMMENT ON FUNCTION auto_register_device IS 'Auto-registro de dispositivos ESP32 con MAC dinámico';

-- =====================================================
-- PARTE 9: VERIFICACIÓN FINAL Y TESTS
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

-- ✅ TESTE da função de auto-registro
SELECT auto_register_device(
    'ESP32_HIDRO_TEST123', 
    '24:6F:28:AB:CD:EF', 
    '192.168.1.100'::INET,
    'ESP32 Test Device',
    'Laboratorio de Pruebas'
);

-- =====================================================
-- SCRIPT VALIDADO PARA MÚLTIPLES DISPOSITIVOS! ✅
-- Compatible with:
-- - Device ID dinámico: ESP32_HIDRO_XXXXXX (basado en MAC)
-- - Auto-registro de dispositivos
-- - Múltiples ESP32 simultáneos
-- - Firmware 2.1.0 
-- - 16 relés PCF8574 (0x20, 0x24)
-- - Sensores: DHT22(15), pH(35), TDS(34), DS18B20(4)
-- - Sistema de descubrimiento automático
-- - Views optimizadas para múltiples dispositivos
-- ===================================================== 
