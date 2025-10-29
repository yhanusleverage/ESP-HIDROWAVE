#ifndef API_CONFIG_H
#define API_CONFIG_H

#include "Config.h"

// Configurações da API
struct APICredentials {
    String vercelUrl;      // URL da interface web
    String supabaseUrl;    // URL do Supabase
    String supabaseKey;    // Chave anon do Supabase
    bool valid = false;

    bool isValid() const {
        return vercelUrl.length() > 0 && supabaseUrl.length() > 0 && supabaseKey.length() > 0;
    }
};

// Endpoints Vercel (controle em tempo real)
#define API_BASE_PATH "/api"
#define API_ENDPOINT_RELAY "/api/relay"
#define API_ENDPOINT_RELAY_CHECK "/api/relay/check"
#define API_ENDPOINT_STATUS "/api/device-status"

// Endpoints Supabase (armazenamento de dados)
#define SUPABASE_ENDPOINT_SENSOR_DATA "/rest/v1/sensor_data"
#define SUPABASE_ENDPOINT_DEVICE_LOGS "/rest/v1/device_logs"

// URLs padrão
#define DEFAULT_VERCEL_URL "https://seu-projeto.vercel.app"
#define DEFAULT_SUPABASE_URL "https://seu-projeto.supabase.co"
#define DEFAULT_SUPABASE_ANON_KEY "sua-chave-anon-key"

// Configurações de requisições HTTP
const unsigned long HTTP_TIMEOUT = 10000;        // 10 segundos
const unsigned long RETRY_DELAY = 1000;          // 1 segundo
const unsigned long COMMAND_TIMEOUT = 60000;     // 1 minuto

// Códigos de erro HTTP
enum class APIErrorCode {
    SUCCESS = 200,
    BAD_REQUEST = 400,
    UNAUTHORIZED = 401,
    NOT_FOUND = 404,
    SERVER_ERROR = 500,
    TIMEOUT = -1,
    CONNECTION_ERROR = -2
};

// Headers HTTP padrão
const char* const API_CONTENT_TYPE = "application/json";
const char* const API_DEVICE_ID_HEADER = "X-Device-ID";

// Headers Supabase
const char* const SUPABASE_AUTH_HEADER = "apikey";
const char* const SUPABASE_PREFER_HEADER = "Prefer";
const char* const SUPABASE_PREFER_VALUE = "return=minimal";

// Validação de respostas
const size_t MAX_JSON_SIZE = 4096;  // 4KB
const size_t MIN_JSON_SIZE = 2;     // {}

// Mensagens de erro padrão
const char* const ERROR_TIMEOUT = "Timeout na requisição";
const char* const ERROR_CONNECTION = "Erro de conexão";
const char* const ERROR_SERVER = "Erro no servidor";
const char* const ERROR_INVALID_JSON = "JSON inválido";
const char* const ERROR_UNAUTHORIZED = "Não autorizado";

#endif // API_CONFIG_H 