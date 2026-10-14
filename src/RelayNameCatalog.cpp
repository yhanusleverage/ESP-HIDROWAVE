#include "RelayNameCatalog.h"

#include <ArduinoJson.h>
#include <cstring>

namespace {

constexpr int kSlots = 4;
constexpr int kNameLen = 32;

struct Slot {
    bool used;
    char mac[18];
    char place[40];
    char names[8][kNameLen];
};

Slot slots_[kSlots];

void normMac(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < n; ++i) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

bool macEq(const char *a, const char *b) {
    char aa[18];
    char bb[18];
    normMac(aa, sizeof(aa), a);
    normMac(bb, sizeof(bb), b);
    return aa[0] && strcmp(aa, bb) == 0;
}

void copyClip(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

void copyName(char *dst, const char *src) { copyClip(dst, kNameLen, src); }

}  // namespace

namespace RelayNameCatalog {

bool applyJson(const char *payload, size_t length) {
    if (!payload || length == 0) {
        return false;
    }
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, payload, length)) {
        return false;
    }
    const char *op = doc["op"] | "";
    if (strcmp(op, "relay_names") != 0) {
        return false;
    }
    JsonArray slaves = doc["slaves"].as<JsonArray>();
    if (slaves.isNull()) {
        return false;
    }
    memset(slots_, 0, sizeof(slots_));
    int n = 0;
    for (JsonObject slave : slaves) {
        if (n >= kSlots) {
            break;
        }
        const char *mac = slave["mac"] | "";
        if (!mac[0]) {
            continue;
        }
        Slot &slot = slots_[n++];
        slot.used = true;
        normMac(slot.mac, sizeof(slot.mac), mac);
        copyClip(slot.place, sizeof(slot.place), slave["place"] | "");
        JsonArray names = slave["names"].as<JsonArray>();
        for (int i = 0; i < 8; ++i) {
            slot.names[i][0] = '\0';
            if (names.isNull() || i >= static_cast<int>(names.size())) {
                continue;
            }
            if (names[i].is<const char *>()) {
                copyName(slot.names[i], names[i].as<const char *>());
            }
        }
    }
    return true;
}

bool setName(const char *mac, int relay, const char *name) {
    if (!mac || !mac[0] || relay < 0 || relay > 7) {
        return false;
    }
    for (int i = 0; i < kSlots; ++i) {
        if (!slots_[i].used || !macEq(slots_[i].mac, mac)) {
            continue;
        }
        copyName(slots_[i].names[relay], name ? name : "");
        return true;
    }
    return false;
}

const char *nameFor(const char *mac, int relay) {
    if (relay < 0 || relay > 7) {
        return nullptr;
    }
    if (mac && mac[0]) {
        for (int i = 0; i < kSlots; ++i) {
            if (!slots_[i].used) {
                continue;
            }
            if (macEq(slots_[i].mac, mac) && slots_[i].names[relay][0]) {
                return slots_[i].names[relay];
            }
        }
    }
    const char *only = nullptr;
    int hits = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (!slots_[i].used || !slots_[i].names[relay][0]) {
            continue;
        }
        only = slots_[i].names[relay];
        ++hits;
    }
    return hits == 1 ? only : nullptr;
}

int slotCount() {
    int n = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (slots_[i].used) {
            ++n;
        }
    }
    return n;
}

bool slotAt(int index, SlotView &out) {
    int seen = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (!slots_[i].used) {
            continue;
        }
        if (seen == index) {
            out.mac = slots_[i].mac;
            out.place = slots_[i].place;
            for (int r = 0; r < 8; ++r) {
                out.names[r] = slots_[i].names[r];
            }
            return true;
        }
        ++seen;
    }
    return false;
}

}  // namespace RelayNameCatalog
