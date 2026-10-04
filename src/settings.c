#include "settings.h"
#include "app.h"
#include "net.h"
#include <math.h>
#include <stdlib.h>

/* https://aladhan.com/calculation-methods */
static const struct { int id; const char *name; } METHODS[] = {
    {0, "Shia Ithna-Ansari (Jafari)"},
    {1, "University of Islamic Sciences, Karachi"},
    {2, "Islamic Society of North America (ISNA)"},
    {3, "Muslim World League"},
    {4, "Umm al-Qura, Makkah"},
    {5, "Egyptian General Authority of Survey"},
    {7, "Institute of Geophysics, Tehran"},
    {8, "Gulf Region"},
    {9, "Kuwait"},
    {10, "Qatar"},
    {11, "Majlis Ugama Islam Singapura"},
    {12, "UOIF, France"},
    {13, "Diyanet, Turkey"},
    {14, "Spiritual Administration of Muslims of Russia"},
    {15, "Moonsighting Committee Worldwide"},
    {16, "Dubai"},
    {17, "JAKIM, Malaysia"},
    {18, "Tunisia"},
    {19, "Algeria"},
    {20, "Kemenag, Indonesia"},
    {21, "Morocco"},
    {22, "Lisbon Islamic Community"},
    {23, "Jordan"},
};

static const struct { const char *country; int method; } METHOD_BY_COUNTRY[] = {
    {"ID", 20}, {"MY", 17}, {"SG", 11}, {"TR", 13}, {"SA", 4},  {"US", 2},  {"CA", 2},  {"EG", 5},
    {"PK", 1},  {"AE", 16}, {"KW", 9},  {"QA", 10}, {"FR", 12}, {"RU", 14}, {"MA", 21}, {"DZ", 19},
    {"TN", 18}, {"PT", 22}, {"JO", 23}, {"IR", 7},  {"BH", 8},  {"OM", 8},
};

static int method_for_country(const char *code)
{
    for (guint i = 0; code && i < G_N_ELEMENTS(METHOD_BY_COUNTRY); i++)
        if (g_ascii_strcasecmp(METHOD_BY_COUNTRY[i].country, code) == 0)
            return METHOD_BY_COUNTRY[i].method;
    return 3;
}

typedef struct {
    int refs;
    gboolean alive; /* FALSE once the window is gone; async results are then dropped */
    GtkWidget *window, *search, *city, *lat, *lon, *tz, *method, *language, *clock, *monitor, *interval, *status;
    GtkListStore *store;
    GPtrArray *places; /* of Place*, indexed by the store's second column */
    gboolean have_location;
    SettingsSaved saved;
    SettingsClosed closed;
    gpointer data;
} Dlg;

static void dlg_unref(Dlg *d)
{
    if (--d->refs > 0) return;
    g_ptr_array_unref(d->places);
    g_free(d);
}

static GtkWidget *section(const char *text)
{
    char *esc = g_markup_escape_text(text, -1);
    char *markup = g_strdup_printf("<b>%s</b>", esc);
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    g_free(esc);
    g_free(markup);
    return l;
}

static void grid_row(GtkWidget *grid, int i, const char *text, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(GTK_GRID(grid), l, 0, i, 1, 1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w, 1, i, 1, 1);
}

static GtkWidget *combo_new(void)
{
    GtkWidget *c = gtk_combo_box_text_new();
    return c;
}

static void combo_select(GtkWidget *combo, const char *id)
{
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), id))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

static void on_coords_edited(GtkSpinButton *spin, gpointer user)
{
    (void)spin;
    ((Dlg *)user)->have_location = TRUE;
}

static void pick_place(Dlg *d, const Place *p)
{
    gtk_entry_set_text(GTK_ENTRY(d->city), p->city);
    g_signal_handlers_block_by_func(d->lat, on_coords_edited, d);
    g_signal_handlers_block_by_func(d->lon, on_coords_edited, d);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->lat), p->latitude);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->lon), p->longitude);
    g_signal_handlers_unblock_by_func(d->lat, on_coords_edited, d);
    g_signal_handlers_unblock_by_func(d->lon, on_coords_edited, d);
    gtk_entry_set_text(GTK_ENTRY(d->tz), p->timezone);
    char id[16];
    g_snprintf(id, sizeof id, "%d", method_for_country(p->country_code));
    combo_select(d->method, id);
    d->have_location = TRUE;
}

