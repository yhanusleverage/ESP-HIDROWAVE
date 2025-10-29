-- =====================================================
-- SISTEMA DE GESTÃO DE USUÁRIOS + CENTRALIZAÇÃO ESP32
-- Complemento ao sistema hidropônico ESP32
-- Funcionalidade: Associar dispositivos ESP32 a usuários por email
-- =====================================================

-- =====================================================
-- PARTE 1: TABELA DE USUÁRIOS
-- =====================================================

-- 1. ✅ TABELA PRINCIPAL DE USUÁRIOS
CREATE TABLE IF NOT EXISTS users (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  
  -- Dados básicos do usuário
  email TEXT UNIQUE NOT NULL,
  full_name TEXT NOT NULL,
  password_hash TEXT, -- Para autenticação futura (opcional)
  
  -- Configurações do usuário
  timezone TEXT DEFAULT 'America/Sao_Paulo',
  language TEXT DEFAULT 'pt-BR',
  notification_email BOOLEAN DEFAULT true,
  notification_sms BOOLEAN DEFAULT false,
  phone_number TEXT,
  
  -- Plano/Limites
  plan_type TEXT DEFAULT 'free' CHECK (plan_type IN ('free', 'basic', 'pro', 'enterprise')),
  max_devices INTEGER DEFAULT 3, -- Limite de ESP32 por usuário
  max_storage_days INTEGER DEFAULT 30, -- Dias de retenção de dados
  
  -- Status da conta
  is_active BOOLEAN DEFAULT true,
  is_verified BOOLEAN DEFAULT false,
  verification_token TEXT,
  
  -- Metadados
  created_at TIMESTAMPTZ DEFAULT NOW(),
  updated_at TIMESTAMPTZ DEFAULT NOW(),
  last_login_at TIMESTAMPTZ,
  
  -- Campos para integração
  supabase_user_id UUID, -- Se usar Supabase Auth
  external_id TEXT -- Para integração com outros sistemas
);

-- Índices para performance
CREATE INDEX idx_users_email ON users(email);
CREATE INDEX idx_users_active ON users(is_active);
CREATE INDEX idx_users_plan ON users(plan_type);
CREATE INDEX idx_users_supabase_id ON users(supabase_user_id);

-- 2. ✅ TABELA DE ASSOCIAÇÃO USUÁRIO-DISPOSITIVO
CREATE TABLE IF NOT EXISTS user_devices (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  
  -- Relacionamentos
  user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  device_id TEXT NOT NULL, -- Referência ao device_status.device_id
  
  -- Configurações específicas do dispositivo para o usuário
  device_alias TEXT, -- Nome personalizado pelo usuário
  device_description TEXT,
  location_custom TEXT, -- Localização personalizada
  
  -- Permissões
  can_control BOOLEAN DEFAULT true, -- Pode controlar relés
  can_view_data BOOLEAN DEFAULT true, -- Pode ver dados dos sensores
  can_configure BOOLEAN DEFAULT true, -- Pode alterar configurações
  
  -- Status da associação
  is_active BOOLEAN DEFAULT true,
  is_primary_owner BOOLEAN DEFAULT false, -- Um dispositivo pode ter um dono principal
  
  -- Configurações de notificação
  notify_offline BOOLEAN DEFAULT true,
  notify_alerts BOOLEAN DEFAULT true,
  notify_maintenance BOOLEAN DEFAULT false,
  
  -- Metadados
  associated_at TIMESTAMPTZ DEFAULT NOW(),
  last_access_at TIMESTAMPTZ,
  
  -- Constraint: um dispositivo não pode estar associado ao mesmo usuário duas vezes
  UNIQUE(user_id, device_id)
);

-- Índices para queries rápidas
CREATE INDEX idx_user_devices_user_id ON user_devices(user_id);
CREATE INDEX idx_user_devices_device_id ON user_devices(device_id);
CREATE INDEX idx_user_devices_active ON user_devices(is_active);
CREATE INDEX idx_user_devices_primary ON user_devices(is_primary_owner) WHERE is_primary_owner = true;

