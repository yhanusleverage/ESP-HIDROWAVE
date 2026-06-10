#ifndef SUPABASE_AUTH_H
#define SUPABASE_AUTH_H

#include <Arduino.h>

/** Resultado do signup via POST /auth/v1/signup (chave anon). */
enum class SupabaseSignupResult {
    Created,          // HTTP 200 — conta criada
    AlreadyExists,    // utilizador já registado — OK para login
    SkippedNoNetwork, // WiFi sem internet no momento do teste
    SkippedNoCreds,   // email ou senha em falta
    Failed            // erro HTTP ou resposta inesperada
};

/**
 * Cria conta Supabase Auth (ou confirma que já existe).
 * Nunca regista a senha em Serial — apenas comprimento.
 */
SupabaseSignupResult signupSupabaseUser(
    const String& email,
    const String& password,
    String& detailMessage
);

#endif