static void on_pick(GtkTreeSelection *sel, gpointer user)
{
    Dlg *d = user;
    GtkTreeModel *model;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return;
    int idx;
    gtk_tree_model_get(model, &it, 1, &idx, -1);
    if (idx >= 0 && (guint)idx < d->places->len)
        pick_place(d, g_ptr_array_index(d->places, idx));
}

/* --- async lookups: curl blocks, so run it in a thread and hop back to the main loop --- */
static void search_thread(GTask *task, gpointer src, gpointer query, GCancellable *c)
{
    (void)src; (void)c;
    GError *err = NULL;
    GPtrArray *r = net_search_places(query, &err);
    if (r) g_task_return_pointer(task, r, (GDestroyNotify)g_ptr_array_unref);
    else g_task_return_error(task, err);
}

static void search_done(GObject *src, GAsyncResult *res, gpointer user)
{
    (void)src;
    Dlg *d = user;
    GError *err = NULL;
    GPtrArray *r = g_task_propagate_pointer(G_TASK(res), &err);
    if (d->alive) {
        gtk_list_store_clear(d->store);
        g_ptr_array_set_size(d->places, 0);
        if (err) {
            char *m = g_strdup_printf("Search failed: %s", err->message);
            gtk_label_set_text(GTK_LABEL(d->status), m);
            g_free(m);
        } else if (r->len == 0) {
            gtk_label_set_text(GTK_LABEL(d->status), "No matching place found.");
        } else {
            gtk_label_set_text(GTK_LABEL(d->status), "");
            for (guint i = 0; i < r->len; i++) {
                Place *p = g_ptr_array_index(r, i);
                Place *copy = g_new0(Place, 1);
                *copy = *p;
                copy->city = g_strdup(p->city);
                copy->timezone = g_strdup(p->timezone);
                copy->country_code = g_strdup(p->country_code);
                g_ptr_array_add(d->places, copy);
                GtkTreeIter it;
                gtk_list_store_append(d->store, &it);
                gtk_list_store_set(d->store, &it, 0, p->city, 1, (int)i, -1);
            }
        }
    }
    g_clear_error(&err);
    if (r) g_ptr_array_unref(r);
    dlg_unref(d);
}

static void on_search(GtkWidget *w, gpointer user)
{
    (void)w;
    Dlg *d = user;
    char *q = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(d->search))));
    if (*q) {
        gtk_label_set_text(GTK_LABEL(d->status), "Searching…");
        d->refs++;
        GTask *t = g_task_new(NULL, NULL, search_done, d);
        g_task_set_task_data(t, g_strdup(q), g_free);
        g_task_run_in_thread(t, search_thread);
        g_object_unref(t);
    }
    g_free(q);
}

static void detect_thread(GTask *task, gpointer src, gpointer td, GCancellable *c)
{
    (void)src; (void)td; (void)c;
    GError *err = NULL;
    Place *p = net_detect_place(&err);
    if (p) g_task_return_pointer(task, p, (GDestroyNotify)place_free);
    else g_task_return_error(task, err);
}

static void detect_done(GObject *src, GAsyncResult *res, gpointer user)
{
    (void)src;
    Dlg *d = user;
    GError *err = NULL;
    Place *p = g_task_propagate_pointer(G_TASK(res), &err);
    if (d->alive) {
        if (err) {
            char *m = g_strdup_printf("Detection failed: %s", err->message);
            gtk_label_set_text(GTK_LABEL(d->status), m);
            g_free(m);
        } else {
            gtk_label_set_text(GTK_LABEL(d->status), "");
            pick_place(d, p);
        }
    }
    g_clear_error(&err);
    place_free(p);
    dlg_unref(d);
}

