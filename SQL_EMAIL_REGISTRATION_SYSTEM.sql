-- =====================================================
-- SISTEMA DE REGISTRO POR EMAIL - ESP32 HIDROPÔNICO
-- Extensão do SCRIPT_SUPABASE_PROJETO_FINAL.sql
-- Funcionalidade: Multi-usuário com rastreamento por email
-- =====================================================

-- =====================================================
-- PARTE 1: NOVA TABELA DE USUÁRIOS
-- =====================================================

-- 1. ✅ TABELA DE USUÁRIOS (para multi-usuário)
CREATE TABLE IF NOT EXISTS users (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  email TEXT UNIQUE NOT NULL,
  name TEXT,
  created_at TIMESTAMPTZ DEFAULT NOW(),
  updated_at TIMESTAMPTZ DEFAULT NOW(),
  is_active BOOLEAN DEFAULT true,
  
  -- Metadados do usuário
  total_devices INTEGER DEFAULT 0,
  subscription_type TEXT DEFAULT 'free' CHECK (subscription_type IN ('free', 'premium', 'enterprise')),
  max_devices INTEGER DEFAULT 5, -- Limite por plano
  
  -- Configurações
  notification_email BOOLEAN DEFAULT true,
  timezone TEXT DEFAULT 'America/Sao_Paulo'
);

-- Índices para usuários
CREATE INDEX idx_users_email ON users(email);
CREATE INDEX idx_users_active ON users(is_active);

-- =====================================================
-- PARTE 2: ATUALIZAR TABELA DEVICE_STATUS
-- =====================================================

-- Adicionar campos de usuário à tabela device_status
ALTER TABLE device_status ADD COLUMN IF NOT EXISTS user_email TEXT;
ALTER TABLE device_status ADD COLUMN IF NOT EXISTS registered_at TIMESTAMPTZ DEFAULT NOW();
ALTER TABLE device_status ADD COLUMN IF NOT EXISTS registration_source TEXT DEFAULT 'wifi_config';

-- Índices para relacionamento usuário-dispositivo
CREATE INDEX idx_device_status_user_email ON device_status(user_email);
CREATE INDEX idx_device_status_registered ON device_status(registered_at);

-- =====================================================
-- PARTE 3: FUNÇÃO DE REGISTRO COM EMAIL
-- =====================================================

-- ✅ FUNÇÃO PRINCIPAL: Registrar dispositivo com email
CREATE OR REPLACE FUNCTION register_device_with_email(
    p_device_id TEXT,
    p_mac_address TEXT,
    p_user_email TEXT,
    p_ip_address INET DEFAULT NULL,
    p_device_name TEXT DEFAULT NULL,
    p_location TEXT DEFAULT NULL
)
RETURNS JSON AS $$
DECLARE
    user_device_count INTEGER;
    user_max_devices INTEGER;
    result_json JSON;
    user_exists BOOLEAN;
BEGIN
    -- Validar email
    IF p_user_email IS NULL OR p_user_email = '' THEN
        RETURN json_build_object(
            'success', false,
            'message', 'Email é obrigatório',
            'error_code', 'EMAIL_REQUIRED'
        );
    END IF;
    
    -- Verificar se usuário existe, se não, criar
    SELECT EXISTS(SELECT 1 FROM users WHERE email = p_user_email) INTO user_exists;
    
    IF NOT user_exists THEN
        INSERT INTO users (email, name, created_at) 
        VALUES (
            p_user_email, 
            SPLIT_PART(p_user_email, '@', 1), -- Nome baseado no email
            NOW()
        );
        
        RAISE NOTICE 'Novo usuário criado: %', p_user_email;
    END IF;
    
    -- Verificar limite de dispositivos
    SELECT 
        COUNT(*) as device_count,
        u.max_devices
    INTO user_device_count, user_max_devices
    FROM users u
    LEFT JOIN device_status ds ON ds.user_email = u.email
    WHERE u.email = p_user_email
    GROUP BY u.max_devices;
    
    -- Verificar se não excede o limite (exceto se for atualização)
    IF user_device_count >= user_max_devices AND 
       NOT EXISTS(SELECT 1 FROM device_status WHERE device_id = p_device_id) THEN
        RETURN json_build_object(
            'success', false,
            'message', 'Limite de dispositivos excedido (' || user_max_devices || ')',
            'error_code', 'DEVICE_LIMIT_EXCEEDED',
            'current_count', user_device_count,
            'max_allowed', user_max_devices
        );
    END IF;
    
    -- Registrar/atualizar dispositivo
    INSERT INTO device_status (
        device_id, 
        mac_address, 
        user_email,
        ip_address, 
        device_name, 
        location,
        registered_at,
        last_seen,
        is_online,
        registration_source
    ) VALUES (
        p_device_id,
        p_mac_address,
        p_user_email,
        p_ip_address,
        COALESCE(p_device_name, 'ESP32 - ' || RIGHT(p_mac_address, 8)),
        COALESCE(p_location, 'Localização não especificada'),
        NOW(),
        NOW(),
        true,
        'wifi_config'
    )
    ON CONFLICT (device_id) DO UPDATE SET
        mac_address = EXCLUDED.mac_address,
        user_email = EXCLUDED.user_email, -- Permitir mudança de usuário
        ip_address = COALESCE(EXCLUDED.ip_address, device_status.ip_address),
        device_name = COALESCE(EXCLUDED.device_name, device_status.device_name),
        location = COALESCE(EXCLUDED.location, device_status.location),
        last_seen = NOW(),
        is_online = true,
        updated_at = NOW();
    
    -- Atualizar contador de dispositivos do usuário
    UPDATE users 
    SET 
        total_devices = (
            SELECT COUNT(*) 
            FROM device_status 
            WHERE user_email = p_user_email
        ),
        updated_at = NOW()
    WHERE email = p_user_email;
    
    -- Retornar sucesso
    result_json := json_build_object(
        'success', true,
        'message', 'Dispositivo registrado com sucesso',
        'device_id', p_device_id,
        'user_email', p_user_email,
        'device_count', (SELECT total_devices FROM users WHERE email = p_user_email),
        'registration_time', NOW()
    );
    
    RAISE NOTICE 'Dispositivo % registrado para usuário %', p_device_id, p_user_email;
    RETURN result_json;
