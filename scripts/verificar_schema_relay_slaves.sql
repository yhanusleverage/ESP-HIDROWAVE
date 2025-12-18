-- =====================================================
-- SCRIPT: VERIFICAR Y CORRIGIR SCHEMA PARA relay_slaves
-- =====================================================
-- Objetivo: Verificar que todas las tablas y campos necesários existam
-- =====================================================

-- =====================================================
-- 1. VERIFICAR SI device_status TIENE user_email
-- =====================================================

SELECT 
    column_name,
    data_type,
    is_nullable,
    column_default
FROM information_schema.columns
WHERE table_name = 'device_status'
  AND column_name = 'user_email';

-- Si no existe, agregar:
-- ALTER TABLE device_status ADD COLUMN IF NOT EXISTS user_email TEXT;

-- =====================================================
-- 2. VERIFICAR SI relay_slaves EXISTE
-- =====================================================

SELECT 
    table_name,
    table_type
FROM information_schema.tables
WHERE table_name = 'relay_slaves';

-- =====================================================
-- 3. CREAR TABLA relay_slaves SI NO EXISTE
-- =====================================================

CREATE TABLE IF NOT EXISTS relay_slaves (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    
    -- ✅ CAMPOS OBLIGATORIOS
    device_id TEXT NOT NULL,                    -- ESP32_SLAVE_XX_XX_XX_XX_XX_XX
    user_email TEXT NOT NULL,                   -- Email del usuario (OBLIGATORIO)
    master_device_id TEXT NOT NULL,            -- ESP32_HIDRO_XXXXXX
    master_mac_address TEXT NOT NULL,           -- MAC del master
    slave_mac_address TEXT NOT NULL,           -- MAC del slave (14:33:5C:38:BF:60)
    
    -- ✅ ARRAYS PARA 8 RELÉS
    relay_states BOOLEAN[] DEFAULT ARRAY[false,false,false,false,false,false,false,false],
    relay_has_timers BOOLEAN[] DEFAULT ARRAY[false,false,false,false,false,false,false,false],
    relay_remaining_times INTEGER[] DEFAULT ARRAY[0,0,0,0,0,0,0,0],
    relay_names TEXT[] DEFAULT ARRAY[NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL],
    
    -- ✅ TIMESTAMPS
    last_update TIMESTAMPTZ DEFAULT NOW(),
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),
    
    -- ✅ CONSTRAINTS
    UNIQUE(device_id),
    
    -- ✅ FOREIGN KEYS (opcional, puede causar problemas si no existen)
    -- CONSTRAINT fk_relay_slaves_master 
    --   FOREIGN KEY (master_device_id) REFERENCES device_status(device_id),
    -- CONSTRAINT fk_relay_slaves_slave 
    --   FOREIGN KEY (device_id) REFERENCES device_status(device_id)
);

-- Comentarios
COMMENT ON TABLE relay_slaves IS 
'Estados de relés de slaves ESP-NOW. Actualizado por Master cuando recibe ACK o status del slave.';

COMMENT ON COLUMN relay_slaves.device_id IS 
'ID único del slave: ESP32_SLAVE_XX_XX_XX_XX_XX_XX';

COMMENT ON COLUMN relay_slaves.user_email IS 
'Email del usuario propietario (OBLIGATORIO - NOT NULL)';

COMMENT ON COLUMN relay_slaves.master_device_id IS 
'ID del dispositivo Master que controla este slave';

COMMENT ON COLUMN relay_slaves.slave_mac_address IS 
'MAC address del slave (formato: 14:33:5C:38:BF:60)';

COMMENT ON COLUMN relay_slaves.relay_states IS 
'Array de 8 booleanos: estados de los relés [0-7]';

COMMENT ON COLUMN relay_slaves.relay_has_timers IS 
'Array de 8 booleanos: si cada relé tiene timer activo';

COMMENT ON COLUMN relay_slaves.relay_remaining_times IS 
'Array de 8 enteros: tiempo restante en segundos para cada relé';

-- =====================================================
-- 4. CREAR ÍNDICES PARA PERFORMANCE
-- =====================================================

CREATE INDEX IF NOT EXISTS idx_relay_slaves_device_id 
ON relay_slaves(device_id);

CREATE INDEX IF NOT EXISTS idx_relay_slaves_master_device_id 
ON relay_slaves(master_device_id);

CREATE INDEX IF NOT EXISTS idx_relay_slaves_slave_mac 
ON relay_slaves(slave_mac_address);

CREATE INDEX IF NOT EXISTS idx_relay_slaves_user_email 
ON relay_slaves(user_email);

-- =====================================================
-- 5. VERIFICAR ESTRUCTURA DE relay_slaves
-- =====================================================

SELECT 
    column_name,
    data_type,
    is_nullable,
    column_default
FROM information_schema.columns
WHERE table_name = 'relay_slaves'
ORDER BY ordinal_position;

-- =====================================================
-- 6. VERIFICAR SI device_status TIENE user_email
-- =====================================================

-- Si device_status no tiene user_email, agregarlo:
DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 
        FROM information_schema.columns 
        WHERE table_name = 'device_status' 
        AND column_name = 'user_email'
    ) THEN
        ALTER TABLE device_status ADD COLUMN user_email TEXT;
        RAISE NOTICE 'Columna user_email agregada a device_status';
    ELSE
        RAISE NOTICE 'Columna user_email ya existe en device_status';
    END IF;
END $$;

-- =====================================================
-- 7. VERIFICAR DATOS DE EJEMPLO
-- =====================================================

-- Verificar si hay registros en device_status con user_email
SELECT 
    device_id,
    user_email,
    device_name,
    is_online,
    last_seen
FROM device_status
WHERE device_id LIKE 'ESP32_HIDRO%'
ORDER BY last_seen DESC
LIMIT 5;

-- Verificar si hay registros en relay_slaves
SELECT 
    device_id,
    user_email,
    master_device_id,
    slave_mac_address,
    relay_states,
    last_update
FROM relay_slaves
ORDER BY last_update DESC
LIMIT 5;

-- =====================================================
-- 8. VERIFICAR RLS EN relay_slaves
-- =====================================================

SELECT 
    schemaname,
    tablename,
    rowsecurity as "RLS Habilitado"
FROM pg_tables
WHERE tablename = 'relay_slaves';

-- Ver políticas RLS
SELECT 
    schemaname,
    tablename,
    policyname,
    cmd,
    qual
FROM pg_policies
WHERE tablename = 'relay_slaves';

-- =====================================================
-- FIN DEL SCRIPT
-- =====================================================



