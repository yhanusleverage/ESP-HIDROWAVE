#include "CommandSerial.h"

void printRelayCommandSerialLine(
    const RelayCommand& cmd,
    bool isSlave,
    const char* via) {
    const char* target = cmd.target_device_id.isEmpty() ? "local" : cmd.target_device_id.c_str();
    Serial.printf(
        "[CMD %s] id=%d %s R%d %s dur=%ds pri=%d tgt=%s\n",
        via,
        cmd.id,
        isSlave ? "slave" : "master",
        cmd.relayNumber,
        cmd.action.c_str(),
        cmd.durationSeconds,
        cmd.priority,
        target);
}