-- 3. ✅ TABELA DE CONVITES/COMPARTILHAMENTO
CREATE TABLE IF NOT EXISTS device_invitations (
  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  
  -- Dados do convite
  inviter_user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  invitee_email TEXT NOT NULL,
  device_id TEXT NOT NULL,
  
  -- Permissões oferecidas
  permissions JSONB DEFAULT '{"can_control": false, "can_view_data": true, "can_configure": false}',
  
  -- Status do convite
  status TEXT DEFAULT 'pending' CHECK (status IN ('pending', 'accepted', 'declined', 'expired')),
  invitation_token TEXT UNIQUE NOT NULL,
  
  -- Metadados
  created_at TIMESTAMPTZ DEFAULT NOW(),
  expires_at TIMESTAMPTZ DEFAULT NOW() + INTERVAL '7 days',
  accepted_at TIMESTAMPTZ,
  
  -- Mensagem personalizada
  message TEXT
);

-- Índices para convites
CREATE INDEX idx_invitations_invitee_email ON device_invitations(invitee_email);
CREATE INDEX idx_invitations_device_id ON device_invitations(device_id);
CREATE INDEX idx_invitations_status ON device_invitations(status);
CREATE INDEX idx_invitations_token ON device_invitations(invitation_token);

-- =====================================================
-- PARTE 2: FUNÇÕES DE GESTÃO DE USUÁRIOS
-- =====================================================

-- 4. ✅ FUNÇÃO: Registrar novo usuário
CREATE OR REPLACE FUNCTION register_user(
    p_email TEXT,
    p_full_name TEXT,
    p_plan_type TEXT DEFAULT 'free',
    p_supabase_user_id UUID DEFAULT NULL
)
RETURNS BIGINT AS $$
DECLARE
    user_id BIGINT;
    max_devices_limit INTEGER;
    storage_days_limit INTEGER;
BEGIN
    -- Validar email
    IF p_email !~ '^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$' THEN
        RAISE EXCEPTION 'Email inválido: %', p_email;
    END IF;
    
    -- Definir limites baseado no plano
    CASE p_plan_type
        WHEN 'free' THEN 
            max_devices_limit := 3;
            storage_days_limit := 30;
        WHEN 'basic' THEN 
            max_devices_limit := 10;
            storage_days_limit := 90;
        WHEN 'pro' THEN 
            max_devices_limit := 50;
            storage_days_limit := 365;
        WHEN 'enterprise' THEN 
            max_devices_limit := 999;
            storage_days_limit := 1095; -- 3 anos
        ELSE
            RAISE EXCEPTION 'Plano inválido: %', p_plan_type;
    END CASE;
    
    -- Inserir usuário
    INSERT INTO users (
        email, full_name, plan_type, max_devices, max_storage_days, 
        supabase_user_id, verification_token
    ) VALUES (
        LOWER(TRIM(p_email)), 
        TRIM(p_full_name), 
        p_plan_type, 
        max_devices_limit, 
        storage_days_limit,
        p_supabase_user_id,
        encode(gen_random_bytes(32), 'hex') -- Token de verificação
    ) RETURNING id INTO user_id;
    
    RAISE NOTICE 'Usuário registrado: % (ID: %)', p_email, user_id;
    RETURN user_id;
END;
$$ LANGUAGE plpgsql;

-- 5. ✅ FUNÇÃO: Associar dispositivo a usuário
CREATE OR REPLACE FUNCTION associate_device_to_user(
    p_user_email TEXT,
    p_device_id TEXT,
    p_device_alias TEXT DEFAULT NULL,
    p_is_primary_owner BOOLEAN DEFAULT true
)
RETURNS BIGINT AS $$
DECLARE
    user_record RECORD;
    device_count INTEGER;
    association_id BIGINT;
