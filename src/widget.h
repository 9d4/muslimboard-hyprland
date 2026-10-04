#pragma once
#include "config.h"
#include <gtk/gtk.h>

typedef struct Widget Widget;

Widget *widget_new(GtkApplication *app, Config *cfg); /* takes ownership of cfg */
void widget_show(Widget *w);
gboolean widget_configured(const Widget *w);
void widget_open_settings(Widget *w);
void widget_next_verse(Widget *w);
void widget_reload_timings(Widget *w);
