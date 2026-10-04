#include "widget.h"
#include "app.h"
#include "net.h"
#include "settings.h"
#include "verses.h"
#include <gtk-layer-shell.h>

static const char *const PRAYER_LABELS[PRAYER_COUNT] = {"FAJR", "SUNRISE", "DHUHR", "ASR", "MAGHRIB", "ISHA"};

struct Widget {
    GtkApplication *app;
    Config *cfg;
    GtkWidget *window, *left, *verse_box, *menu;
    GtkWidget *clock, *date_label, *hijri_row, *hijri_label, *city_label, *title, *eta;
    GtkWidget *v_ar, *v_tr, *v_ref;
    GtkWidget *row[PRAYER_COUNT], *cell[PRAYER_COUNT][3];
    GtkWidget *dialog;

    GTimeZone *tz; /* NULL = system timezone */
    GDateTime *times[PRAYER_COUNT];
    GPtrArray *verses;
    guint verse_timer;
    guint generation; /* bumped on every settings change to drop stale fetches */
    gboolean fetching;
    gint64 last_try;
    int monitor_index;
    gboolean region_pending;
};

static void apply_config(Widget *w);
static void refresh_timings(Widget *w);
static gboolean tick(gpointer user);

/* --- time helpers --- */
static GDateTime *now(Widget *w)
{
    GTimeZone *tz = w->tz ? g_time_zone_ref(w->tz) : g_time_zone_new_local();
    GDateTime *n = g_date_time_new_now(tz);
    g_time_zone_unref(tz);
    return n;
}

static char *fmt_time(const Widget *w, GDateTime *dt, gboolean seconds)
{
    int h = g_date_time_get_hour(dt), m = g_date_time_get_minute(dt), s = g_date_time_get_second(dt);
    char sec[8] = "";
    if (seconds) g_snprintf(sec, sizeof sec, ":%02d", s);
    if (w->cfg->clock_24h) return g_strdup_printf("%02d:%02d%s", h, m, sec);
    return g_strdup_printf("%d:%02d%s %s", h % 12 ? h % 12 : 12, m, sec, h < 12 ? "AM" : "PM");
}

static gboolean same_day(GDateTime *a, GDateTime *b)
{
    int ay, am, ad, by, bm, bd;
    g_date_time_get_ymd(a, &ay, &am, &ad);
    g_date_time_get_ymd(b, &by, &bm, &bd);
    return ay == by && am == bm && ad == bd;
}

static void clear_times(Widget *w)
{
    for (int i = 0; i < PRAYER_COUNT; i++) g_clear_pointer(&w->times[i], g_date_time_unref);
}

/* --- drawing --- */
/* A transparent window keeps the previous frame; wipe it so old text shadows don't pile up. */
static gboolean clear_frame(GtkWidget *win, cairo_t *cr, gpointer user)
{
    (void)win; (void)user;
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    return FALSE;
}

/* Only the widget's own areas take clicks; the rest of the desktop stays click-through. */
static gboolean update_input_region(gpointer user)
{
    Widget *w = user;
    w->region_pending = FALSE;
    GdkWindow *gw = gtk_widget_get_window(w->window);
    if (!gw) return G_SOURCE_REMOVE;
    cairo_region_t *region = cairo_region_create();
    GtkWidget *kids[] = {w->left, w->verse_box};
    for (int i = 0; i < 2; i++) {
        GtkAllocation a;
        int x, y;
        gtk_widget_get_allocation(kids[i], &a);
        /* allocations are relative to the overlay child's own window */
        if (!gtk_widget_translate_coordinates(kids[i], w->window, 0, 0, &x, &y)) continue;
        cairo_rectangle_int_t r = {x, y, a.width, a.height};
        cairo_region_union_rectangle(region, &r);
    }
    gdk_window_input_shape_combine_region(gw, region, 0, 0);
    cairo_region_destroy(region);
    return G_SOURCE_REMOVE;
}

static void on_size_allocate(GtkWidget *wd, GdkRectangle *a, gpointer user)
{
    (void)wd; (void)a;
    Widget *w = user;
    if (w->region_pending) return;
    w->region_pending = TRUE;
    g_idle_add(update_input_region, w);
}

