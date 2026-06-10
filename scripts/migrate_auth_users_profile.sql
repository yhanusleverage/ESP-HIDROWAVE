-- =============================================================================
-- MIGRAÇÃO INCREMENTAL — Auth + perfil public.users
-- =============================================================================
-- Seguro para projeto existente:
--   • Não apaga tabelas, dados, colunas nem políticas antigas
--   • CREATE TABLE IF NOT EXISTS (só cria se a tabela não existir)
--   • ADD COLUMN IF NOT EXISTS (só acrescenta colunas em falta)
--   • Políticas RLS só se ainda não existirem
--   • CREATE OR REPLACE na função + trigger (atualiza lógica, não apaga users)
--   • Backfill: insere perfis em falta a partir de auth.users (ON CONFLICT DO NOTHING)
--
-- O ESP32 já faz signup via POST /auth/v1/signup (anon). Este script melhora o painel.
-- Executar: Supabase Dashboard → SQL Editor → Run
-- =============================================================================

-- -----------------------------------------------------------------------------
-- 1) Tabela users (só cria se não existir — esquema alinhado ao frontend)
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS public.users (
  id bigint GENERATED ALWAYS AS IDENTITY NOT NULL,
  email text NOT NULL UNIQUE,
  name text,
  created_at timestamptz DEFAULT now(),
  updated_at timestamptz DEFAULT now(),
  is_active boolean DEFAULT true,
  total_devices integer DEFAULT 0,
  subscription_type text DEFAULT 'free',
  max_devices integer DEFAULT 5,
  notification_email boolean DEFAULT true,
  timezone text DEFAULT 'America/Sao_Paulo',
  CONSTRAINT users_pkey PRIMARY KEY (id)
);

-- -----------------------------------------------------------------------------
-- 2) Colunas em falta (se a tabela já existia com esquema antigo)
-- -----------------------------------------------------------------------------
ALTER TABLE public.users
  ADD COLUMN IF NOT EXISTS name text,
  ADD COLUMN IF NOT EXISTS created_at timestamptz DEFAULT now(),
  ADD COLUMN IF NOT EXISTS updated_at timestamptz DEFAULT now(),
  ADD COLUMN IF NOT EXISTS is_active boolean DEFAULT true,
  ADD COLUMN IF NOT EXISTS total_devices integer DEFAULT 0,
  ADD COLUMN IF NOT EXISTS subscription_type text DEFAULT 'free',
  ADD COLUMN IF NOT EXISTS max_devices integer DEFAULT 5,
  ADD COLUMN IF NOT EXISTS notification_email boolean DEFAULT true,
  ADD COLUMN IF NOT EXISTS timezone text DEFAULT 'America/Sao_Paulo';

-- id / email: não alteramos PK nem UNIQUE existentes (evita quebrar FKs)

-- -----------------------------------------------------------------------------
-- 3) RLS — só ativa; políticas novas só se não existirem
-- -----------------------------------------------------------------------------
ALTER TABLE public.users ENABLE ROW LEVEL SECURITY;

DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM pg_policies
    WHERE schemaname = 'public' AND tablename = 'users' AND policyname = 'users_select_own'
  ) THEN
    CREATE POLICY users_select_own ON public.users
      FOR SELECT TO authenticated
      USING (lower(email) = lower(auth.jwt() ->> 'email'));
  END IF;

  IF NOT EXISTS (
    SELECT 1 FROM pg_policies
    WHERE schemaname = 'public' AND tablename = 'users' AND policyname = 'users_insert_own'
  ) THEN
    CREATE POLICY users_insert_own ON public.users
      FOR INSERT TO authenticated
      WITH CHECK (lower(email) = lower(auth.jwt() ->> 'email'));
  END IF;

  IF NOT EXISTS (
    SELECT 1 FROM pg_policies
    WHERE schemaname = 'public' AND tablename = 'users' AND policyname = 'users_update_own'
  ) THEN
    CREATE POLICY users_update_own ON public.users
      FOR UPDATE TO authenticated
      USING (lower(email) = lower(auth.jwt() ->> 'email'));
  END IF;
END $$;

-- -----------------------------------------------------------------------------
-- 4) Função + trigger (substitui só a função; não mexe em linhas de users)
-- -----------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION public.handle_new_auth_user()
RETURNS trigger
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = public
AS $$
BEGIN
  INSERT INTO public.users (email, name, is_active)
  VALUES (
    lower(NEW.email),
    COALESCE(NEW.raw_user_meta_data ->> 'name', split_part(NEW.email, '@', 1)),
    true
  )
  ON CONFLICT (email) DO NOTHING;
  RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS on_auth_user_created ON auth.users;
CREATE TRIGGER on_auth_user_created
  AFTER INSERT ON auth.users
  FOR EACH ROW
  EXECUTE FUNCTION public.handle_new_auth_user();

-- -----------------------------------------------------------------------------
-- 5) Backfill — utilizadores Auth sem linha em public.users (não atualiza existentes)
-- -----------------------------------------------------------------------------
INSERT INTO public.users (email, name, is_active)
SELECT
  lower(u.email),
  COALESCE(u.raw_user_meta_data ->> 'name', split_part(u.email, '@', 1)),
  true
FROM auth.users u
WHERE u.email IS NOT NULL
  AND NOT EXISTS (
    SELECT 1 FROM public.users p WHERE lower(p.email) = lower(u.email)
  );

-- -----------------------------------------------------------------------------
-- 6) Permissões (idempotentes)
-- -----------------------------------------------------------------------------
GRANT SELECT, INSERT, UPDATE ON public.users TO authenticated;
GRANT SELECT ON public.users TO anon;

-- -----------------------------------------------------------------------------
-- 7) Verificação (opcional)
-- -----------------------------------------------------------------------------
DO $$
BEGIN
  RAISE NOTICE '✅ Migração incremental users/auth aplicada';
  RAISE NOTICE '   Nada foi apagado; só criado/acrescentado o que faltava';
END $$;
