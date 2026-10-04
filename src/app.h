#pragma once
#include <glib.h>

#define APP_ID "io.github._9d4.muslimboard" /* a leading digit is not allowed in a D-Bus name, hence the underscore */
#define RES_PREFIX "/muslimboard/"
#define VERSE_INTERVAL_DEFAULT 10

/* The IANA zone `name`, or NULL when it is empty or unknown. */
static inline GTimeZone *tz_load(const char *name)
{
    return name && *name ? g_time_zone_new_identifier(name) : NULL;
}
