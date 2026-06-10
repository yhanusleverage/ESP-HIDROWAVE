#include "SupabaseAuth.h"
#include "Config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFi.h>

static String sanitizeEmail(const String& email) {
    String e = email;
    e.trim();
    e.toLowerCase();
    e.replace("\"", "");
    e.replace("'", "");
    e.replace(" ", "");
    return e;
}

static bool isValidEmailFormat(const String& email) {
    int at = email.indexOf('@');
    if (at <= 0) return false;
    int dot = email.lastIndexOf('.');
    if (dot <= at + 1) return false;
    if (dot >= (int)email.length() - 2) return false;
    return true;
}

static bool responseIndicatesExistingUser(const String& body, int httpCode) {
    if (httpCode == 422 || httpCode == 400) {
        String lower = body;
        lower.toLowerCase();
        if (lower.indexOf("already") >= 0 ||
            lower.indexOf("registered") >= 0 ||
            lower.indexOf("user_already_exists") >= 0) {
            return true;
        }
    }
    return false;
}

SupabaseSignupResult signupSupabaseUser(
    const String& email,
    const String& password,
    String& detailMessage
) {
    detailMessage = "";

    String cleanEmail = sanitizeEmail(email);

    if (cleanEmail.length() == 0 || password.length() < 6) {
        detailMessage = "Email e senha (min. 6) obrigatorios";
        return SupabaseSignupResult::SkippedNoCreds;
    }

    if (!isValidEmailFormat(cleanEmail)) {
        detailMessage = "Formato de email invalido (ex: nome@gmail.com)";
        return SupabaseSignupResult::Failed;
    }

    if (WiFi.status() != WL_CONNECTED) {
        detailMessage = "Sem WiFi — conta sera criada no proximo boot se possivel";
        return SupabaseSignupResult::SkippedNoNetwork;
    }

    Serial.printf("🔐 [AUTH] Signup para %s (senha: %u chars)\n",
                  cleanEmail.c_str(),
                  (unsigned)password.length());

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String url = String(SUPABASE_URL) + "/auth/v1/signup";
    if (!http.begin(client, url)) {
        detailMessage = "Falha ao iniciar cliente HTTP";
        return SupabaseSignupResult::Failed;
    }

    http.setConnectTimeout(12000);
    http.setTimeout(15000);
    http.addHeader("apikey", SUPABASE_ANON_KEY);
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<192> body;
    body["email"] = cleanEmail;
    body["password"] = password;

    String payload;
    serializeJson(body, payload);

    int httpCode = http.POST(payload);
    String response = http.getString();
    http.end();

    Serial.printf("🔐 [AUTH] Signup HTTP %d\n", httpCode);

    if (httpCode == 200) {
        detailMessage = "Conta criada no Supabase";
        return SupabaseSignupResult::Created;
    }

    if (responseIndicatesExistingUser(response, httpCode)) {
        detailMessage = "Conta ja existia — use a mesma senha no painel";
        return SupabaseSignupResult::AlreadyExists;
    }

    if (httpCode > 0) {
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, response) == DeserializationError::Ok) {
            if (doc["msg"].is<const char*>()) {
                detailMessage = doc["msg"].as<String>();
            } else if (doc["error_description"].is<const char*>()) {
                detailMessage = doc["error_description"].as<String>();
            } else if (doc["message"].is<const char*>()) {
                detailMessage = doc["message"].as<String>();
            }
        }
        if (detailMessage.length() == 0) {
            detailMessage = "Signup falhou HTTP " + String(httpCode);
        }
        return SupabaseSignupResult::Failed;
    }

    detailMessage = "Sem resposta do servidor Auth";
    return SupabaseSignupResult::Failed;
}
