-- =====================================================
-- SCRIPT: VERIFICAR Y DESHABILITAR RLS (SOLO DESARROLLO)
-- =====================================================
-- ⚠️ ADVERTENCIA: Este script deshabilita RLS
-- ⚠️ SOLO USAR EN DESARROLLO - NO EN PRODUCCIÓN
-- =====================================================

-- =====================================================
-- 1. VERIFICAR ESTADO ACTUAL DE RLS
-- =====================================================

SELECT 
    schemaname,
    tablename,
    rowsecurity as "RLS Habilitado"
FROM pg_tables
WHERE tablename IN (
    'relay_commands',
    'relay_commands_master',
    'relay_commands_slave',
    'relay_master',
    'relay_slaves',
    'device_status'
)
ORDER BY tablename;

-- =====================================================
-- 2. VERIFICAR POLÍTICAS RLS EXISTENTES
-- =====================================================

SELECT 
    schemaname,
    tablename,
    policyname,
    permissive,
    roles,
    cmd,
    qual,
    with_check
FROM pg_policies
WHERE tablename IN (
    'relay_commands',
    'relay_commands_master',
    'relay_commands_slave',
    'relay_master',
    'relay_slaves',
    'device_status'
)
ORDER BY tablename, policyname;

-- =====================================================
-- 3. DESHABILITAR RLS (SOLO DESARROLLO)
-- =====================================================
-- ⚠️ DESCOMENTAR PARA DESHABILITAR RLS

/*
ALTER TABLE relay_commands DISABLE ROW LEVEL SECURITY;
ALTER TABLE relay_commands_master DISABLE ROW LEVEL SECURITY;
ALTER TABLE relay_commands_slave DISABLE ROW LEVEL SECURITY;
ALTER TABLE relay_master DISABLE ROW LEVEL SECURITY;
ALTER TABLE relay_slaves DISABLE ROW LEVEL SECURITY;
ALTER TABLE device_status DISABLE ROW LEVEL SECURITY;
*/

-- =====================================================
-- 4. HABILITAR RLS (PARA REVERTER)
-- =====================================================
-- ⚠️ DESCOMENTAR PARA HABILITAR RLS DE NUEVO

/*
ALTER TABLE relay_commands ENABLE ROW LEVEL SECURITY;
ALTER TABLE relay_commands_master ENABLE ROW LEVEL SECURITY;
ALTER TABLE relay_commands_slave ENABLE ROW LEVEL SECURITY;
ALTER TABLE relay_master ENABLE ROW LEVEL SECURITY;
ALTER TABLE relay_slaves ENABLE ROW LEVEL SECURITY;
ALTER TABLE device_status ENABLE ROW LEVEL SECURITY;
*/

-- =====================================================
-- 5. ELIMINAR TODAS LAS POLÍTICAS (CUIDADO!)
-- =====================================================
-- ⚠️ SOLO SI QUIERES EMPEZAR DE CERO

/*
DROP POLICY IF EXISTS "Enable insert for anon" ON relay_commands;
DROP POLICY IF EXISTS "Enable select for anon" ON relay_commands;
DROP POLICY IF EXISTS "Enable update for anon" ON relay_commands;
DROP POLICY IF EXISTS "Enable insert for anon" ON relay_commands_master;
DROP POLICY IF EXISTS "Enable select for anon" ON relay_commands_master;
DROP POLICY IF EXISTS "Enable update for anon" ON relay_commands_master;
DROP POLICY IF EXISTS "Enable insert for anon" ON relay_commands_slave;
DROP POLICY IF EXISTS "Enable select for anon" ON relay_commands_slave;
DROP POLICY IF EXISTS "Enable update for anon" ON relay_commands_slave;
*/

-- =====================================================
-- 6. CREAR POLÍTICAS PERMISIVAS (PARA DESARROLLO)
-- =====================================================
-- ⚠️ ESTAS POLÍTICAS PERMITEN TODO - SOLO DESARROLLO

/*
-- Políticas para relay_commands_master
CREATE POLICY "Dev: Allow all operations" ON relay_commands_master
FOR ALL
USING (true)
WITH CHECK (true);

-- Políticas para relay_commands_slave
CREATE POLICY "Dev: Allow all operations" ON relay_commands_slave
FOR ALL
USING (true)
WITH CHECK (true);

-- Políticas para relay_master
CREATE POLICY "Dev: Allow all operations" ON relay_master
FOR ALL
USING (true)
WITH CHECK (true);

-- Políticas para relay_slaves
CREATE POLICY "Dev: Allow all operations" ON relay_slaves
FOR ALL
USING (true)
WITH CHECK (true);
*/

-- =====================================================
-- 7. VERIFICAR FUNCIONES RPC
-- =====================================================

SELECT 
    proname as "Función",
    pronargs as "Número de Parámetros",
    pg_get_function_arguments(oid) as "Argumentos"
FROM pg_proc
WHERE proname LIKE '%get_and_lock%'
ORDER BY proname;

-- =====================================================
-- 8. PROBAR FUNCIONES RPC MANUALMENTE
-- =====================================================

-- Probar get_and_lock_master_commands
SELECT * FROM get_and_lock_master_commands(
    'ESP32_HIDRO_F44738'::text,
    1::integer,
    30::integer
);

-- Probar get_and_lock_slave_commands
SELECT * FROM get_and_lock_slave_commands(
    'ESP32_HIDRO_F44738'::text,
    1::integer,
    30::integer
);

-- =====================================================
-- FIN DEL SCRIPT
-- =====================================================