BEGIN
    -- Buscar usuário
    SELECT id, max_devices, plan_type INTO user_record
    FROM users 
    WHERE email = LOWER(TRIM(p_user_email)) AND is_active = true;
    
    IF NOT FOUND THEN
        RAISE EXCEPTION 'Usuário não encontrado ou inativo: %', p_user_email;
    END IF;
    
    -- Verificar se dispositivo existe
    IF NOT EXISTS (SELECT 1 FROM device_status WHERE device_id = p_device_id) THEN
        RAISE EXCEPTION 'Dispositivo não encontrado: %', p_device_id;
    END IF;
    
    -- Verificar limite de dispositivos
    SELECT COUNT(*) INTO device_count
    FROM user_devices 
    WHERE user_id = user_record.id AND is_active = true;
    
    IF device_count >= user_record.max_devices THEN
        RAISE EXCEPTION 'Limite de dispositivos atingido para plano %: % dispositivos', 
                        user_record.plan_type, user_record.max_devices;
    END IF;
    
    -- Associar dispositivo
    INSERT INTO user_devices (
        user_id, device_id, device_alias, is_primary_owner,
        can_control, can_view_data, can_configure
    ) VALUES (
        user_record.id, p_device_id, 
        COALESCE(p_device_alias, 'Meu ESP32 ' || SUBSTRING(p_device_id, -4)),
        p_is_primary_owner,
        true, true, p_is_primary_owner -- Primary owner tem todas as permissões
    ) ON CONFLICT (user_id, device_id) DO UPDATE SET
        is_active = true,
        device_alias = COALESCE(EXCLUDED.device_alias, user_devices.device_alias),
        is_primary_owner = EXCLUDED.is_primary_owner OR user_devices.is_primary_owner
    RETURNING id INTO association_id;
    
    RAISE NOTICE 'Dispositivo % associado ao usuário % (ID: %)', p_device_id, p_user_email, association_id;
    RETURN association_id;
END;
$$ LANGUAGE plpgsql;

-- 6. ✅ FUNÇÃO: Listar dispositivos do usuário
CREATE OR REPLACE FUNCTION get_user_devices(p_user_email TEXT)
RETURNS TABLE (
    device_id TEXT,
    device_alias TEXT,
    device_name TEXT,
    location TEXT,
    is_online BOOLEAN,
    last_seen TIMESTAMPTZ,
    is_primary_owner BOOLEAN,
    can_control BOOLEAN,
    active_relays INTEGER,
    health_score INTEGER
) AS $$
BEGIN
    RETURN QUERY
    SELECT 
        ud.device_id,
        ud.device_alias,
        ds.device_name,
        COALESCE(ud.location_custom, ds.location) as location,
        ds.is_online,
        ds.last_seen,
        ud.is_primary_owner,
        ud.can_control,
        (SELECT COUNT(*) FROM unnest(ds.relay_states) AS state WHERE state = true)::INTEGER as active_relays,
        CASE 
            WHEN NOT ds.is_online THEN 0
            WHEN ds.last_seen < NOW() - INTERVAL '10 minutes' THEN 25
            WHEN ds.free_heap < 20000 THEN 30
            WHEN ds.wifi_rssi < -80 THEN 40
            WHEN ds.free_heap < 50000 THEN 80
            ELSE 100
        END::INTEGER as health_score
    FROM user_devices ud
    JOIN users u ON ud.user_id = u.id
    LEFT JOIN device_status ds ON ud.device_id = ds.device_id
    WHERE u.email = LOWER(TRIM(p_user_email))
      AND u.is_active = true
      AND ud.is_active = true
    ORDER BY ud.is_primary_owner DESC, ds.last_seen DESC;
END;
$$ LANGUAGE plpgsql;

-- 7. ✅ FUNÇÃO: Verificar permissões do usuário
CREATE OR REPLACE FUNCTION check_user_device_permission(
    p_user_email TEXT,
    p_device_id TEXT,
    p_permission TEXT -- 'control', 'view', 'configure'
)
RETURNS BOOLEAN AS $$
DECLARE
    has_permission BOOLEAN := false;
BEGIN
    SELECT 
        CASE p_permission
            WHEN 'control' THEN ud.can_control
            WHEN 'view' THEN ud.can_view_data
            WHEN 'configure' THEN ud.can_configure
            ELSE false
        END INTO has_permission
    FROM user_devices ud
    JOIN users u ON ud.user_id = u.id
    WHERE u.email = LOWER(TRIM(p_user_email))
      AND ud.device_id = p_device_id
      AND u.is_active = true
      AND ud.is_active = true;
    
    RETURN COALESCE(has_permission, false);
END;
$$ LANGUAGE plpgsql;

-- 8. ✅ FUNÇÃO: Criar convite para compartilhar dispositivo
CREATE OR REPLACE FUNCTION invite_user_to_device(
    p_owner_email TEXT,
    p_invitee_email TEXT,
    p_device_id TEXT,
    p_can_control BOOLEAN DEFAULT false,
    p_message TEXT DEFAULT NULL
)
RETURNS TEXT AS $$
DECLARE
    owner_id BIGINT;
    invitation_token TEXT;