static void on_detect(GtkWidget *w, gpointer user)
{
    (void)w;
    Dlg *d = user;
    gtk_label_set_text(GTK_LABEL(d->status), "Detecting…");
    d->refs++;
    GTask *t = g_task_new(NULL, NULL, detect_done, d);
    g_task_run_in_thread(t, detect_thread);
    g_object_unref(t);
}

/* The new settings, or NULL (with a message shown) when the form is incomplete. */
static Config *dlg_result(Dlg *d)
{
    if (!d->have_location) {
        gtk_label_set_text(GTK_LABEL(d->status), "Search for a city or detect your location first.");
        return NULL;
    }
    char *tz = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(d->tz))));
    if (*tz) {
        GTimeZone *z = tz_load(tz);
        if (!z) {
            char *m = g_strdup_printf("Unknown timezone “%s”. Use a name like Asia/Jakarta.", tz);
            gtk_label_set_text(GTK_LABEL(d->status), m);
            g_free(m);
            g_free(tz);
            return NULL;
        }
        g_time_zone_unref(z);
    }
    Config *c = g_new0(Config, 1);
    c->has_location = TRUE;
    c->latitude = round(gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->lat)) * 10000) / 10000;
    c->longitude = round(gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->lon)) * 10000) / 10000;
    c->timezone = tz;
    char *city = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(d->city))));
    if (*city) {
        c->city = city;
    } else {
        char a[G_ASCII_DTOSTR_BUF_SIZE], b[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(a, sizeof a, "%.4f", c->latitude);
        g_ascii_formatd(b, sizeof b, "%.4f", c->longitude);
        c->city = g_strdup_printf("%s, %s", a, b);
        g_free(city);
    }
    c->method = atoi(gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->method)));
    c->language = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->language)));
    c->clock_24h = g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->clock)), "24") == 0;
    c->monitor = atoi(gtk_combo_box_get_active_id(GTK_COMBO_BOX(d->monitor)));
    c->verse_interval_minutes = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->interval));
    return c;
}