/* --- verses --- */
static void show_verse(Widget *w)
{
    const char *current = gtk_label_get_text(GTK_LABEL(w->v_ar));
    GArray *choices = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; i < w->verses->len; i++) {
        Verse *v = g_ptr_array_index(w->verses, i);
        if (g_strcmp0(v->ar, current) != 0) g_array_append_val(choices, i);
    }
    guint pick = choices->len ? g_array_index(choices, guint, g_random_int_range(0, choices->len))
                              : (guint)g_random_int_range(0, w->verses->len);
    g_array_free(choices, TRUE);
    Verse *v = g_ptr_array_index(w->verses, pick);
    gtk_label_set_text(GTK_LABEL(w->v_ar), v->ar);
    gtk_label_set_text(GTK_LABEL(w->v_tr), v->tr);
    gtk_label_set_text(GTK_LABEL(w->v_ref), v->ref);
    /* marks above the Arabic line draw outside its box; repaint everything so none are left behind */
    gtk_widget_queue_draw(w->window);
}

static gboolean on_verse_timer(gpointer user)
{
    show_verse(user);
    return G_SOURCE_CONTINUE;
}

static void restart_verse_timer(Widget *w)
{
    if (w->verse_timer) g_source_remove(w->verse_timer);
    int minutes = MAX(1, w->cfg->verse_interval_minutes);
    w->verse_timer = g_timeout_add_seconds(minutes * 60, on_verse_timer, w);
}

void widget_next_verse(Widget *w)
{
    show_verse(w);
    restart_verse_timer(w);
}

/* --- prayer times --- */
typedef struct {
    Config *cfg;
    int y, m, d;
    guint generation;
} FetchJob;

static void fetch_job_free(FetchJob *j)
{
    config_free(j->cfg);
    g_free(j);
}

static void fetch_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    FetchJob *j = data;
    GError *err = NULL;
    Timings *t = net_fetch_timings(j->cfg, j->y, j->m, j->d, &err);
    if (t) g_task_return_pointer(task, t, (GDestroyNotify)timings_free);
    else g_task_return_error(task, err);
}

static void apply_timings(Widget *w, const Timings *t, const FetchJob *j);
static void highlight(Widget *w);

static void fetch_done(GObject *src, GAsyncResult *res, gpointer user)
{
    (void)src;
    Widget *w = user;
    FetchJob *j = g_task_get_task_data(G_TASK(res));
    GError *err = NULL;
    Timings *t = g_task_propagate_pointer(G_TASK(res), &err);
    if (j->generation == w->generation) {
        w->fetching = FALSE;
        if (t) {
            apply_timings(w, t, j);
        } else {
            g_printerr("prayer times fetch failed: %s\n", err->message);
            if (!w->times[0]) gtk_label_set_text(GTK_LABEL(w->title), "Prayer times unavailable, retrying…");
        }
    }
    g_clear_error(&err);
    timings_free(t);
}

static void refresh_timings(Widget *w)
{
    if (!w->cfg->has_location || w->fetching) return;
    w->fetching = TRUE;
    w->last_try = g_get_monotonic_time();
    FetchJob *j = g_new0(FetchJob, 1);
    j->cfg = config_copy(w->cfg);
    j->generation = w->generation;
    GDateTime *n = now(w);
    g_date_time_get_ymd(n, &j->y, &j->m, &j->d);
    g_date_time_unref(n);
    GTask *task = g_task_new(NULL, NULL, fetch_done, w);
    g_task_set_task_data(task, j, (GDestroyNotify)fetch_job_free);
    g_task_run_in_thread(task, fetch_thread);
    g_object_unref(task);
}

static gboolean refresh_hourly(gpointer user)
{
    refresh_timings(user);
    return G_SOURCE_CONTINUE;
}

void widget_reload_timings(Widget *w)
{
    net_clear_cache();
    w->generation++;
    w->fetching = FALSE;
    refresh_timings(w);
}

static void apply_timings(Widget *w, const Timings *t, const FetchJob *j)
{
    (void)j;
    GTimeZone *tz = w->tz ? g_time_zone_ref(w->tz) : g_time_zone_new_local();
    clear_times(w);
    for (int i = 0; i < PRAYER_COUNT; i++)
        w->times[i] = g_date_time_new(tz, j->y, j->m, j->d, t->hour[i], t->minute[i], 0);
    g_time_zone_unref(tz);

    gtk_label_set_text(GTK_LABEL(w->hijri_label), t->hijri);
    gtk_widget_show(w->hijri_row);
    gtk_label_set_text(GTK_LABEL(w->title), "Estimated prayer times for today:");
    const char *tzname = g_date_time_get_timezone_abbreviation(w->times[0]);
    for (int i = 0; i < PRAYER_COUNT; i++) {
        char *s = fmt_time(w, w->times[i], FALSE);
        gtk_label_set_text(GTK_LABEL(w->cell[i][1]), s);
        gtk_label_set_text(GTK_LABEL(w->cell[i][2]), tzname);
        g_free(s);
    }
    highlight(w);
}

