/*
 * test_agent_config.c — host-side smoke test for the SlumberCube Agent
 * configuration parser and URL construction.
 *
 * Mirrors the relevant logic from main/wifi_provisioning.c and
 * main/audio_player_wrapper.c. Kept byte-for-byte in sync with the
 * production functions; if you change a parser there, copy the change
 * here and re-run this test.
 *
 * Build & run on the host (macOS / Linux):
 *   make -C tests test
 *   # or directly:
 *   cc -std=c99 -Wall -Wextra -I. tests/test_agent_config.c -o tests/test_agent_config
 *   ./tests/test_agent_config
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- ESP-IDF return codes (host-side stand-ins; values match ESP-IDF) ---- */
#define ESP_OK                  0
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106

/* ---- Mirror of agent_config_t (kept in sync — see main/agent_config.h) --- */
#define AGENT_HOST_MAX 64
typedef struct {
    char host[AGENT_HOST_MAX + 1];
    int  enabled;   /* bool on device, int on host for portability */
} agent_config_t;

/* ---- Copy of parse_provisioning_post() from main/wifi_provisioning.c ----
 * Inlined here so the test exercises the EXACT production code path
 * (modulo the EXTRACT macro, which is preserved verbatim). */
static int parse_provisioning_post(const char *body, size_t body_len,
                                   char *out_ssid, size_t ssid_size,
                                   char *out_pass, size_t pass_size,
                                   char *out_host, size_t host_size,
                                   int  *out_agent)
{
    if (body == NULL || body_len == 0) return ESP_ERR_INVALID_ARG;
    if (out_agent) *out_agent = 0;
    if (out_host)  out_host[0] = '\0';

    const char *p   = body;
    const char *end = body + body_len;

    const char *ssid_key = strstr(p, "\"ssid\"");
    const char *pass_key = strstr(p, "\"pass\"");
    if (!ssid_key || !pass_key) return ESP_ERR_INVALID_ARG;

    #define EXTRACT(key_ptr, out, out_size)                                       \
        do {                                                                      \
            const char *colon = strchr(key_ptr, ':');                            \
            if (!colon) return ESP_ERR_INVALID_ARG;                              \
            colon++;                                                              \
            while (colon < end && (*colon == ' ' || *colon == '\t')) colon++;     \
            if (colon >= end || *colon != '"') return ESP_ERR_INVALID_ARG;       \
            colon++;                                                              \
            size_t oi = 0;                                                        \
            while (colon < end && *colon != '"' && oi < (out_size) - 1) {        \
                if (*colon == '\\' && colon + 1 < end) colon++;                   \
                (out)[oi++] = *colon++;                                           \
            }                                                                     \
            (out)[oi] = '\0';                                                     \
        } while (0)

    EXTRACT(ssid_key, out_ssid, ssid_size);
    EXTRACT(pass_key, out_pass, pass_size);
    #undef EXTRACT

    if (out_ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    if (out_agent) {
        const char *agent_key = strstr(p, "\"agent\"");
        if (agent_key) {
            const char *colon = strchr(agent_key, ':');
            if (colon) {
                colon++;
                while (colon < end && (*colon == ' ' || *colon == '\t')) colon++;
                if (colon < end && (*colon == 't' || *colon == '1')) {
                    *out_agent = 1;
                }
            }
        }
    }

    if (out_host) {
        const char *host_key = strstr(p, "\"host\"");
        if (host_key) {
            const char *colon = strchr(host_key, ':');
            if (!colon) return ESP_ERR_INVALID_ARG;
            colon++;
            while (colon < end && (*colon == ' ' || *colon == '\t')) colon++;
            if (colon >= end || *colon != '"') return ESP_ERR_INVALID_ARG;
            colon++;
            while (colon < end && (*colon == ' ' || *colon == '\t')) colon++;
            size_t oi = 0;
            while (colon < end && *colon != '"' && oi < host_size - 1) {
                if (*colon == '\\' && colon + 1 < end) colon++;
                out_host[oi++] = *colon++;
            }
            out_host[oi] = '\0';
            while (oi > 0 && (out_host[oi - 1] == ' ' || out_host[oi - 1] == '\t')) {
                out_host[--oi] = '\0';
            }
        }
    }

    return ESP_OK;
}

/* ---- Copy of sanitize_host() from main/wifi_provisioning.c -------------- */
static const char *AGENT_HOST_DEFAULT = "192.168.8.192";

static const char *sanitize_host(const char *host)
{
    if (host == NULL || host[0] == '\0') return AGENT_HOST_DEFAULT;
    for (const char *p = host; *p; p++) {
        if (*p == ':' || *p == '/' || *p == ' ' || *p == '\t') {
            return AGENT_HOST_DEFAULT;
        }
    }
    return host;
}

/* ---- Mirror of radio_api_url() URL construction (host + path + id + qs) -- */
static int build_radio_url(const char *host, const char *device_id,
                           const char *wake, const char *indoor,
                           char *out, size_t out_size)
{
    char qs[96] = {0};
    int  offset = 0;
    if (wake && wake[0]) {
        offset += snprintf(qs + offset, sizeof(qs) - offset, "?wake=%s", wake);
    }
    if (indoor && indoor[0]) {
        offset += snprintf(qs + offset, sizeof(qs) - offset,
                           "%c%s", (offset > 0) ? '&' : '?', indoor);
    }
    return snprintf(out, out_size, "http://%s:%d%s/%s%s",
                    host, 3000, "/api/esp", device_id, qs);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Test cases
 * ────────────────────────────────────────────────────────────────────────── */

static void test_well_formed_with_agent_enabled(void)
{
    const char *body = "{\"ssid\":\"A\",\"pass\":\"B\",\"agent\":true,\"host\":\"10.0.0.42\"}";
    char ssid[33] = {0}, pass[65] = {0}, host[AGENT_HOST_MAX + 1] = {0};
    int agent = -1;
    int rc = parse_provisioning_post(body, strlen(body),
                                     ssid, sizeof(ssid), pass, sizeof(pass),
                                     host, sizeof(host), &agent);
    assert(rc == ESP_OK);
    assert(strcmp(ssid, "A") == 0);
    assert(strcmp(pass, "B") == 0);
    assert(agent == 1);
    assert(strcmp(host, "10.0.0.42") == 0);
    printf("  ok  well-formed JSON with agent=true parses cleanly\n");
}

static void test_well_formed_with_agent_disabled(void)
{
    const char *body = "{\"ssid\":\"A\",\"pass\":\"B\",\"agent\":false,\"host\":\"10.0.0.42\"}";
    char ssid[33] = {0}, pass[65] = {0}, host[AGENT_HOST_MAX + 1] = {0};
    int agent = -1;
    int rc = parse_provisioning_post(body, strlen(body),
                                     ssid, sizeof(ssid), pass, sizeof(pass),
                                     host, sizeof(host), &agent);
    assert(rc == ESP_OK);
    assert(agent == 0);
    assert(strcmp(host, "10.0.0.42") == 0);
    printf("  ok  well-formed JSON with agent=false parses cleanly\n");
}

static void test_agent_field_absent(void)
{
    /* No "agent" key at all — parser should default to false. */
    const char *body = "{\"ssid\":\"A\",\"pass\":\"B\",\"host\":\"10.0.0.42\"}";
    char ssid[33] = {0}, pass[65] = {0}, host[AGENT_HOST_MAX + 1] = {0};
    int agent = -1;
    int rc = parse_provisioning_post(body, strlen(body),
                                     ssid, sizeof(ssid), pass, sizeof(pass),
                                     host, sizeof(host), &agent);
    assert(rc == ESP_OK);
    assert(agent == 0);
    printf("  ok  missing agent key defaults to false\n");
}

static void test_host_field_absent(void)
{
    /* No "host" key — parser should leave host[0] == '\0'. */
    const char *body = "{\"ssid\":\"A\",\"pass\":\"B\",\"agent\":true}";
    char ssid[33] = {0}, pass[65] = {0}, host[AGENT_HOST_MAX + 1] = {0};
    int agent = -1;
    int rc = parse_provisioning_post(body, strlen(body),
                                     ssid, sizeof(ssid), pass, sizeof(pass),
                                     host, sizeof(host), &agent);
    assert(rc == ESP_OK);
    assert(agent == 1);
    assert(host[0] == '\0');
    printf("  ok  missing host key leaves host empty\n");
}

static void test_host_too_long_truncated(void)
{
    /* 100-char host — parser must NUL-terminate within AGENT_HOST_MAX. */
    char long_host[101];
    memset(long_host, 'a', 100);
    long_host[100] = '\0';
    char body[256];
    snprintf(body, sizeof(body),
             "{\"ssid\":\"A\",\"pass\":\"B\",\"agent\":true,\"host\":\"%s\"}",
             long_host);
    char ssid[33] = {0}, pass[65] = {0}, host[AGENT_HOST_MAX + 1] = {0};
    int agent = -1;
    int rc = parse_provisioning_post(body, strlen(body),
                                     ssid, sizeof(ssid), pass, sizeof(pass),
                                     host, sizeof(host), &agent);
    assert(rc == ESP_OK);
    assert(strlen(host) == AGENT_HOST_MAX);   /* truncated to 64 */
    assert(host[AGENT_HOST_MAX] == '\0');
    printf("  ok  overlong host truncated to AGENT_HOST_MAX\n");
}

static void test_host_with_whitespace_stripped(void)
{
    const char *body = "{\"ssid\":\"A\",\"pass\":\"B\",\"agent\":true,\"host\":\"  10.0.0.42  \"}";
    char ssid[33] = {0}, pass[65] = {0}, host[AGENT_HOST_MAX + 1] = {0};
    int agent = -1;
    int rc = parse_provisioning_post(body, strlen(body),
                                     ssid, sizeof(ssid), pass, sizeof(pass),
                                     host, sizeof(host), &agent);
    assert(rc == ESP_OK);
    assert(strcmp(host, "10.0.0.42") == 0);
    printf("  ok  host whitespace is stripped\n");
}

static void test_host_garbage_rejected(void)
{
    /* Hosts containing ':' or '/' or ' ' must be coerced to the default by
     * sanitize_host. The parser itself extracts verbatim; sanitisation is
     * a separate policy step. */
    assert(strcmp(sanitize_host("10.0.0.42"), "10.0.0.42") == 0);
    assert(strcmp(sanitize_host(""),          AGENT_HOST_DEFAULT) == 0);
    assert(strcmp(sanitize_host(NULL),        AGENT_HOST_DEFAULT) == 0);
    assert(strcmp(sanitize_host("http://x"),  AGENT_HOST_DEFAULT) == 0);
    assert(strcmp(sanitize_host("10.0.0.42:3000"), AGENT_HOST_DEFAULT) == 0);
    assert(strcmp(sanitize_host("foo/bar"),   AGENT_HOST_DEFAULT) == 0);
    assert(strcmp(sanitize_host("foo bar"),   AGENT_HOST_DEFAULT) == 0);
    printf("  ok  garbage host substituted with default by sanitize_host\n");
}

static void test_agent_value_is_int(void)
{
    /* Accept the form-encoded / loose interpretation: "agent":1 → true. */
    const char *body = "{\"ssid\":\"A\",\"pass\":\"B\",\"agent\":1,\"host\":\"10.0.0.42\"}";
    char ssid[33] = {0}, pass[65] = {0}, host[AGENT_HOST_MAX + 1] = {0};
    int agent = -1;
    int rc = parse_provisioning_post(body, strlen(body),
                                     ssid, sizeof(ssid), pass, sizeof(pass),
                                     host, sizeof(host), &agent);
    assert(rc == ESP_OK);
    assert(agent == 1);
    printf("  ok  \"agent\":1 accepted as true\n");
}

static void test_agent_value_is_string_on(void)
{
    /* "agent":"on" — HTML default form-encoded value. The parser does NOT
     * match this (the JSON-literal rule matches only 't' or '1' as the
     * first non-whitespace char after ':'), so this should default to 0.
     * The captive portal's JS always serializes the boolean as true/false,
     * so this case is documented but not actively supported. */
    const char *body = "{\"ssid\":\"A\",\"pass\":\"B\",\"agent\":\"on\",\"host\":\"x\"}";
    char ssid[33] = {0}, pass[65] = {0}, host[AGENT_HOST_MAX + 1] = {0};
    int agent = -1;
    int rc = parse_provisioning_post(body, strlen(body),
                                     ssid, sizeof(ssid), pass, sizeof(pass),
                                     host, sizeof(host), &agent);
    assert(rc == ESP_OK);
    assert(agent == 0);
    printf("  ok  \"agent\":\"on\" defaults to false (JS always sends bool)\n");
}

static void test_struct_invariants(void)
{
    /* agent_config_t field sizes are part of the NVS contract. */
    assert(sizeof(((agent_config_t *)0)->host) == AGENT_HOST_MAX + 1);
    printf("  ok  agent_config_t host field is %d bytes\n", AGENT_HOST_MAX + 1);
}

static void test_url_construction_with_default_host(void)
{
    char url[384];
    int n = build_radio_url("192.168.8.192", "DEADBEEF1234",
                            NULL, NULL, url, sizeof(url));
    assert(n > 0 && (size_t)n < sizeof(url));
    assert(strcmp(url, "http://192.168.8.192:3000/api/esp/DEADBEEF1234") == 0);
    printf("  ok  URL with default host built correctly\n");
}

static void test_url_construction_with_custom_host(void)
{
    char url[384];
    int n = build_radio_url("10.0.0.42", "DEADBEEF1234",
                            "rtc", "t=24.3&h=58", url, sizeof(url));
    assert(n > 0 && (size_t)n < sizeof(url));
    assert(strcmp(url,
        "http://10.0.0.42:3000/api/esp/DEADBEEF1234?wake=rtc&t=24.3&h=58") == 0);
    printf("  ok  URL with custom host + query string built correctly\n");
}

static void test_url_fits_in_384(void)
{
    /* Worst case: 63-char host + 12-char device id + full query. */
    char host[64];
    memset(host, 'a', 63);
    host[63] = '\0';
    char url[384];
    int n = build_radio_url(host, "DEADBEEF1234",
                            "rtc", "t=99.9&h=99", url, sizeof(url));
    assert(n > 0 && (size_t)n < sizeof(url));
    assert(strlen(url) < sizeof(url));
    printf("  ok  worst-case URL fits in 384-byte buffer (%d chars)\n", n);
}

int main(void)
{
    printf("test_agent_config:\n");
    test_well_formed_with_agent_enabled();
    test_well_formed_with_agent_disabled();
    test_agent_field_absent();
    test_host_field_absent();
    test_host_too_long_truncated();
    test_host_with_whitespace_stripped();
    test_host_garbage_rejected();
    test_agent_value_is_int();
    test_agent_value_is_string_on();
    test_struct_invariants();
    test_url_construction_with_default_host();
    test_url_construction_with_custom_host();
    test_url_fits_in_384();
    printf("all tests passed\n");
    return 0;
}
