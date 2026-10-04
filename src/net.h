#pragma once
#include "config.h"
#include <glib.h>

#define PRAYER_COUNT 6 /* Fajr, Sunrise, Dhuhr, Asr, Maghrib, Isha */

typedef struct {
    char *city;
    double latitude, longitude;
    char *timezone;
    char *country_code;
} Place;

typedef struct {
    int hour[PRAYER_COUNT], minute[PRAYER_COUNT];
    char *hijri; /* e.g. "20 Rabi al-thani 1448 AH" */
} Timings;

void place_free(Place *p);
void timings_free(Timings *t);

/* All of these block; call them from a worker thread. */
GPtrArray *net_search_places(const char *query, GError **err); /* of Place*, free func set */
Place *net_detect_place(GError **err);
Timings *net_fetch_timings(const Config *cfg, int year, int month, int day, GError **err);
void net_clear_cache(void);
