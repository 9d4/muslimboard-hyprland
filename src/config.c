#include "config.h"
#include <glib/gstdio.h>
#include "app.h"
#include <json-glib/json-glib.h>
#include <stdio.h>

static Config *config_default(void)
{
    Config *c = g_new0(Config, 1);
    c->city = g_strdup("");
    c->timezone = g_strdup("");
    c->method = 3;
    c->language = g_strdup("en");
    c->clock_24h = TRUE;
    c->verse_interval_minutes = VERSE_INTERVAL_DEFAULT;
    return c;
}

char *config_dir(void)
{
    return g_build_filename(g_get_user_config_dir(), "muslimboard", NULL);
}

static char *config_path(void)
{
    char *dir = config_dir();
    char *path = g_build_filename(dir, "config.json", NULL);
    g_free(dir);
    return path;
}

static const char *get_string(JsonObject *o, const char *key)
{
    JsonNode *n = json_object_get_member(o, key);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}

Config *config_load(void)
{
    Config *c = config_default();
    char *path = config_path();
    JsonParser *p = json_parser_new();
    GError *err = NULL;

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        goto out;
    if (!json_parser_load_from_file(p, path, &err)) {
        g_warning("ignoring unreadable %s: %s", path, err->message);
        g_clear_error(&err);
        goto out;
    }
    JsonNode *root = json_parser_get_root(p);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        goto out;
    JsonObject *o = json_node_get_object(root);
    const char *s;
    JsonNode *n;

    if ((s = get_string(o, "city"))) { g_free(c->city); c->city = g_strdup(s); }
    if ((s = get_string(o, "timezone"))) { g_free(c->timezone); c->timezone = g_strdup(s); }
    if ((s = get_string(o, "language"))) { g_free(c->language); c->language = g_strdup(s); }

    JsonNode *lat = json_object_get_member(o, "latitude");
    JsonNode *lon = json_object_get_member(o, "longitude");
    if (lat && lon && JSON_NODE_HOLDS_VALUE(lat) && JSON_NODE_HOLDS_VALUE(lon)) { /* a JSON null is not a value */
        c->has_location = TRUE;
        c->latitude = json_node_get_double(lat);
        c->longitude = json_node_get_double(lon);
    }
    if ((n = json_object_get_member(o, "method")) && JSON_NODE_HOLDS_VALUE(n)) c->method = json_node_get_int(n);
    if ((n = json_object_get_member(o, "monitor")) && JSON_NODE_HOLDS_VALUE(n)) c->monitor = json_node_get_int(n);
    if ((n = json_object_get_member(o, "clock_24h")) && JSON_NODE_HOLDS_VALUE(n)) c->clock_24h = json_node_get_boolean(n);
    if ((n = json_object_get_member(o, "verse_interval_minutes")) && JSON_NODE_HOLDS_VALUE(n))
        c->verse_interval_minutes = json_node_get_int(n);
out:
    g_object_unref(p);
    g_free(path);
    return c;
}

gboolean config_save(const Config *c, GError **err)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "city");
    json_builder_add_string_value(b, c->city);
    json_builder_set_member_name(b, "latitude");
    if (c->has_location) json_builder_add_double_value(b, c->latitude); else json_builder_add_null_value(b);
    json_builder_set_member_name(b, "longitude");
    if (c->has_location) json_builder_add_double_value(b, c->longitude); else json_builder_add_null_value(b);
    json_builder_set_member_name(b, "timezone");
    json_builder_add_string_value(b, c->timezone);
    json_builder_set_member_name(b, "method");
    json_builder_add_int_value(b, c->method);
    json_builder_set_member_name(b, "language");
    json_builder_add_string_value(b, c->language);
    json_builder_set_member_name(b, "clock_24h");
    json_builder_add_boolean_value(b, c->clock_24h);
    json_builder_set_member_name(b, "monitor");
    json_builder_add_int_value(b, c->monitor);
    json_builder_set_member_name(b, "verse_interval_minutes");
    json_builder_add_int_value(b, c->verse_interval_minutes);
    json_builder_end_object(b);

    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    json_generator_set_pretty(g, TRUE);
    json_generator_set_indent(g, 2);
    gsize len;
    char *data = json_generator_to_data(g, &len);

    char *dir = config_dir();
    char *path = config_path();
    char *tmp = g_strconcat(path, ".tmp", NULL);
    gboolean ok = FALSE;
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "cannot create %s", dir);
    } else {
        char *text = g_strconcat(data, "\n", NULL); /* write atomically: temp file, then rename */
        ok = g_file_set_contents(tmp, text, -1, err) && (g_rename(tmp, path) == 0 || (g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED, "cannot replace %s", path), FALSE));
        g_free(text);
    }
    g_free(tmp); g_free(path); g_free(dir); g_free(data);
    json_node_unref(root);
    g_object_unref(g);
    g_object_unref(b);
    return ok;
}

Config *config_copy(const Config *c)
{
    Config *n = g_memdup2(c, sizeof *c);
    n->city = g_strdup(c->city);
    n->timezone = g_strdup(c->timezone);
    n->language = g_strdup(c->language);
    return n;
}

void config_free(Config *c)
{
    if (!c) return;
    g_free(c->city);
    g_free(c->timezone);
    g_free(c->language);
    g_free(c);
}