static void highlight(Widget *w)
{
    if (!w->times[0]) return;
    GDateTime *n = now(w);
    int next = -1;
    for (int i = 0; i < PRAYER_COUNT; i++)
        if (g_date_time_compare(w->times[i], n) > 0) { next = i; break; }
    GDateTime *when = next >= 0 ? g_date_time_ref(w->times[next]) : g_date_time_add_days(w->times[0], 1);
    if (next < 0) next = 0; /* after Isha the next one is tomorrow's Fajr */
    int mins = (int)(g_date_time_difference(when, n) / G_TIME_SPAN_MINUTE) + 1;
    char *eta = mins >= 60 ? g_strdup_printf("in %d hour(s) %d minute(s)", mins / 60, mins % 60)
                           : g_strdup_printf("in %d minute(s)", mins);
    gtk_label_set_text(GTK_LABEL(w->eta), eta);
    gtk_widget_show(w->eta);
    g_free(eta);
    for (int i = 0; i < PRAYER_COUNT; i++) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(w->row[i]);
        if (i == next) gtk_style_context_add_class(ctx, "next");
        else gtk_style_context_remove_class(ctx, "next");
    }
    g_date_time_unref(when);
    g_date_time_unref(n);
}

static gboolean tick(gpointer user)
{
    Widget *w = user;
    GDateTime *n = now(w);
    char *t = fmt_time(w, n, FALSE), *ts = fmt_time(w, n, TRUE);
    gtk_label_set_text(GTK_LABEL(w->clock), t);
    char *day = g_date_time_format(n, "%A, %d %B %Y");
    char *line = g_strdup_printf("%s %s %s", day, ts, g_date_time_get_timezone_abbreviation(n));
    gtk_label_set_text(GTK_LABEL(w->date_label), line);
    if (w->cfg->has_location) {
        gboolean stale = !w->times[0] || !same_day(w->times[0], n);
        if (stale && !w->fetching && g_get_monotonic_time() - w->last_try > 30 * G_USEC_PER_SEC)
            refresh_timings(w);
        highlight(w);
    }
    g_free(t); g_free(ts); g_free(day); g_free(line);
    g_date_time_unref(n);
    return G_SOURCE_CONTINUE;
}

/* --- settings --- */
static void on_settings_saved(const Config *cfg, gpointer user)
{
    Widget *w = user;
    Config *copy = config_copy(cfg);
    config_free(w->cfg);
    w->cfg = copy;
    GError *err = NULL;
    if (!config_save(w->cfg, &err)) {
        g_printerr("could not save settings: %s\n", err->message);
        g_clear_error(&err);
    }
    apply_config(w);
}

static void on_settings_closed(gpointer user)
{
    ((Widget *)user)->dialog = NULL;
}

void widget_open_settings(Widget *w)
{
    if (w->dialog) gtk_window_present(GTK_WINDOW(w->dialog));
    else w->dialog = settings_open(w->cfg, on_settings_saved, on_settings_closed, w);
}

static void apply_config(Widget *w)
{
    Config *cfg = w->cfg;
    w->generation++;
    w->fetching = FALSE;
    w->last_try = 0;
    clear_times(w);
    g_clear_pointer(&w->tz, g_time_zone_unref);
    w->tz = tz_load(cfg->timezone);

    GdkDisplay *display = gdk_display_get_default();
    int index = CLAMP(cfg->monitor, 0, gdk_display_get_n_monitors(display) - 1);
    GdkMonitor *monitor = gdk_display_get_monitor(display, index);
    if (index != w->monitor_index) {
        gboolean mapped = gtk_widget_get_mapped(w->window);
        if (mapped) gtk_widget_hide(w->window);
        gtk_layer_set_monitor(GTK_WINDOW(w->window), monitor);
        w->monitor_index = index;
        if (mapped) gtk_widget_show_all(w->window);
    }
    GdkRectangle geo;
    gdk_monitor_get_geometry(monitor, &geo);
    GtkWidget *verse_labels[] = {w->v_ar, w->v_tr, w->v_ref};
    for (int i = 0; i < 3; i++) gtk_widget_set_size_request(verse_labels[i], (int)(geo.width * 0.55), -1);

    int time_width = cfg->clock_24h ? 58 : 84;
    for (int i = 0; i < PRAYER_COUNT; i++) {
        gtk_widget_set_size_request(w->cell[i][1], time_width, -1);
        gtk_label_set_text(GTK_LABEL(w->cell[i][1]), "");
        gtk_label_set_text(GTK_LABEL(w->cell[i][2]), "");
        gtk_style_context_remove_class(gtk_widget_get_style_context(w->row[i]), "next");
    }
    gtk_label_set_text(GTK_LABEL(w->hijri_label), "");
    gtk_widget_hide(w->hijri_row);
    gtk_widget_hide(w->eta);

    if (w->verses) g_ptr_array_unref(w->verses);
    w->verses = verses_load(cfg->language);
    show_verse(w);
    restart_verse_timer(w);

    if (cfg->has_location) {
        gtk_label_set_text(GTK_LABEL(w->city_label), cfg->city);
        gtk_label_set_text(GTK_LABEL(w->title), "Estimated prayer times for today:");
        refresh_timings(w);
    } else {
        gtk_label_set_text(GTK_LABEL(w->city_label), "Click here to set your location");
        gtk_label_set_text(GTK_LABEL(w->title), "Prayer times need a location.");
    }
    tick(w);
}