BEGIN
    -- Verificar se o usuário é dono do dispositivo
    SELECT u.id INTO owner_id
    FROM users u
    JOIN user_devices ud ON u.id = ud.user_id
    WHERE u.email = LOWER(TRIM(p_owner_email))
      AND ud.device_id = p_device_id
      AND ud.is_primary_owner = true
      AND u.is_active = true
      AND ud.is_active = true;
    
    IF NOT FOUND THEN
        RAISE EXCEPTION 'Usuário % não é dono do dispositivo %', p_owner_email, p_device_id;
    END IF;
    
    -- Gerar token único
    invitation_token := encode(gen_random_bytes(32), 'hex');
    
    -- Criar convite
    INSERT INTO device_invitations (
        inviter_user_id, invitee_email, device_id, invitation_token,
        permissions, message
    ) VALUES (
        owner_id, LOWER(TRIM(p_invitee_email)), p_device_id, invitation_token,
        json_build_object(
            'can_control', p_can_control,
            'can_view_data', true,
            'can_configure', false
        )::jsonb,
        p_message
    );
    
    RAISE NOTICE 'Convite criado para % acessar dispositivo %', p_invitee_email, p_device_id;
    RETURN invitation_token;
END;
$$ LANGUAGE plpgsql;

-- 9. ✅ FUNÇÃO: Aceitar convite
CREATE OR REPLACE FUNCTION accept_device_invitation(
    p_invitation_token TEXT,
    p_invitee_email TEXT
)
RETURNS BIGINT AS $$
DECLARE
    invitation_record RECORD;
    user_id BIGINT;
    association_id BIGINT;
BEGIN
    -- Buscar convite válido
    SELECT * INTO invitation_record
    FROM device_invitations
    WHERE invitation_token = p_invitation_token
      AND invitee_email = LOWER(TRIM(p_invitee_email))
      AND status = 'pending'
      AND expires_at > NOW();
    
    IF NOT FOUND THEN
        RAISE EXCEPTION 'Convite inválido ou expirado';
    END IF;
    
    -- Buscar ou criar usuário
    SELECT id INTO user_id FROM users WHERE email = LOWER(TRIM(p_invitee_email));
    
    IF NOT FOUND THEN
        -- Auto-registrar usuário se não existir
        user_id := register_user(p_invitee_email, split_part(p_invitee_email, '@', 1));
    END IF;
    
    -- Associar dispositivo com as permissões do convite
    INSERT INTO user_devices (
        user_id, device_id, is_primary_owner,
        can_control, can_view_data, can_configure
    ) VALUES (
        user_id, invitation_record.device_id, false,
        (invitation_record.permissions->>'can_control')::boolean,
        (invitation_record.permissions->>'can_view_data')::boolean,
        (invitation_record.permissions->>'can_configure')::boolean
    ) ON CONFLICT (user_id, device_id) DO UPDATE SET
        can_control = EXCLUDED.can_control OR user_devices.can_control,
        can_view_data = EXCLUDED.can_view_data OR user_devices.can_view_data,
        can_configure = EXCLUDED.can_configure OR user_devices.can_configure,
        is_active = true
    RETURNING id INTO association_id;
    
    -- Marcar convite como aceito
    UPDATE device_invitations 
    SET status = 'accepted', accepted_at = NOW()
    WHERE id = invitation_record.id;
    
    RAISE NOTICE 'Convite aceito - Usuário % agora tem acesso ao dispositivo %', 
                 p_invitee_email, invitation_record.device_id;
    RETURN association_id;
END;
$$ LANGUAGE plpgsql;

-- =====================================================
-- PARTE 3: VIEWS PARA DASHBOARD DE USUÁRIOS
-- =====================================================

-- 10. ✅ VIEW: Dashboard do usuário
CREATE OR REPLACE VIEW user_dashboard AS
SELECT 
    u.id as user_id,
    u.email,
    u.full_name,
    u.plan_type,
    u.max_devices,
    
    -- Estatísticas dos dispositivos
    COUNT(ud.device_id) as total_devices,
    COUNT(CASE WHEN ds.is_online = true THEN 1 END) as online_devices,
    COUNT(CASE WHEN ud.is_primary_owner = true THEN 1 END) as owned_devices,
    COUNT(CASE WHEN ud.is_primary_owner = false THEN 1 END) as shared_devices,
    
    -- Estatísticas de atividade (últimas 24h)
    COALESCE(activity.total_commands, 0) as commands_24h,
    COALESCE(activity.sensor_readings, 0) as sensor_readings_24h,
    
    -- Status da conta
    u.is_active,
    u.is_verified,
    u.last_login_at,
    u.created_at as member_since
    
