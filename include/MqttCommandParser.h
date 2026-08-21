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

/** action=ec_dilution_start, volume_l>0 */
bool parseMqttEcDilutionCommand(const char* payload, size_t length, float& outVolumeL);

/** action=set_level_interlock, mode=normal|carrera */
bool parseMqttLevelInterlockCommand(const char* payload, size_t length, char* outMode, size_t outModeLen);

#endif