/* --- input --- */
static gboolean on_press(GtkWidget *win, GdkEventButton *ev, gpointer user)
{
    (void)win;
    Widget *w = user;
    if (ev->button == 3) {
        gtk_menu_popup_at_pointer(GTK_MENU(w->menu), (GdkEvent *)ev);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_location_click(GtkWidget *wd, GdkEventButton *ev, gpointer user)
{
    (void)wd;
    if (ev->button != 1) return FALSE;
    widget_open_settings(user);
    return TRUE;
}

static gboolean on_verse_click(GtkWidget *wd, GdkEventButton *ev, gpointer user)
{
    (void)wd;
    if (ev->button != 1) return FALSE;
    widget_next_verse(user);
    return TRUE;
}

static void menu_settings(GtkMenuItem *i, gpointer w) { (void)i; widget_open_settings(w); }
static void menu_next(GtkMenuItem *i, gpointer w) { (void)i; widget_next_verse(w); }
static void menu_refresh(GtkMenuItem *i, gpointer w) { (void)i; widget_reload_timings(w); }
static void menu_quit(GtkMenuItem *i, gpointer w) { (void)i; g_application_quit(G_APPLICATION(((Widget *)w)->app)); }

/* --- layout --- */
static GtkWidget *build_menu(Widget *w)
{
    GtkWidget *menu = gtk_menu_new();
    const struct { const char *label; GCallback cb; } items[] = {
        {"Settings…", G_CALLBACK(menu_settings)},
        {"Next verse", G_CALLBACK(menu_next)},
        {"Refresh prayer times", G_CALLBACK(menu_refresh)},
        {NULL, NULL},
        {"Quit", G_CALLBACK(menu_quit)},
    };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *item = items[i].label ? gtk_menu_item_new_with_label(items[i].label) : gtk_separator_menu_item_new();
        if (items[i].cb) g_signal_connect(item, "activate", items[i].cb, w);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    return menu;
}

static void add_class(GtkWidget *wd, const char *css)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(wd), css);
}

static GtkWidget *label_new(const char *css)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (css) add_class(l, css);
    return l;
}