FROM users u
LEFT JOIN user_devices ud ON u.id = ud.user_id AND ud.is_active = true
LEFT JOIN device_status ds ON ud.device_id = ds.device_id
LEFT JOIN (
    -- Atividade das últimas 24h
    SELECT 
        u.id as user_id,
        COUNT(DISTINCT rc.id) as total_commands,
        COUNT(DISTINCT ed.id) + COUNT(DISTINCT hm.id) as sensor_readings
    FROM users u
    JOIN user_devices ud ON u.id = ud.user_id
    LEFT JOIN relay_commands rc ON ud.device_id = rc.device_id 
        AND rc.created_at > NOW() - INTERVAL '24 hours'
    LEFT JOIN environment_data ed ON ud.device_id = ed.device_id 
        AND ed.created_at > NOW() - INTERVAL '24 hours'
    LEFT JOIN hydro_measurements hm ON ud.device_id = hm.device_id 
        AND hm.created_at > NOW() - INTERVAL '24 hours'
    GROUP BY u.id
) activity ON u.id = activity.user_id
WHERE u.is_active = true
GROUP BY u.id, u.email, u.full_name, u.plan_type, u.max_devices, 
         u.is_active, u.is_verified, u.last_login_at, u.created_at,
         activity.total_commands, activity.sensor_readings;

-- 11. ✅ VIEW: Dispositivos compartilhados
CREATE OR REPLACE VIEW shared_devices_view AS
SELECT 
    ds.device_id,
    ds.device_name,
    ds.location,
    ds.is_online,
    
    -- Dono principal
    owner.email as owner_email,
    owner.full_name as owner_name,
    
    -- Usuários com acesso
    json_agg(
        json_build_object(
            'email', shared_user.email,
            'name', shared_user.full_name,
            'can_control', ud.can_control,
            'can_view_data', ud.can_view_data,
            'can_configure', ud.can_configure,
            'associated_at', ud.associated_at
        ) ORDER BY ud.associated_at
    ) as shared_users
    
FROM device_status ds
JOIN user_devices ud_owner ON ds.device_id = ud_owner.device_id AND ud_owner.is_primary_owner = true
JOIN users owner ON ud_owner.user_id = owner.id
LEFT JOIN user_devices ud ON ds.device_id = ud.device_id AND ud.is_primary_owner = false AND ud.is_active = true
LEFT JOIN users shared_user ON ud.user_id = shared_user.id AND shared_user.is_active = true
GROUP BY ds.device_id, ds.device_name, ds.location, ds.is_online, 
         owner.email, owner.full_name
HAVING COUNT(ud.user_id) > 0; -- Apenas dispositivos que têm compartilhamento

-- =====================================================
-- PARTE 4: POLÍTICAS RLS ATUALIZADAS
-- =====================================================

-- 12. ✅ RLS para tabelas de usuários
ALTER TABLE users ENABLE ROW LEVEL SECURITY;
ALTER TABLE user_devices ENABLE ROW LEVEL SECURITY;
ALTER TABLE device_invitations ENABLE ROW LEVEL SECURITY;

-- Políticas básicas para desenvolvimento (ajustar para produção)
CREATE POLICY "Enable read for anon" ON users FOR SELECT USING (true);
CREATE POLICY "Enable insert for anon" ON users FOR INSERT WITH CHECK (true);
CREATE POLICY "Enable update for anon" ON users FOR UPDATE USING (true);

CREATE POLICY "Enable read for anon" ON user_devices FOR SELECT USING (true);
CREATE POLICY "Enable insert for anon" ON user_devices FOR INSERT WITH CHECK (true);
CREATE POLICY "Enable update for anon" ON user_devices FOR UPDATE USING (true);

CREATE POLICY "Enable read for anon" ON device_invitations FOR SELECT USING (true);
CREATE POLICY "Enable insert for anon" ON device_invitations FOR INSERT WITH CHECK (true);
CREATE POLICY "Enable update for anon" ON device_invitations FOR UPDATE USING (true);

-- =====================================================
-- PARTE 5: TRIGGERS E AUTOMAÇÃO
-- =====================================================

