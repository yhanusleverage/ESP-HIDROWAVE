#include "MqttCommandDedup.h"

MqttCommandDedup::MqttCommandDedup() : count(0) {
    for (int i = 0; i < CAPACITY; i++) {
        ids[i] = -1;
    }
}

bool MqttCommandDedup::alreadyProcessed(int commandId) {
    if (commandId <= 0) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (ids[i] == commandId) {
            return true;
        }
    }
    return false;
}

void MqttCommandDedup::markProcessed(int commandId) {
    if (commandId <= 0) {
        return;
    }
    if (alreadyProcessed(commandId)) {
        return;
    }
    if (count < CAPACITY) {
        ids[count++] = commandId;
        return;
    }
    for (int i = 1; i < CAPACITY; i++) {
        ids[i - 1] = ids[i];
    }
    ids[CAPACITY - 1] = commandId;
}