static GtkWidget *icon_label(const char *icon, GtkWidget **label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 12);
    *label = label_new("meta");
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), *label, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *build_left(Widget *w)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(col, GTK_ALIGN_START);
    gtk_widget_set_valign(col, GTK_ALIGN_START);
    gtk_widget_set_margin_start(col, 34);
    gtk_widget_set_margin_top(col, 24);

    w->clock = label_new("clock");
    gtk_box_pack_start(GTK_BOX(col), w->clock, FALSE, FALSE, 0);

    GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(meta, 18);
    gtk_box_pack_start(GTK_BOX(meta), icon_label("x-office-calendar-symbolic", &w->date_label), FALSE, FALSE, 0);
    w->hijri_row = icon_label("x-office-calendar-symbolic", &w->hijri_label);
    gtk_widget_show_all(w->hijri_row);
    gtk_widget_set_no_show_all(w->hijri_row, TRUE); /* shown once prayer times arrive */
    gtk_box_pack_start(GTK_BOX(meta), w->hijri_row, FALSE, FALSE, 0);
    GtkWidget *loc = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(loc), FALSE);
    gtk_widget_set_tooltip_text(loc, "Click to change location · right-click for menu");
    gtk_container_add(GTK_CONTAINER(loc), icon_label("mark-location-symbolic", &w->city_label));
    g_signal_connect(loc, "button-press-event", G_CALLBACK(on_location_click), w);
    gtk_box_pack_start(GTK_BOX(meta), loc, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col), meta, FALSE, FALSE, 0);

    w->title = label_new("small-title");
    gtk_widget_set_margin_top(w->title, 26);
    gtk_widget_set_margin_start(w->title, 5);
    gtk_box_pack_start(GTK_BOX(col), w->title, FALSE, FALSE, 0);

    GtkWidget *listing = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(listing, 6);
    static const char *const CELL_CSS[3] = {"prayer-name", "prayer-time", "prayer-tz"};
    for (int i = 0; i < PRAYER_COUNT; i++) {
        w->row[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        for (int j = 0; j < 3; j++) {
            GtkWidget *l = label_new(CELL_CSS[j]);
            gtk_widget_set_margin_start(l, j == 0 ? 5 : 0);
            gtk_widget_set_margin_end(l, j == 2 ? 5 : 0);
            gtk_box_pack_start(GTK_BOX(w->row[i]), l, FALSE, FALSE, 0);
            w->cell[i][j] = l;
        }
        gtk_widget_set_size_request(w->cell[i][0], 88, -1);
        gtk_label_set_text(GTK_LABEL(w->cell[i][0]), PRAYER_LABELS[i]);
        gtk_box_pack_start(GTK_BOX(listing), w->row[i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(col), listing, FALSE, FALSE, 0);

    w->eta = gtk_label_new("");
    gtk_widget_set_no_show_all(w->eta, TRUE);
    add_class(w->eta, "next-eta");
    gtk_box_pack_start(GTK_BOX(col), w->eta, FALSE, FALSE, 0);
    return col;
}

static GtkWidget *build_verse(Widget *w)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget **labels[] = {&w->v_ar, &w->v_tr, &w->v_ref};
    static const char *const css[] = {"arabic", "quote-en", "quote-ref"};
    for (int i = 0; i < 3; i++) {
        GtkWidget *l = gtk_label_new("");
        if (i < 2) {
            gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
            gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        }
        add_class(l, css[i]);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 80);
        gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
        gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
        *labels[i] = l;
    }
    /* alignment lives on the event box so it only covers the verse, not the whole window */
    GtkWidget *box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(box), FALSE);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_START);
    gtk_widget_set_margin_top(box, 120);
    gtk_widget_set_margin_start(box, 300); /* keep the verse clear of the left column */
    gtk_widget_set_tooltip_text(box, "Click for the next verse · right-click for menu");
    gtk_container_add(GTK_CONTAINER(box), col);
    g_signal_connect(box, "button-press-event", G_CALLBACK(on_verse_click), w);
    return box;
}

Widget *widget_new(GtkApplication *app, Config *cfg)
{
    Widget *w = g_new0(Widget, 1);
    w->app = app;
    w->cfg = cfg;
    w->monitor_index = -1;

    GtkWidget *win = gtk_application_window_new(app);
    w->window = win;
    gtk_window_set_title(GTK_WINDOW(win), "MuslimBoard");
    gtk_widget_set_app_paintable(win, TRUE);
    add_class(win, "mb");
    GdkVisual *rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(win));
    if (rgba) gtk_widget_set_visual(win, rgba);

    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_BOTTOM);
    gtk_layer_set_namespace(GTK_WINDOW(win), "muslimboard");
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
    for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++) gtk_layer_set_anchor(GTK_WINDOW(win), e, TRUE);

    g_signal_connect(win, "draw", G_CALLBACK(clear_frame), NULL);
    w->menu = build_menu(w);
    gtk_widget_add_events(win, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(win, "button-press-event", G_CALLBACK(on_press), w);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    w->left = build_left(w);
    w->verse_box = build_verse(w);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), w->left);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), w->verse_box);
    GtkWidget *watch[] = {overlay, w->left, w->verse_box};
    for (int i = 0; i < 3; i++) g_signal_connect(watch[i], "size-allocate", G_CALLBACK(on_size_allocate), w);
    gtk_container_add(GTK_CONTAINER(win), overlay);

    apply_config(w);
    g_timeout_add_seconds(1, tick, w);
    g_timeout_add_seconds(3600, refresh_hourly, w);
    return w;
}

void widget_show(Widget *w)
{
    gtk_widget_show_all(w->window);
}

gboolean widget_configured(const Widget *w)
{
    return w->cfg->has_location;
}
