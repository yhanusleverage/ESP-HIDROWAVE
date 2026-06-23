#include "StatePersistenceManager.h"
#include "HydroControl.h"

uint8_t StatePersistenceManager::computeChecksum(const uint8_t* data, size_t len) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

bool StatePersistenceManager::saveMasterRelayCache(const MasterRelayStatesCache& cache) {
    nvs_handle_t handle;
    if (nvs_open("hidro_state", NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(handle, "master_relays", &cache, sizeof(MasterRelayStatesCache));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool StatePersistenceManager::loadMasterRelayCache(MasterRelayStatesCache& cache) {
    nvs_handle_t handle;
    if (nvs_open("hidro_state", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t required = sizeof(MasterRelayStatesCache);
    esp_err_t err = nvs_get_blob(handle, "master_relays", &cache, &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t expected = computeChecksum(reinterpret_cast<uint8_t*>(&cache), sizeof(MasterRelayStatesCache) - 1);
    return expected == cache.checksum;
}

bool StatePersistenceManager::saveEcPhBootSnapshot(const char* ecState, const char* phState, bool interrupted) {
    EcPhBootSnapshot snapshot = {};
    snapshot.timestamp = millis();
    snapshot.wasInterrupted = interrupted ? 1 : 0;
    if (ecState) {
        strncpy(snapshot.lastEcState, ecState, sizeof(snapshot.lastEcState) - 1);
    }
    if (phState) {
        strncpy(snapshot.lastPhState, phState, sizeof(snapshot.lastPhState) - 1);
    }
    snapshot.checksum = computeChecksum(reinterpret_cast<uint8_t*>(&snapshot), sizeof(EcPhBootSnapshot) - 1);

    nvs_handle_t handle;
    if (nvs_open("hidro_state", NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(handle, "ec_ph_snap", &snapshot, sizeof(EcPhBootSnapshot));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool StatePersistenceManager::loadEcPhBootSnapshot(EcPhBootSnapshot& snapshot) {
    nvs_handle_t handle;
    if (nvs_open("hidro_state", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t required = sizeof(EcPhBootSnapshot);
    esp_err_t err = nvs_get_blob(handle, "ec_ph_snap", &snapshot, &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    uint8_t expected = computeChecksum(reinterpret_cast<uint8_t*>(&snapshot), sizeof(EcPhBootSnapshot) - 1);
    return expected == snapshot.checksum;
}

bool StatePersistenceManager::applySelectiveMasterRelayRestore(HydroControl& hydroControl) {
    MasterRelayStatesCache cache = {};
    if (!loadMasterRelayCache(cache)) {
        Serial.println("BOOT_POLICY=selective_nvs_restore (sem cache válido)");
        return false;
    }

    unsigned long age = (cache.timestamp == 0) ? ULONG_MAX : (millis() - cache.timestamp);
    if (age > MAX_CACHE_AGE_MS) {
        Serial.printf("BOOT_POLICY=selective_nvs_restore (cache expirado: %lu ms)\n", age);
        return false;
    }

    Serial.println("BOOT_POLICY=selective_nvs_restore (aplicando level/reserved)");

    for (int i = 0; i < 16; i++) {
        const CachedMasterRelayState& s = cache.states[i];
        if (i <= 7) {
            hydroControl.setRelay(i, false, 0);
            continue;
        }
        if (s.state == 1 && s.hasTimer == 0) {
            hydroControl.setRelay(i, true, 0);
        } else {
            hydroControl.setRelay(i, false, 0);
        }
    }
    return true;
}
