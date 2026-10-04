#include "verses.h"
#include "app.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>

static void verse_free(Verse *v)
{
    g_free(v->ar); g_free(v->tr); g_free(v->ref);
    g_free(v);
}

static const char *str_member(JsonObject *o, const char *key)
{
    return json_object_has_member(o, key) ? json_object_get_string_member(o, key) : "";
}

static GPtrArray *load_file(const char *language)
{
    char *path = g_strdup_printf(RES_PREFIX "data-content-%s.json", language);
    GBytes *bytes = g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    g_free(path);
    if (!bytes) return NULL;

    JsonParser *p = json_parser_new();
    gsize len;
    const char *data = g_bytes_get_data(bytes, &len);
    GPtrArray *out = NULL;
    if (json_parser_load_from_data(p, data, len, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(p));
        JsonArray *items = json_object_get_array_member(root, "content");
        out = g_ptr_array_new_with_free_func((GDestroyNotify)verse_free);
        for (guint i = 0; i < json_array_get_length(items); i++) {
            JsonObject *o = json_array_get_object_element(items, i);
            Verse *v = g_new0(Verse, 1);
            v->ar = g_strdup(str_member(o, "matan"));
            v->tr = g_strdup(str_member(o, "translation"));
            v->ref = g_strdup(str_member(o, "reference"));
            g_ptr_array_add(out, v);
        }
    }
    g_object_unref(p);
    g_bytes_unref(bytes);
    return out;
}

GPtrArray *verses_load(const char *language)
{
    GPtrArray *v = load_file(language);
    if (!v || v->len == 0) {
        if (v) g_ptr_array_unref(v);
        v = load_file("en");
    }
    return v;
}
