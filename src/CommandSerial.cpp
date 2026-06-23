#include "CommandSerial.h"

void printRelayCommandSerialLine(
    const RelayCommand& cmd,
    bool isSlave,
    const char* via) {
    const char* target = cmd.target_device_id.isEmpty() ? "local" : cmd.target_device_id.c_str();
    Serial.printf(
        "[CMD %s] supabase_id=%d %s R%d %s dur=%ds mode=%s pri=%d tgt=%s\n",
        via,
        cmd.id,
        isSlave ? "slave" : "master",
        cmd.relayNumber,
        cmd.action.c_str(),
        cmd.durationSeconds,
        cmd.commandMode.length() > 0 ? cmd.commandMode.c_str() : "instant",
        cmd.priority,
        target);
}

void logCmdCloudAckResult(
    const char* via,
    int supabaseId,
    uint32_t espnowId,
    int relayNumber,
    bool stateOn,
    bool cloudClosed) {
    Serial.printf(
        "[CMD %s] supabase_id=%d espnow_id=%u R%d state=%s cloud_closed=%s\n",
        via,
        supabaseId,
        espnowId,
        relayNumber,
        stateOn ? "ON" : "OFF",
        cloudClosed ? "true" : "false");
}
