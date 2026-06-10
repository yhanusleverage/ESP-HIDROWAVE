#ifndef MQTT_COMMAND_PARSER_H
#define MQTT_COMMAND_PARSER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "SupabaseClient.h"

/** Parse JSON do tópico hidrowave/{id}/command → RelayCommand. */
bool parseMqttRelayCommand(
    const char* payload,
    size_t length,
    RelayCommand& out,
    bool& outIsSlave
);

#endif