-- 13. ✅ TRIGGER: Auto-update do updated_at
CREATE OR REPLACE FUNCTION update_updated_at_users()
RETURNS TRIGGER AS $$
BEGIN
  NEW.updated_at = NOW();
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER update_users_updated_at
  BEFORE UPDATE ON users
  FOR EACH ROW
  EXECUTE FUNCTION update_updated_at_users();

-- 14. ✅ TRIGGER: Log de acesso ao dispositivo
CREATE OR REPLACE FUNCTION log_device_access()
RETURNS TRIGGER AS $$
BEGIN
  NEW.last_access_at = NOW();
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER log_user_device_access
  BEFORE UPDATE ON user_devices
  FOR EACH ROW
  EXECUTE FUNCTION log_device_access();

-- =====================================================
-- PARTE 6: DADOS DE TESTE E VALIDAÇÃO
-- =====================================================

-- 15. ✅ FUNÇÃO: Criar dados de teste
CREATE OR REPLACE FUNCTION create_test_users()
RETURNS TEXT AS $$
DECLARE
    user1_id BIGINT;
    user2_id BIGINT;
    invitation_token TEXT;
BEGIN
    -- Criar usuários de teste
    user1_id := register_user('joao@hidroponia.com', 'João Silva', 'pro');
    user2_id := register_user('maria@hidroponia.com', 'Maria Santos', 'basic');
    
    -- Associar dispositivos (assumindo que existem)
    IF EXISTS (SELECT 1 FROM device_status LIMIT 1) THEN
        PERFORM associate_device_to_user('joao@hidroponia.com', 
                                       (SELECT device_id FROM device_status LIMIT 1),
                                       'Estufa Principal', true);
        
        -- Criar convite de compartilhamento
        invitation_token := invite_user_to_device('joao@hidroponia.com', 
                                                 'maria@hidroponia.com',
                                                 (SELECT device_id FROM device_status LIMIT 1),
                                                 false, 'Acesso à estufa para monitoramento');
        
        RETURN format('Usuários criados! Token de convite: %s', invitation_token);
    ELSE
        RETURN 'Usuários criados, mas nenhum dispositivo encontrado para associar';
    END IF;
END;
$$ LANGUAGE plpgsql;

-- =====================================================
-- PARTE 7: COMENTÁRIOS E DOCUMENTAÇÃO
-- =====================================================

COMMENT ON TABLE users IS 'Usuários do sistema hidropônico - gestão centralizada por email';
COMMENT ON TABLE user_devices IS 'Associação entre usuários e dispositivos ESP32 com permissões';
COMMENT ON TABLE device_invitations IS 'Sistema de convites para compartilhar dispositivos';

COMMENT ON FUNCTION register_user IS 'Registrar novo usuário com limites baseados no plano';
COMMENT ON FUNCTION associate_device_to_user IS 'Associar dispositivo ESP32 a usuário específico';
COMMENT ON FUNCTION get_user_devices IS 'Listar todos os dispositivos de um usuário';
COMMENT ON FUNCTION check_user_device_permission IS 'Verificar permissões específicas do usuário';
COMMENT ON FUNCTION invite_user_to_device IS 'Criar convite para compartilhar dispositivo';
COMMENT ON FUNCTION accept_device_invitation IS 'Aceitar convite e obter acesso ao dispositivo';

COMMENT ON VIEW user_dashboard IS 'Dashboard completo do usuário com estatísticas';
COMMENT ON VIEW shared_devices_view IS 'Visão de dispositivos compartilhados entre usuários';

-- =====================================================
-- SISTEMA DE USUÁRIOS IMPLEMENTADO! ✅
-- 
-- Funcionalidades:
-- ✅ Registro de usuários por email
-- ✅ Planos com limites diferentes (free, basic, pro, enterprise)
-- ✅ Associação de ESP32 a usuários específicos
-- ✅ Sistema de permissões granulares
-- ✅ Compartilhamento de dispositivos via convites
-- ✅ Dashboard personalizado por usuário
-- ✅ Views para gestão multi-usuário
-- ✅ Triggers para logs e automação
-- ✅ Políticas RLS para segurança
-- 
-- Aplicabilidades:
-- 📧 Centralização por email de cada usuário
-- 🔐 Controle de acesso por dispositivo
-- 👥 Compartilhamento seguro entre usuários
-- 📊 Dashboard personalizado
-- 💼 Planos comerciais com limites
-- 🎯 Sistema escalável para múltiplos usuários
-- =====================================================