static void on_response(GtkDialog *dialog, int response, gpointer user)
{
    Dlg *d = user;
    if (response == GTK_RESPONSE_OK) {
        Config *c = dlg_result(d);
        if (!c) return; /* keep the window open */
        d->saved(c, d->data);
        config_free(c);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void on_destroy(GtkWidget *w, gpointer user)
{
    (void)w;
    Dlg *d = user;
    d->alive = FALSE;
    if (d->closed) d->closed(d->data);
    dlg_unref(d);
}

GtkWidget *settings_open(const Config *cfg, SettingsSaved saved, SettingsClosed closed, gpointer data)
{
    Dlg *d = g_new0(Dlg, 1);
    d->refs = 1;
    d->alive = TRUE;
    d->places = g_ptr_array_new_with_free_func((GDestroyNotify)place_free);
    d->have_location = cfg->has_location;
    d->saved = saved;
    d->closed = closed;
    d->data = data;

    GtkWidget *win = gtk_dialog_new();
    d->window = win;
    gtk_window_set_title(GTK_WINDOW(win), "MuslimBoard settings");
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_default_size(GTK_WINDOW(win), 480, -1);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE); /* fixed-size windows float by default on Hyprland */
    gtk_dialog_add_button(GTK_DIALOG(win), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(win), "Save", GTK_RESPONSE_OK);
    gtk_dialog_set_default_response(GTK_DIALOG(win), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(win));
    gtk_box_set_spacing(GTK_BOX(box), 10);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);

    gtk_box_pack_start(GTK_BOX(box), section("Location"), FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d->search = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->search), "Search for a city…");
    gtk_widget_set_hexpand(d->search, TRUE);
    g_signal_connect(d->search, "activate", G_CALLBACK(on_search), d);
    GtkWidget *search_btn = gtk_button_new_with_label("Search");
    g_signal_connect(search_btn, "clicked", G_CALLBACK(on_search), d);
    GtkWidget *detect_btn = gtk_button_new_with_label("Detect from IP");
    gtk_widget_set_tooltip_text(detect_btn, "Look up your approximate location from your IP address");
    g_signal_connect(detect_btn, "clicked", G_CALLBACK(on_detect), d);
    gtk_box_pack_start(GTK_BOX(row), d->search, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), search_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), detect_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    d->store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->store));
    g_object_unref(d->store); /* the tree view keeps it alive; d->store stays valid while d->alive */
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree),
        gtk_tree_view_column_new_with_attributes("Place", gtk_cell_renderer_text_new(), "text", 0, NULL));
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), "changed", G_CALLBACK(on_pick), d);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 110);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), tree);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    d->city = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(d->city), cfg->city);
    d->lat = gtk_spin_button_new_with_range(-90, 90, 0.0001);
    d->lon = gtk_spin_button_new_with_range(-180, 180, 0.0001);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->lat), 4);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->lon), 4);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->lat), cfg->has_location ? cfg->latitude : 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->lon), cfg->has_location ? cfg->longitude : 0);
    g_signal_connect(d->lat, "value-changed", G_CALLBACK(on_coords_edited), d);
    g_signal_connect(d->lon, "value-changed", G_CALLBACK(on_coords_edited), d);
    d->tz = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(d->tz), cfg->timezone);
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->tz), "system timezone");
    grid_row(grid, 0, "Display name", d->city);
    grid_row(grid, 1, "Latitude", d->lat);
    grid_row(grid, 2, "Longitude", d->lon);
    grid_row(grid, 3, "Timezone", d->tz);
    GtkWidget *expander = gtk_expander_new("Name, coordinates and timezone");
    gtk_container_add(GTK_CONTAINER(expander), grid);
    gtk_box_pack_start(GTK_BOX(box), expander, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), section("Preferences"), FALSE, FALSE, 6);
    GtkWidget *prefs = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(prefs), 10);
    gtk_grid_set_row_spacing(GTK_GRID(prefs), 8);

    d->method = combo_new();
    for (guint i = 0; i < G_N_ELEMENTS(METHODS); i++) {
        char id[16];
        g_snprintf(id, sizeof id, "%d", METHODS[i].id);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->method), id, METHODS[i].name);
    }
    char buf[16];
    g_snprintf(buf, sizeof buf, "%d", cfg->method);
    combo_select(d->method, buf);

    d->language = combo_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->language), "en", "English");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->language), "id", "Bahasa Indonesia");
    combo_select(d->language, cfg->language);

    d->clock = combo_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->clock), "24", "24-hour");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->clock), "12", "12-hour (AM/PM)");
    combo_select(d->clock, cfg->clock_24h ? "24" : "12");

    d->monitor = combo_new();
    GdkDisplay *display = gdk_display_get_default();
    for (int i = 0; i < gdk_display_get_n_monitors(display); i++) {
        GdkMonitor *m = gdk_display_get_monitor(display, i);
        GdkRectangle g;
        gdk_monitor_get_geometry(m, &g);
        const char *model = gdk_monitor_get_model(m);
        char id[16];
        g_snprintf(id, sizeof id, "%d", i);
        char *label = g_strdup_printf("%d: %s (%d×%d)", i, model ? model : "Monitor", g.width, g.height);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->monitor), id, label);
        g_free(label);
    }
    g_snprintf(buf, sizeof buf, "%d", cfg->monitor);
    combo_select(d->monitor, buf);

    d->interval = gtk_spin_button_new_with_range(1, 1440, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->interval), cfg->verse_interval_minutes);
    grid_row(prefs, 0, "Calculation method", d->method);
    grid_row(prefs, 1, "Verse language", d->language);
    grid_row(prefs, 2, "Clock", d->clock);
    grid_row(prefs, 3, "Monitor", d->monitor);
    grid_row(prefs, 4, "New verse every (min)", d->interval);
    gtk_box_pack_start(GTK_BOX(box), prefs, FALSE, FALSE, 0);

    d->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d->status), 0);
    gtk_label_set_line_wrap(GTK_LABEL(d->status), TRUE);
    gtk_box_pack_start(GTK_BOX(box), d->status, FALSE, FALSE, 0);

    g_signal_connect(win, "response", G_CALLBACK(on_response), d);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), d);
    gtk_widget_show_all(win);
    return win;
}
