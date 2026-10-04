#pragma once
#include <glib.h>

typedef struct {
    char *ar, *tr, *ref;
} Verse;

/* Curated list shipped by the MuslimBoard browser extension, embedded in the binary. */
GPtrArray *verses_load(const char *language); /* of Verse*; never empty */
