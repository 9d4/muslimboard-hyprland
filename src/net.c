#include "net.h"
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <time.h>

/* --- http --- */
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    g_string_append_len(userdata, ptr, size * nmemb);
    return size * nmemb;
}

static char *http_get(const char *url, GError **err)
{
    CURL *c = curl_easy_init();
    GString *buf = g_string_new(NULL);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); /* we call this from threads */
    curl_easy_setopt(c, CURLOPT_USERAGENT, "muslimboard");
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", curl_easy_strerror(rc));
        g_string_free(buf, TRUE);
        return NULL;
    }
    return g_string_free(buf, FALSE);
}

/* Parses `body`; the returned object is owned by *parser, which the caller unrefs. */
static JsonObject *parse_object(const char *body, JsonParser **parser, GError **err)
{
    *parser = json_parser_new();
    if (!json_parser_load_from_data(*parser, body, -1, err))
        return NULL;
    JsonNode *root = json_parser_get_root(*parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unexpected response");
        return NULL;
    }
    return json_node_get_object(root);
}

static const char *str_member(JsonObject *o, const char *key)
{
    JsonNode *n = json_object_get_member(o, key);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
}

static char *join_place(const char *a, const char *b, const char *c)
{
    const char *parts[] = {a, b, c};
    GString *s = g_string_new(NULL);
    for (int i = 0; i < 3; i++) {
        if (!parts[i] || !*parts[i]) continue;
        if (s->len) g_string_append(s, ", ");
        g_string_append(s, parts[i]);
    }
    return g_string_free(s, FALSE);
}

void place_free(Place *p)
{
    if (!p) return;
    g_free(p->city); g_free(p->timezone); g_free(p->country_code);
    g_free(p);
}

void timings_free(Timings *t)
{
    if (!t) return;
    g_free(t->hijri);
    g_free(t);
}

/* --- places --- */
GPtrArray *net_search_places(const char *query, GError **err)
{
    char *q = g_uri_escape_string(query, NULL, FALSE);
    char *url = g_strdup_printf("https://geocoding-api.open-meteo.com/v1/search?name=%s&count=8&language=en&format=json", q);
    g_free(q);
    char *body = http_get(url, err);
    g_free(url);
    if (!body) return NULL;

    JsonParser *parser;
    JsonObject *root = parse_object(body, &parser, err);
    g_free(body);
    if (!root) { g_object_unref(parser); return NULL; }

    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)place_free);
    JsonArray *results = json_object_has_member(root, "results") ? json_object_get_array_member(root, "results") : NULL;
    for (guint i = 0; results && i < json_array_get_length(results); i++) {
        JsonObject *r = json_array_get_object_element(results, i);
        Place *p = g_new0(Place, 1);
        p->city = join_place(str_member(r, "name"), str_member(r, "admin1"), str_member(r, "country"));
        p->latitude = json_object_get_double_member(r, "latitude");
        p->longitude = json_object_get_double_member(r, "longitude");
        p->timezone = g_strdup(str_member(r, "timezone"));
        p->country_code = g_strdup(str_member(r, "country_code"));
        g_ptr_array_add(out, p);
    }
    g_object_unref(parser);
    return out;
}

Place *net_detect_place(GError **err)
{
    char *body = http_get("https://ipwho.is/", err);
    if (!body) return NULL;
    JsonParser *parser;
    JsonObject *o = parse_object(body, &parser, err);
    g_free(body);
    if (!o) { g_object_unref(parser); return NULL; }

    Place *p = NULL;
    if (!json_object_has_member(o, "success") || !json_object_get_boolean_member(o, "success")) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", str_member(o, "message")[0] ? str_member(o, "message") : "lookup failed");
    } else {
        p = g_new0(Place, 1);
        p->city = join_place(str_member(o, "city"), str_member(o, "region"), str_member(o, "country"));
        p->latitude = json_object_get_double_member(o, "latitude");
        p->longitude = json_object_get_double_member(o, "longitude");
        p->country_code = g_strdup(str_member(o, "country_code"));
        JsonObject *tz = json_object_has_member(o, "timezone") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, "timezone"))
                             ? json_object_get_object_member(o, "timezone") : NULL;
        p->timezone = g_strdup(tz ? str_member(tz, "id") : "");
    }
    g_object_unref(parser);
    return p;
}

/* --- prayer times --- */
static const char *const API_KEYS[PRAYER_COUNT] = {"Fajr", "Sunrise", "Dhuhr", "Asr", "Maghrib", "Isha"};

