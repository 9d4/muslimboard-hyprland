#pragma once
#include "config.h"
#include <gtk/gtk.h>

typedef void (*SettingsSaved)(const Config *cfg, gpointer data);
typedef void (*SettingsClosed)(gpointer data);

/* Opens the (non-modal) settings window. `saved` gets a validated config; `closed` always fires once at the end. */
GtkWidget *settings_open(const Config *cfg, SettingsSaved saved, SettingsClosed closed, gpointer data);
