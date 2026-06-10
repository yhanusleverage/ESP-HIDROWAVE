-- =============================================================================
-- MIGRAÇÃO INCREMENTAL FASE 2 — objetos que o firmware ESP32 espera
-- =============================================================================
-- Executar DEPOIS de migrate_device_status_incremental.sql
-- Não apaga tabelas existentes (device_status, relay_commands, hydro_*, etc.)
--
-- Corrige 404 de:
--   • get_and_lock_master_commands / get_and_lock_slave_commands
--   • relay_master / relay_slaves
--   • ec_config_view / activate_auto_ec
-- =============================================================================

-- -----------------------------------------------------------------------------
-- 1) relay_master (estados dos relés do Master — segregado por arrays)
--    Sem FK para users (tabela users pode não existir ainda)
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS public.relay_master (
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id text NOT NULL UNIQUE,
  user_email text NOT NULL DEFAULT 'unknown@local.dev',
  master_mac_address text NOT NULL DEFAULT '00:00:00:00:00:00',
  doser_relay_states boolean[] NOT NULL DEFAULT ARRAY[false,false,false,false,false,false,false,false],
  doser_relay_has_timers boolean[] DEFAULT ARRAY[false,false,false,false,false,false,false,false],
  doser_relay_remaining_times integer[] DEFAULT ARRAY[0,0,0,0,0,0,0,0],
  doser_relay_names text[],
  level_relay_states boolean[] NOT NULL DEFAULT ARRAY[false,false,false,false],
  level_relay_has_timers boolean[] DEFAULT ARRAY[false,false,false,false],
  level_relay_remaining_times integer[] DEFAULT ARRAY[0,0,0,0],
  level_relay_names text[],
  reserved_relay_states boolean[] NOT NULL DEFAULT ARRAY[false,false,false,false],
  reserved_relay_has_timers boolean[] DEFAULT ARRAY[false,false,false,false],
  reserved_relay_remaining_times integer[] DEFAULT ARRAY[0,0,0,0],
  reserved_relay_names text[],
  last_update timestamptz DEFAULT now(),
  updated_at timestamptz DEFAULT now(),
  CONSTRAINT fk_relay_master_device
    FOREIGN KEY (device_id) REFERENCES public.device_status(device_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_relay_master_device_id ON public.relay_master(device_id);
CREATE INDEX IF NOT EXISTS idx_relay_master_user_email ON public.relay_master(user_email);

-- -----------------------------------------------------------------------------
-- 2) relay_slaves (estados dos relés ESP-NOW slaves)
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS public.relay_slaves (
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id text NOT NULL UNIQUE,
  user_email text NOT NULL DEFAULT 'unknown@local.dev',
  master_device_id text NOT NULL,
  master_mac_address text NOT NULL DEFAULT '00:00:00:00:00:00',
  slave_mac_address text NOT NULL,
  relay_states boolean[] NOT NULL DEFAULT ARRAY[false,false,false,false,false,false,false,false],
  relay_has_timers boolean[] DEFAULT ARRAY[false,false,false,false,false,false,false,false],
  relay_remaining_times integer[] DEFAULT ARRAY[0,0,0,0,0,0,0,0],
  relay_names text[],
  last_update timestamptz DEFAULT now(),
  updated_at timestamptz DEFAULT now(),
  CONSTRAINT fk_relay_slaves_device
    FOREIGN KEY (device_id) REFERENCES public.device_status(device_id) ON DELETE CASCADE,
  CONSTRAINT fk_relay_slaves_master
    FOREIGN KEY (master_device_id) REFERENCES public.device_status(device_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_relay_slaves_master_device_id ON public.relay_slaves(master_device_id);

-- -----------------------------------------------------------------------------
-- 3) ec_config_view (config Auto EC — frontend + ESP32)
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS public.ec_config_view (
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  device_id text NOT NULL UNIQUE,
  base_dose double precision DEFAULT 0,
  flow_rate double precision DEFAULT 1.0,
  volume double precision DEFAULT 10,
  total_ml double precision DEFAULT 0,
  kp double precision DEFAULT 1.0,
  ec_setpoint double precision DEFAULT 0,
  auto_enabled boolean DEFAULT false,
  intervalo_auto_ec integer DEFAULT 300 CHECK (intervalo_auto_ec > 0),
  tempo_recirculacao integer DEFAULT 60 CHECK (tempo_recirculacao > 0),
  nutrients jsonb DEFAULT '[]'::jsonb,
  distribution jsonb DEFAULT NULL,
  created_at timestamptz DEFAULT now(),
  updated_at timestamptz DEFAULT now(),
  created_by text DEFAULT 'web_interface',
  CONSTRAINT fk_ec_config_view_device
    FOREIGN KEY (device_id) REFERENCES public.device_status(device_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_ec_config_view_device_id ON public.ec_config_view(device_id);

-- -----------------------------------------------------------------------------
-- 4) RPC activate_auto_ec — sem 404; retorna vazio se ainda não há config
-- -----------------------------------------------------------------------------
DROP FUNCTION IF EXISTS public.activate_auto_ec(text);

CREATE OR REPLACE FUNCTION public.activate_auto_ec(p_device_id text)
RETURNS TABLE (
  id bigint,
  device_id text,
  base_dose double precision,
  flow_rate double precision,
  volume double precision,
  total_ml double precision,
  kp double precision,
  ec_setpoint double precision,
  auto_enabled boolean,
  intervalo_auto_ec integer,
  tempo_recirculacao integer,
  nutrients jsonb,
  created_at timestamptz,
  updated_at timestamptz
)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  RETURN QUERY
  SELECT
    v.id,
    v.device_id,
    v.base_dose,
    v.flow_rate,
    v.volume,
    v.total_ml,
    v.kp,
    v.ec_setpoint,
    true AS auto_enabled,
    v.intervalo_auto_ec,
    v.tempo_recirculacao,
    v.nutrients,
    v.created_at,
    now() AS updated_at
  FROM public.ec_config_view v
  WHERE v.device_id = p_device_id
    AND v.auto_enabled = true;

  IF NOT FOUND THEN
    RETURN;
  END IF;

  UPDATE public.ec_config_view
     SET updated_at = now()
   WHERE ec_config_view.device_id = p_device_id;
END;
$$;

-- -----------------------------------------------------------------------------
-- 5) RPCs de comandos — retorno vazio até existir relay_commands_master/slave
--    (evita 404 e loop de reboot; depois pode substituir pelo SCRIPT_COMPLETO_RPC)
-- -----------------------------------------------------------------------------
DROP FUNCTION IF EXISTS public.get_and_lock_master_commands(text, integer, integer);

CREATE OR REPLACE FUNCTION public.get_and_lock_master_commands(
  p_device_id text,
  p_limit integer DEFAULT 5,
  p_timeout_seconds integer DEFAULT 30
)
RETURNS TABLE (
  id bigint,
  device_id text,
  relay_numbers integer[],
  actions text[],
  duration_seconds integer[],
  command_type text,
  priority integer,
  triggered_by text,
  rule_id text,
  rule_name text,
  created_at timestamptz
)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  RETURN;
END;
$$;

DROP FUNCTION IF EXISTS public.get_and_lock_slave_commands(text, integer, integer);

CREATE OR REPLACE FUNCTION public.get_and_lock_slave_commands(
  p_master_device_id text,
  p_limit integer DEFAULT 5,
  p_timeout_seconds integer DEFAULT 30
)
RETURNS TABLE (
  id bigint,
  master_device_id text,
  slave_device_id text,
  slave_mac_address text,
  relay_numbers integer[],
  actions text[],
  duration_seconds integer[],
  command_type text,
  priority integer,
  triggered_by text,
  rule_id text,
  rule_name text,
  created_at timestamptz
)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  RETURN;
END;
$$;

-- -----------------------------------------------------------------------------
-- 6) Permissões REST / RPC para anon
-- -----------------------------------------------------------------------------
GRANT SELECT, INSERT, UPDATE ON public.relay_master TO anon, authenticated, service_role;
GRANT SELECT, INSERT, UPDATE ON public.relay_slaves TO anon, authenticated, service_role;
GRANT SELECT, INSERT, UPDATE ON public.ec_config_view TO anon, authenticated, service_role;

GRANT EXECUTE ON FUNCTION public.activate_auto_ec(text) TO anon, authenticated, service_role;
GRANT EXECUTE ON FUNCTION public.get_and_lock_master_commands(text, integer, integer) TO anon, authenticated, service_role;
GRANT EXECUTE ON FUNCTION public.get_and_lock_slave_commands(text, integer, integer) TO anon, authenticated, service_role;

-- -----------------------------------------------------------------------------
-- 7) RLS — políticas só se não existirem (não remove as atuais)
-- -----------------------------------------------------------------------------
ALTER TABLE public.relay_master ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.relay_slaves ENABLE ROW LEVEL SECURITY;
ALTER TABLE public.ec_config_view ENABLE ROW LEVEL SECURITY;

DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_policies WHERE schemaname='public' AND tablename='relay_master' AND policyname='hidro_anon_all_relay_master') THEN
    CREATE POLICY hidro_anon_all_relay_master ON public.relay_master FOR ALL TO anon, authenticated USING (true) WITH CHECK (true);
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_policies WHERE schemaname='public' AND tablename='relay_slaves' AND policyname='hidro_anon_all_relay_slaves') THEN
    CREATE POLICY hidro_anon_all_relay_slaves ON public.relay_slaves FOR ALL TO anon, authenticated USING (true) WITH CHECK (true);
  END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_policies WHERE schemaname='public' AND tablename='ec_config_view' AND policyname='hidro_anon_all_ec_config_view') THEN
    CREATE POLICY hidro_anon_all_ec_config_view ON public.ec_config_view FOR ALL TO anon, authenticated USING (true) WITH CHECK (true);
  END IF;
END $$;

DO $$
BEGIN
  RAISE NOTICE '✅ Fase 2 aplicada: relay_master, relay_slaves, ec_config_view, RPCs';
  RAISE NOTICE '   Próximo: reiniciar ESP32 — 404 de schema devem parar';
  RAISE NOTICE '   Comandos na fila: depois rode HIDROWAVE-main/scripts/SCRIPT_COMPLETO_RPC_ATOMICO.sql';
END $$;
