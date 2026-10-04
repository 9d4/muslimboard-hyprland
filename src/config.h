#pragma once
#include <glib.h>

typedef struct {
    char *city;
    gboolean has_location;
    double latitude, longitude;
    char *timezone; /* IANA name; empty = system timezone */
    int method;     /* Aladhan calculation method id */
    char *language; /* "en" | "id" */
    gboolean clock_24h;
    int monitor;
    int verse_interval_minutes;
} Config;

Config *config_load(void);
gboolean config_save(const Config *cfg, GError **err);
Config *config_copy(const Config *cfg);
void config_free(Config *cfg);
char *config_dir(void);