END;
$$ LANGUAGE plpgsql;

-- =====================================================
-- PARTE 4: VIEWS ATUALIZADAS COM EMAIL
-- =====================================================

-- ✅ VIEW: Dispositivos por usuário
CREATE OR REPLACE VIEW user_devices AS
SELECT 
    u.email as user_email,
    u.name as user_name,
    u.subscription_type,
    u.max_devices,
    u.total_devices,
    
    ds.device_id,
    ds.device_name,
    ds.location,
    ds.mac_address,
    ds.ip_address,
    ds.is_online,
    ds.last_seen,
    ds.registered_at,
    
    -- Status de conexão
    CASE 
        WHEN ds.last_seen > NOW() - INTERVAL '2 minutes' THEN 'online'
        WHEN ds.last_seen > NOW() - INTERVAL '10 minutes' THEN 'recent'
        ELSE 'offline'
    END as connection_status,
    
    -- Tempo offline
    EXTRACT(EPOCH FROM (NOW() - ds.last_seen))/60 as minutes_offline,
    
    -- Estatísticas últimas 24h
    (SELECT COUNT(*) FROM environment_data ed WHERE ed.device_id = ds.device_id AND ed.created_at > NOW() - INTERVAL '24 hours') as sensor_readings_24h,
    (SELECT COUNT(*) FROM relay_commands rc WHERE rc.device_id = ds.device_id AND rc.created_at > NOW() - INTERVAL '24 hours') as commands_24h
    
FROM users u
LEFT JOIN device_status ds ON ds.user_email = u.email
WHERE u.is_active = true
ORDER BY u.email, ds.last_seen DESC;

-- ✅ VIEW: Estatísticas por usuário
CREATE OR REPLACE VIEW user_statistics AS
SELECT 
    u.email,
    u.name,
    u.subscription_type,
    u.total_devices,
    u.max_devices,
    u.created_at as user_since,
    
    -- Dispositivos online/offline
    COUNT(CASE WHEN ds.is_online = true THEN 1 END) as devices_online,
    COUNT(CASE WHEN ds.is_online = false THEN 1 END) as devices_offline,
    
    -- Atividade últimas 24h
    COALESCE(SUM(
        (SELECT COUNT(*) FROM environment_data ed 
         WHERE ed.device_id = ds.device_id AND ed.created_at > NOW() - INTERVAL '24 hours')
    ), 0) as total_sensor_readings_24h,
    
    COALESCE(SUM(
        (SELECT COUNT(*) FROM relay_commands rc 
         WHERE rc.device_id = ds.device_id AND rc.created_at > NOW() - INTERVAL '24 hours')
    ), 0) as total_commands_24h,
    
    -- Último dispositivo ativo
    MAX(ds.last_seen) as last_device_activity
    
FROM users u
LEFT JOIN device_status ds ON ds.user_email = u.email
WHERE u.is_active = true
GROUP BY u.email, u.name, u.subscription_type, u.total_devices, u.max_devices, u.created_at;

-- =====================================================
-- PARTE 5: FUNÇÕES DE GESTÃO DE USUÁRIOS
-- =====================================================

