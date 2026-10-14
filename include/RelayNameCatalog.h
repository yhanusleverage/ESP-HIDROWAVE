#ifndef RELAY_NAME_CATALOG_H
#define RELAY_NAME_CATALOG_H

#include <stddef.h>

/** Nombres de relé Atlas que publica la web (relay_slaves.relay_names). */
namespace RelayNameCatalog {

struct SlotView {
    const char *mac;
    const char *place;
    const char *names[8];
};

bool applyJson(const char *payload, size_t length);
/** Escribe un nombre en el slot ya recibido. No crea slots. */
bool setName(const char *mac, int relay, const char *name);
const char *nameFor(const char *mac, int relay);
int slotCount();
bool slotAt(int index, SlotView &out);

}  // namespace RelayNameCatalog

#endif
