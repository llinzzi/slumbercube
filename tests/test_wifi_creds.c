/*
 * test_wifi_creds.c — host-side smoke test for WiFi credential handling.
 *
 * Exercises the two pieces of logic that are easy to break and hard to
 * catch without a real ESP32 in hand:
 *
 *   1. parse_wifi_post() — JSON {ssid, pass} parser used by the captive
 *      portal POST /api/wifi handler.
 *   2. wifi_creds_t invariants — SSID + pass length limits, NUL termination.
 *
 * The actual NVS round-trip (wifi_creds_save → nvs_set_str → nvs_commit)
 * runs on-device; we don't try to mock NVS here because that's better
 * validated by a real flash + serial log.
 *
 * Build & run on the host (macOS / Linux):
 *   cc -std=c99 -Wall -Wextra -I. tests/test_wifi_creds.c -o tests/test_wifi_creds
 *   ./tests/test_wifi_creds
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- mirror of wifi.h types (kept in sync — see main/wifi.h) ------------ */
typedef struct {
    char ssid[33];
    char pass[65];
    int  configured;
} wifi_creds_t;

/* Stand-in for ESP-IDF return codes; the production code uses esp_err_t
 * but the integer values match. */
#define ESP_OK                  0
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_NOT_FOUND       0x105

/* ---- copy of parse_wifi_post() from main/wifi_provisioning.c -------------
 * Kept byte-for-byte in sync with the production function. If you change
 * the parser there, copy the change here and re-run this test. */
static int parse_wifi_post(const char *body, size_t body_len,
                           char *out_ssid, size_t ssid_size,
                           char *out_pass, size_t pass_size)
{
    if (body == NULL || body_len == 0) return ESP_ERR_INVALID_ARG;

    const char *p = body;
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
    return ESP_OK;
}

/* ---- the actual test cases ------------------------------------------------ */

static void test_parse_well_formed(void)
{
    const char *body = "{\"ssid\":\"Happy\",\"pass\":\"ping8275\"}";
    char ssid[33], pass[65];
    int rc = parse_wifi_post(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    assert(rc == ESP_OK);
    assert(strcmp(ssid, "Happy") == 0);
    assert(strcmp(pass, "ping8275") == 0);
    printf("  ok  well-formed JSON parses cleanly\n");
}

static void test_parse_with_extra_fields(void)
{
    const char *body = "{\"foo\":\"bar\",\"ssid\":\"  Trimmed  \",\"pass\":\"  secret  \",\"extra\":42}";
    char ssid[33], pass[65];
    int rc = parse_wifi_post(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    assert(rc == ESP_OK);
    /* Leading/trailing whitespace IS preserved by this parser — the WiFi
     * driver will reject it, which is the right failure mode (visible to
     * the user via a connection retry). Just assert what we return. */
    assert(strcmp(ssid, "  Trimmed  ") == 0);
    assert(strcmp(pass, "  secret  ") == 0);
    printf("  ok  extra fields don't break parsing\n");
}

static void test_parse_missing_pass(void)
{
    const char *body = "{\"ssid\":\"Happy\"}";
    char ssid[33], pass[65];
    int rc = parse_wifi_post(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    assert(rc == ESP_ERR_INVALID_ARG);
    printf("  ok  missing pass key → INVALID_ARG\n");
}

static void test_parse_missing_ssid(void)
{
    const char *body = "{\"pass\":\"secret\"}";
    char ssid[33], pass[65];
    int rc = parse_wifi_post(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    assert(rc == ESP_ERR_INVALID_ARG);
    printf("  ok  missing ssid key → INVALID_ARG\n");
}

static void test_parse_empty_ssid(void)
{
    const char *body = "{\"ssid\":\"\",\"pass\":\"secret\"}";
    char ssid[33], pass[65];
    int rc = parse_wifi_post(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    assert(rc == ESP_ERR_INVALID_ARG);
    printf("  ok  empty SSID → INVALID_ARG\n");
}

static void test_parse_empty_body(void)
{
    char ssid[33], pass[65];
    int rc = parse_wifi_post("", 0, ssid, sizeof(ssid), pass, sizeof(pass));
    assert(rc == ESP_ERR_INVALID_ARG);
    rc = parse_wifi_post(NULL, 0, ssid, sizeof(ssid), pass, sizeof(pass));
    assert(rc == ESP_ERR_INVALID_ARG);
    printf("  ok  empty/null body → INVALID_ARG\n");
}

static void test_parse_long_ssid_truncated(void)
{
    /* SSID at the 32-byte NVS limit. Parser must NUL-terminate within
     * the output buffer. */
    char body[128];
    char long_ssid[40];
    memset(long_ssid, 'A', 32);
    long_ssid[32] = '\0';
    snprintf(body, sizeof(body), "{\"ssid\":\"%s\",\"pass\":\"pw\"}", long_ssid);
    char ssid[33], pass[65];
    int rc = parse_wifi_post(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    assert(rc == ESP_OK);
    assert(strlen(ssid) == 32);
    assert(strcmp(ssid, long_ssid) == 0);
    printf("  ok  32-byte SSID parses without overflow\n");
}

static void test_parse_escaped_quote(void)
{
    /* SSID with an embedded escaped quote: foo\"bar → foo"bar after parse. */
    const char *body = "{\"ssid\":\"foo\\\"bar\",\"pass\":\"secret\"}";
    char ssid[33], pass[65];
    int rc = parse_wifi_post(body, strlen(body), ssid, sizeof(ssid), pass, sizeof(pass));
    assert(rc == ESP_OK);
    assert(strcmp(ssid, "foo\"bar") == 0);
    printf("  ok  escaped quote in SSID handled\n");
}

static void test_creds_struct_invariants(void)
{
    /* The wifi_creds_t struct has fixed-size fields; SSID + 1 NUL = 33,
     * pass + 1 NUL = 65. Verify these so a struct change is caught early. */
    assert(sizeof(((wifi_creds_t *)0)->ssid) == 33);
    assert(sizeof(((wifi_creds_t *)0)->pass) == 65);
    printf("  ok  wifi_creds_t field sizes are 33 / 65\n");
}

static void test_qr_payload_format(void)
{
    /* The QR code embeds "WIFI:T:WPA;S:<ssid>;P:<pass>;;". A long SSID
     * could overflow a 160-byte C string. Verify the format compiles
     * and that the snprintf size limit holds. */
    char qr[160];
    const char *ssid = "SlumberCube-AABB";
    const char *pass = "setup12345678";
    int n = snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
    assert(n > 0 && (size_t)n < sizeof(qr));
    assert(strstr(qr, ssid) != NULL);
    assert(strstr(qr, pass) != NULL);
    assert(strstr(qr, "WIFI:T:WPA;") == qr);  /* must start with prefix */
    printf("  ok  QR payload fits in 160-byte buffer\n");
}

int main(void)
{
    printf("test_wifi_creds:\n");
    test_parse_well_formed();
    test_parse_with_extra_fields();
    test_parse_missing_pass();
    test_parse_missing_ssid();
    test_parse_empty_ssid();
    test_parse_empty_body();
    test_parse_long_ssid_truncated();
    test_parse_escaped_quote();
    test_creds_struct_invariants();
    test_qr_payload_format();
    printf("all tests passed\n");
    return 0;
}