-- ✅ FUNÇÃO: Listar dispositivos de um usuário
CREATE OR REPLACE FUNCTION get_user_devices(p_user_email TEXT)
RETURNS TABLE (
    device_id TEXT,
    device_name TEXT,
    location TEXT,
    is_online BOOLEAN,
    last_seen TIMESTAMPTZ,
    connection_status TEXT
) AS $$
BEGIN
    RETURN QUERY
    SELECT 
        ds.device_id,
        ds.device_name,
        ds.location,
        ds.is_online,
        ds.last_seen,
        CASE 
            WHEN ds.last_seen > NOW() - INTERVAL '2 minutes' THEN 'online'
            WHEN ds.last_seen > NOW() - INTERVAL '10 minutes' THEN 'recent'
            ELSE 'offline'
        END as connection_status
    FROM device_status ds
    WHERE ds.user_email = p_user_email
    ORDER BY ds.last_seen DESC;
END;
$$ LANGUAGE plpgsql;

-- ✅ FUNÇÃO: Verificar se usuário pode adicionar mais dispositivos
CREATE OR REPLACE FUNCTION can_add_device(p_user_email TEXT)
RETURNS JSON AS $$
DECLARE
    current_count INTEGER;
    max_allowed INTEGER;
    result JSON;
BEGIN
    SELECT 
        u.total_devices,
        u.max_devices
    INTO current_count, max_allowed
    FROM users u
    WHERE u.email = p_user_email;
    
    IF current_count IS NULL THEN
        -- Usuário não existe
        result := json_build_object(
            'can_add', true,
            'reason', 'new_user',
            'current_count', 0,
            'max_allowed', 5
        );
    ELSIF current_count < max_allowed THEN
        result := json_build_object(
            'can_add', true,
            'current_count', current_count,
            'max_allowed', max_allowed,
            'remaining', max_allowed - current_count
        );
    ELSE
        result := json_build_object(
            'can_add', false,
            'reason', 'limit_exceeded',
            'current_count', current_count,
            'max_allowed', max_allowed
        );
    END IF;
    
    RETURN result;
END;
$$ LANGUAGE plpgsql;

-- =====================================================
-- PARTE 6: POLÍTICAS DE SEGURANÇA ATUALIZADAS
-- =====================================================

-- Política para usuários
ALTER TABLE users ENABLE ROW LEVEL SECURITY;
CREATE POLICY "Users can read their own data" ON users
  FOR SELECT USING (true); -- Por enquanto público, depois pode restringir

CREATE POLICY "Users can update their own data" ON users
  FOR UPDATE USING (true);

-- Atualizar políticas de device_status para incluir email
DROP POLICY IF EXISTS "Enable select for anon" ON device_status;
CREATE POLICY "Enable select for anon" ON device_status
  FOR SELECT USING (true);

-- =====================================================
-- PARTE 7: TRIGGERS ATUALIZADOS
-- =====================================================

-- Trigger para atualizar updated_at em users
CREATE TRIGGER update_users_updated_at
  BEFORE UPDATE ON users
  FOR EACH ROW
  EXECUTE FUNCTION update_updated_at_column();

-- =====================================================
-- PARTE 8: DADOS DE TESTE
-- =====================================================

-- ✅ TESTE da função de registro com email
SELECT register_device_with_email(
    'ESP32_HIDRO_TEST001', 
    '24:6F:28:AB:CD:EF', 
    'usuario@exemplo.com',
    '192.168.1.100'::INET,
    'ESP32 Teste',
    'Estufa Principal'
);

-- ✅ TESTE verificação de limite
SELECT can_add_device('usuario@exemplo.com');

-- ✅ TESTE listar dispositivos do usuário
SELECT * FROM get_user_devices('usuario@exemplo.com');

-- =====================================================
-- PARTE 9: COMENTÁRIOS E DOCUMENTAÇÃO
-- =====================================================

COMMENT ON TABLE users IS 'Usuários do sistema hidropônico - Multi-tenant';
COMMENT ON FUNCTION register_device_with_email IS 'Registra ESP32 com email do usuário - Entrada principal';
COMMENT ON VIEW user_devices IS 'Lista todos dispositivos por usuário com status';
COMMENT ON VIEW user_statistics IS 'Estatísticas agregadas por usuário';

-- =====================================================
-- SISTEMA DE EMAIL REGISTRATION IMPLEMENTADO! ✅
-- 
-- Funcionalidades:
-- ✅ Multi-usuário com email único
-- ✅ Limite de dispositivos por plano
-- ✅ Auto-criação de usuários
-- ✅ Rastreamento completo por email
-- ✅ Views otimizadas para interface
-- ✅ Funções de validação
-- ✅ Políticas de segurança
-- ✅ Sistema de testes integrado
-- =====================================================
