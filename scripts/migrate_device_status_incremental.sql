-- =============================================================================
-- MIGRAÇÃO INCREMENTAL — device_status + registro ESP32 (Opção A)
-- =============================================================================
-- Seguro para rodar em projeto existente:
--   • Só ADICIONA colunas (IF NOT EXISTS)
--   • Não apaga tabelas, dados nem colunas existentes
--   • CREATE OR REPLACE nas funções RPC
--   • Políticas RLS só se ainda não existirem
--
-- Onde executar: Supabase Dashboard → SQL Editor → New query → Run
-- Depois: reiniciar ESP32 ou refazer setup WiFi para testar registro
-- =============================================================================

-- -----------------------------------------------------------------------------
-- 1) Colunas novas em device_status (mantém todas as atuais)
-- -----------------------------------------------------------------------------
ALTER TABLE public.device_status
  ADD COLUMN IF NOT EXISTS user_email text,
  ADD COLUMN IF NOT EXISTS mac_address text,
  ADD COLUMN IF NOT EXISTS device_name text,
  ADD COLUMN IF NOT EXISTS location text,
  ADD COLUMN IF NOT EXISTS device_type text DEFAULT 'ESP32_HYDROPONIC';

COMMENT ON COLUMN public.device_status.user_email IS 'Email do utilizador (dashboard / multi-dispositivo)';
COMMENT ON COLUMN public.device_status.mac_address IS 'MAC WiFi do ESP32';
COMMENT ON COLUMN public.device_status.device_name IS 'Nome amigável do dispositivo';
COMMENT ON COLUMN public.device_status.location IS 'Localização (ex: Estufa)';
COMMENT ON COLUMN public.device_status.device_type IS 'ESP32_HYDROPONIC | ESP32_SLAVE';

-- Índices úteis (idempotentes)
CREATE INDEX IF NOT EXISTS idx_device_status_user_email
  ON public.device_status (user_email);

CREATE INDEX IF NOT EXISTS idx_device_status_mac_address
  ON public.device_status (mac_address);

-- -----------------------------------------------------------------------------
-- 2) RPC: register_device_with_email (ESP32 portal WiFi + DeviceRegistration.cpp)
--    Parâmetros na ordem esperada pelo PostgREST / firmware
-- -----------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION public.register_device_with_email(
  p_device_id text,
  p_mac_address text,
  p_user_email text,
  p_device_name text DEFAULT NULL,
  p_location text DEFAULT NULL,
  p_ip_address text DEFAULT NULL
)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_count integer;
  v_final_name text;
  v_final_location text;
  v_email text;
  v_ip inet;
BEGIN
  IF p_device_id IS NULL OR btrim(p_device_id) = '' THEN
    RETURN jsonb_build_object('success', false, 'message', 'device_id é obrigatório');
  END IF;

  IF p_mac_address IS NULL OR btrim(p_mac_address) = '' THEN
    RETURN jsonb_build_object('success', false, 'message', 'mac_address é obrigatório');
  END IF;

  IF p_user_email IS NULL OR btrim(p_user_email) = '' THEN
    RETURN jsonb_build_object('success', false, 'message', 'user_email é obrigatório');
  END IF;

  IF position('@' in p_user_email) = 0 THEN
    RETURN jsonb_build_object('success', false, 'message', 'user_email inválido');
  END IF;

  v_email := lower(btrim(p_user_email));
  v_final_name := COALESCE(
    NULLIF(btrim(p_device_name), ''),
    'ESP32 - ' || right(replace(p_mac_address, ':', ''), 8)
  );
  v_final_location := COALESCE(
    NULLIF(btrim(p_location), ''),
    'Localização não especificada'
  );

  v_ip := NULL;
  IF p_ip_address IS NOT NULL AND btrim(p_ip_address) <> '' THEN
    BEGIN
      v_ip := btrim(p_ip_address)::inet;
    EXCEPTION
      WHEN others THEN
        v_ip := NULL;
    END;
  END IF;

  INSERT INTO public.device_status (
    device_id,
    mac_address,
    user_email,
    device_name,
    location,
    ip_address,
    device_type,
    last_seen,
    is_online,
    updated_at
  )
  VALUES (
    btrim(p_device_id),
    btrim(p_mac_address),
    v_email,
    v_final_name,
    v_final_location,
    v_ip,
    'ESP32_HYDROPONIC',
    now(),
    true,
    now()
  )
  ON CONFLICT (device_id) DO UPDATE SET
    mac_address = EXCLUDED.mac_address,
    device_name = COALESCE(EXCLUDED.device_name, device_status.device_name),
    location = COALESCE(EXCLUDED.location, device_status.location),
    ip_address = COALESCE(EXCLUDED.ip_address, device_status.ip_address),
    device_type = COALESCE(device_status.device_type, EXCLUDED.device_type),
    user_email = EXCLUDED.user_email,
    last_seen = now(),
    is_online = true,
    updated_at = now();

  SELECT count(*)::integer
    INTO v_count
    FROM public.device_status
   WHERE lower(user_email) = v_email;

  RETURN jsonb_build_object(
    'success', true,
    'message', 'Dispositivo registrado com sucesso',
    'device_id', btrim(p_device_id),
    'user_email', v_email,
    'device_count', v_count
  );
