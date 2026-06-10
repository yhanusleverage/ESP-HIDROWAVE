#ifndef MQTT_COMMAND_DEDUP_H
#define MQTT_COMMAND_DEDUP_H

#include <Arduino.h>

/** Evita executar o mesmo relay_commands.id duas vezes (QoS1 / duplicatas). */
class MqttCommandDedup {
public:
    static const int CAPACITY = 32;

    MqttCommandDedup();

    bool alreadyProcessed(int commandId);
    void markProcessed(int commandId);

private:
    int ids[CAPACITY];
    int count;
};

#endif
