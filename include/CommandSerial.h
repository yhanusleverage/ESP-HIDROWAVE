#ifndef COMMAND_SERIAL_H
#define COMMAND_SERIAL_H

#include "SupabaseClient.h"

/** Log conciso — mesma semântica para MQTT e HTTPS poll. */
void printRelayCommandSerialLine(
    const RelayCommand& cmd,
    bool isSlave,
    const char* via
);

#endif
