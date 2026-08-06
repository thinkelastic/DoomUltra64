#ifndef I_SAVE_H
#define I_SAVE_H

#include <stddef.h>
#include <stdint.h>

#include "doomtype.h"

/* I_SaveSlotWrite prepends a 16-byte "PDSV" wrapper header to every
   save it persists, so callers must budget payloads against
   (slot size - SAVE_WRAP_HEADER_SIZE) or the on-disk file overflows the
   256 KB NVRAM slot. */
#define SAVE_WRAP_HEADER_SIZE  16

void I_SetSaveIdentity(const char *iwad, const char *pwad);
uint32_t I_SaveGameID(void);
boolean I_SaveSlotRead(const char *name, byte *buffer,
                           size_t capacity, size_t *length);
boolean I_SaveSlotReadHeader(const char *name, byte *buffer,
                                 size_t length);
boolean I_SaveSlotWrite(const char *name, const byte *buffer,
                            size_t length);
void I_MigratePocketDoomSaves(void);

#endif