EXCEPTION
  WHEN others THEN
    RETURN jsonb_build_object(
      'success', false,
      'message', SQLERRM
    );
END;
$$;

-- -----------------------------------------------------------------------------
-- 3) RPC opcional: can_add_device (DeviceRegistration.cpp — fail-safe no ESP)
-- -----------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION public.can_add_device(p_user_email text)
RETURNS jsonb
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
DECLARE
  v_count integer;
  v_max integer := 20;
BEGIN
  IF p_user_email IS NULL OR btrim(p_user_email) = '' THEN
    RETURN jsonb_build_object(
      'can_add', false,
      'current_count', 0,
      'max_allowed', v_max,
      'message', 'email obrigatório'
    );
  END IF;

  SELECT count(*)::integer
    INTO v_count
    FROM public.device_status
   WHERE lower(user_email) = lower(btrim(p_user_email));

  RETURN jsonb_build_object(
    'can_add', v_count < v_max,
    'current_count', v_count,
    'max_allowed', v_max
  );
END;
$$;

-- -----------------------------------------------------------------------------
-- 4) Permissões para REST / ESP32 (anon)
-- -----------------------------------------------------------------------------
GRANT EXECUTE ON FUNCTION public.register_device_with_email(
  text, text, text, text, text, text
) TO anon, authenticated, service_role;

GRANT EXECUTE ON FUNCTION public.can_add_device(text)
  TO anon, authenticated, service_role;

-- -----------------------------------------------------------------------------
-- 5) RLS — só cria políticas se ainda não existirem (não remove nada)
-- -----------------------------------------------------------------------------
ALTER TABLE public.device_status ENABLE ROW LEVEL SECURITY;

DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_policies
    WHERE schemaname = 'public'
      AND tablename = 'device_status'
      AND policyname = 'hidro_anon_insert_device_status'
  ) THEN
    CREATE POLICY hidro_anon_insert_device_status ON public.device_status
      FOR INSERT TO anon, authenticated
      WITH CHECK (true);
  END IF;

  IF NOT EXISTS (
    SELECT 1 FROM pg_policies
    WHERE schemaname = 'public'
      AND tablename = 'device_status'
      AND policyname = 'hidro_anon_select_device_status'
  ) THEN
    CREATE POLICY hidro_anon_select_device_status ON public.device_status
      FOR SELECT TO anon, authenticated
      USING (true);
  END IF;

  IF NOT EXISTS (
    SELECT 1 FROM pg_policies
    WHERE schemaname = 'public'
      AND tablename = 'device_status'
      AND policyname = 'hidro_anon_update_device_status'
  ) THEN
    CREATE POLICY hidro_anon_update_device_status ON public.device_status
      FOR UPDATE TO anon, authenticated
      USING (true)
      WITH CHECK (true);
  END IF;
END $$;

-- -----------------------------------------------------------------------------
-- 6) Verificação rápida (opcional — comentar se não quiser output)
-- -----------------------------------------------------------------------------
DO $$
BEGIN
  RAISE NOTICE '✅ Migração incremental aplicada';
  RAISE NOTICE '   Colunas: user_email, mac_address, device_name, location, device_type';
  RAISE NOTICE '   RPC: register_device_with_email, can_add_device';
END $$;
