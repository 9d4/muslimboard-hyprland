#include "app.h"
#include "config.h"
#include "widget.h"
#include <curl/curl.h>
#include <gtk/gtk.h>
#include <string.h>

typedef struct {
    GtkApplication *app;
    Widget *widget;
} App;

static gboolean open_first_run(gpointer widget)
{
    widget_open_settings(widget);
    return G_SOURCE_REMOVE;
}

static void install_css(void)
{
    GdkScreen *screen = gdk_screen_get_default();
    GtkCssProvider *base = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(base, RES_PREFIX "style.css");
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(base), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(base);

    char *dir = config_dir();
    char *path = g_build_filename(dir, "style.css", NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        GtkCssProvider *user = gtk_css_provider_new();
        GError *err = NULL;
        if (gtk_css_provider_load_from_path(user, path, &err))
            gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(user), GTK_STYLE_PROVIDER_PRIORITY_USER);
        else {
            g_printerr("ignoring %s: %s\n", path, err->message);
            g_clear_error(&err);
        }
        g_object_unref(user);
    }
    g_free(path);
    g_free(dir);
}

static int on_command_line(GApplication *ga, GApplicationCommandLine *cl, gpointer user)
{
    App *a = user;
    GVariantDict *opts = g_application_command_line_get_options_dict(cl);
    if (g_variant_dict_contains(opts, "quit")) {
        g_application_quit(ga);
        return 0;
    }
    if (!a->widget) {
        install_css();
        a->widget = widget_new(a->app, config_load());
        widget_show(a->widget);
        if (!widget_configured(a->widget)) g_idle_add(open_first_run, a->widget);
    }
    if (g_variant_dict_contains(opts, "settings")) widget_open_settings(a->widget);
    if (g_variant_dict_contains(opts, "next-verse")) widget_next_verse(a->widget);
    if (g_variant_dict_contains(opts, "refresh")) widget_reload_timings(a->widget);
    return 0;
}

int main(int argc, char **argv)
{
    g_set_prgname(APP_ID); /* window class (Wayland app_id), used by Hyprland window rules */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    App a = {0};
    a.app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    const GOptionEntry entries[] = {
        {"settings", 0, 0, G_OPTION_ARG_NONE, NULL, "open the settings window", NULL},
        {"next-verse", 0, 0, G_OPTION_ARG_NONE, NULL, "show another verse now", NULL},
        {"refresh", 0, 0, G_OPTION_ARG_NONE, NULL, "re-download prayer times", NULL},
        {"quit", 0, 0, G_OPTION_ARG_NONE, NULL, "stop the widget", NULL},
        {NULL},
    };
    g_application_add_main_option_entries(G_APPLICATION(a.app), entries);
    g_application_set_option_context_summary(G_APPLICATION(a.app), "Start the widget, or control the running one.");
    g_signal_connect(a.app, "command-line", G_CALLBACK(on_command_line), &a);
    int rc = g_application_run(G_APPLICATION(a.app), argc, argv);
    g_object_unref(a.app);
    curl_global_cleanup();
    return rc;
}