static char *node_text(JsonNode *n) /* Aladhan sends some numbers as strings, some as ints */
{
    if (!n || !JSON_NODE_HOLDS_VALUE(n)) return g_strdup("");
    if (json_node_get_value_type(n) == G_TYPE_STRING) return g_strdup(json_node_get_string(n));
    return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(n));
}

static Timings *parse_timings(const char *body, GError **err)
{
    JsonParser *parser;
    JsonObject *root = parse_object(body, &parser, err);
    if (!root) { g_object_unref(parser); return NULL; }

    Timings *t = NULL;
    JsonObject *data = json_object_has_member(root, "data") ? json_object_get_object_member(root, "data") : NULL;
    JsonObject *times = data && json_object_has_member(data, "timings") ? json_object_get_object_member(data, "timings") : NULL;
    JsonObject *date = data && json_object_has_member(data, "date") ? json_object_get_object_member(data, "date") : NULL;
    JsonObject *hijri = date && json_object_has_member(date, "hijri") ? json_object_get_object_member(date, "hijri") : NULL;
    if (!times || !hijri) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unexpected prayer times response");
        goto out;
    }
    t = g_new0(Timings, 1);
    for (int i = 0; i < PRAYER_COUNT; i++) { /* values look like "04:03" or "04:03 (WIB)" */
        if (sscanf(str_member(times, API_KEYS[i]), "%d:%d", &t->hour[i], &t->minute[i]) != 2) {
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "bad time for %s", API_KEYS[i]);
            timings_free(t);
            t = NULL;
            goto out;
        }
    }
    JsonObject *month = json_object_has_member(hijri, "month") ? json_object_get_object_member(hijri, "month") : NULL;
    char *day = node_text(json_object_get_member(hijri, "day"));
    char *year = node_text(json_object_get_member(hijri, "year"));
    t->hijri = g_strdup_printf("%d %s %s AH", atoi(day), month ? str_member(month, "en") : "", year);
    g_free(day);
    g_free(year);
out:
    g_object_unref(parser);
    return t;
}

static char *cache_dir(void)
{
    return g_build_filename(g_get_user_cache_dir(), "muslimboard", NULL);
}

static void prune_cache(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name(d))) {
        char *path = g_build_filename(dir, name, NULL);
        GStatBuf st;
        if (g_str_has_suffix(name, ".json") && g_stat(path, &st) == 0 && time(NULL) - st.st_mtime > 3 * 86400)
            g_unlink(path);
        g_free(path);
    }
    if (d) g_dir_close(d);
}

void net_clear_cache(void)
{
    char *dir = cache_dir();
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".json")) continue;
        char *path = g_build_filename(dir, name, NULL);
        g_unlink(path);
        g_free(path);
    }
    if (d) g_dir_close(d);
    g_free(dir);
}

Timings *net_fetch_timings(const Config *cfg, int y, int m, int d, GError **err)
{
    char lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(lat, sizeof lat, "%.4f", cfg->latitude); /* never locale-dependent: it goes into URLs and file names */
    g_ascii_formatd(lon, sizeof lon, "%.4f", cfg->longitude);

    char *dir = cache_dir();
    g_mkdir_with_parents(dir, 0755);
    char *name = g_strdup_printf("%04d-%02d-%02d_%s_%s_%d.json", y, m, d, lat, lon, cfg->method);
    char *path = g_build_filename(dir, name, NULL);
    g_free(name);

    Timings *t = NULL;
    char *body = NULL;
    if (g_file_get_contents(path, &body, NULL, NULL)) {
        t = parse_timings(body, NULL);
        g_free(body);
        body = NULL;
    }
    if (!t) {
        GString *url = g_string_new(NULL);
        g_string_printf(url, "https://api.aladhan.com/v1/timings/%02d-%02d-%04d?latitude=%s&longitude=%s&method=%d", d, m, y, lat, lon, cfg->method);
        if (cfg->timezone[0]) {
            char *tz = g_uri_escape_string(cfg->timezone, NULL, FALSE);
            g_string_append_printf(url, "&timezonestring=%s", tz);
            g_free(tz);
        }
        body = http_get(url->str, err);
        g_string_free(url, TRUE);
        if (body && (t = parse_timings(body, err))) {
            g_file_set_contents(path, body, -1, NULL);
            prune_cache(dir);
        }
        g_free(body);
    }
    g_free(path);
    g_free(dir);
    return t;
}
