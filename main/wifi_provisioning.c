#include "wifi_provisioning.h"
#include "wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_random.h"

/* lwIP BSD sockets — for the DNS redirect task on UDP/53. */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

/* Captive-portal HTTP port — 80 is what phone browsers probe first. */
#define WIFI_PROV_HTTP_PORT    80

/* Bits for the provisioning state event group. */
#define WIFI_PROV_BIT_DONE     BIT0   /* user submitted credentials */
#define WIFI_PROV_BIT_ABORT    BIT1   /* external caller requested abort */

static const char *TAG = "WIFI_PROV";

/* Shared state for handlers and the runner task. */
static httpd_handle_t  s_httpd      = NULL;
static esp_netif_t    *s_ap_netif   = NULL;
static TaskHandle_t    s_dns_task   = NULL;
static EventGroupHandle_t s_events  = NULL;
static volatile bool   s_aborted    = false;

/* Build the AP SSID as "SlumberCube-XXXX" where XXXX is the last 2 bytes of
 * the STA MAC (uppercase hex). Stable per device. */
static void build_ap_ssid(char *out, size_t out_len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_len, "SlumberCube-%02X%02X", mac[4], mac[5]);
}

/* ---------- DNS captive-portal redirect ------------------------------------
 * Listens on UDP/53 and answers every A query with 192.168.4.1, so phones
 * that probe well-known captive-portal hostnames (captive.apple.com,
 * connectivitycheck.gstatic.net, etc.) end up hitting our HTTP server,
 * which redirects them to /. */
static void dns_redirect_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket() failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind() failed: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* 200 ms recv timeout so we can observe s_aborted without stalling. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[256];
    while (!s_aborted) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&from, &from_len);
        if (len < 12) continue;  /* too short to be a real DNS query */

        /* Parse just enough to find the question count.
         * Bytes 0-1: ID; 2-3: flags; 4-5: QDCOUNT; 6-7: ANCOUNT;
         * 8-9: NSCOUNT; 10-11: ARCOUNT. */
        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if (qdcount == 0) continue;

        /* Build a minimal response: copy query, set QR=1, AA=1, ANSWER=1.
         * Answer is a single A record pointing at 192.168.4.1. We re-use
         * the query bytes and append the answer after them. */
        uint8_t resp[sizeof(buf) + 32];
        if ((size_t)len + 32 > sizeof(resp)) continue;
        memcpy(resp, buf, len);

        /* Header: copy ID, set flags to standard response with 1 answer. */
        resp[2] = 0x81;  /* QR=1, opcode=0, AA=1, TC=0, RD=1 */
        resp[3] = 0x80;  /* RA=1, Z=0, RCODE=0 */
        resp[6] = 0x00;  /* ANCOUNT = 1 */
        resp[7] = 0x01;
        /* NSCOUNT / ARCOUNT = 0 */

        /* Skip past the question section. The query format is:
         *  [QNAME: len-prefixed labels], QTYPE(2), QCLASS(2).
         * QNAME ends at the first 0x00 byte. */
        size_t off = 12;
        while (off < (size_t)len && resp[off] != 0) {
            off += resp[off] + 1;
        }
        off += 1 + 4;  /* skip trailing 0 + QTYPE + QCLASS */
        if (off + 16 > sizeof(resp)) continue;

        /* Answer RR: pointer to name, type A, class IN, TTL 60, RDLENGTH 4, RD 192.168.4.1 */
        resp[off++] = 0xC0; resp[off++] = 0x0C;          /* name pointer to offset 12 */
        resp[off++] = 0x00; resp[off++] = 0x01;          /* TYPE = A */
        resp[off++] = 0x00; resp[off++] = 0x01;          /* CLASS = IN */
        resp[off++] = 0x00; resp[off++] = 0x00;          /* TTL = 0 (upper) */
        resp[off++] = 0x00; resp[off++] = 0x3C;          /* TTL = 60 (lower) */
        resp[off++] = 0x00; resp[off++] = 0x04;          /* RDLENGTH = 4 */
        resp[off++] = 192; resp[off++] = 168;
        resp[off++] =   4; resp[off++] =   1;

        sendto(sock, resp, off, 0,
               (struct sockaddr *)&from, sizeof(from));
    }

    close(sock);
    ESP_LOGI(TAG, "DNS redirect task exiting");
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void start_dns_redirect(void)
{
    s_aborted = false;
    xTaskCreate(dns_redirect_task, "prov_dns", 3072, NULL, 5, &s_dns_task);
}

static void stop_dns_redirect(void)
{
    s_aborted = true;
    /* The task checks s_aborted every 200 ms and exits. Give it up to 1 s. */
    for (int i = 0; i < 5 && s_dns_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ---------- HTTP server handlers ------------------------------------------- */

/* Compact HTML form: a single page that lists nearby networks and lets the
 * user pick one + enter a password. Posted to /api/wifi as JSON. */
static const char INDEX_HTML[] =
"<!doctype html><html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>SlumberCube WiFi</title>"
"<style>"
"body{font:16px/1.4 -apple-system,system-ui,sans-serif;margin:24px;color:#222}"
"h1{font-size:18px;margin:0 0 16px}"
"label{display:block;margin:12px 0 4px;font-weight:600}"
"select,input{width:100%;padding:10px;border:1px solid #ccc;border-radius:6px;box-sizing:border-box;font-size:16px}"
"button{margin-top:16px;width:100%;padding:12px;background:#0066cc;color:#fff;border:0;border-radius:6px;font-size:16px;font-weight:600}"
".msg{margin-top:12px;padding:10px;border-radius:6px;display:none}"
".ok{background:#dfd;color:#060}.err{background:#fdd;color:#600}"
"</style></head><body>"
"<h1>SlumberCube WiFi setup</h1>"
"<form id=f>"
"<label for=ssid>Network name</label>"
"<select id=ssid name=ssid></select>"
"<input id=ssid_custom name=ssid_custom placeholder='or type hidden SSID' style='margin-top:8px'>"
"<label for=pass>Password</label>"
"<input id=pass name=pass type=password>"
"<button type=submit>Connect</button>"
"</form>"
"<div id=msg class=msg></div>"
"<script>"
"async function load(){const r=await fetch('/scan');const a=await r.json();"
"const s=document.getElementById('ssid');s.innerHTML=a.map(n=>`<option value=\"${n.ssid.replace(/\"/g,'&quot;')}\">${n.ssid} (${n.rssi}dBm)</option>`).join('')+'<option value=\"\">(hidden)</option>';}"
"load();"
"document.getElementById('f').onsubmit=async e=>{e.preventDefault();"
"const ssid=document.getElementById('ssid').value||document.getElementById('ssid_custom').value;"
"const pass=document.getElementById('pass').value;"
"const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})});"
"const j=await r.json();const m=document.getElementById('msg');"
"m.className='msg '+(j.ok?'ok':'err');m.textContent=j.ok?'Saved. Device will reboot in 3s.':j.error;m.style.display='block';};"
"</script></body></html>";

/* Generic captive-portal probe redirect. Phones GET one of these well-known
 * URLs to detect a captive portal; we redirect them to / (the form). */
static const char *CAPTIVE_PORTAL_PATHS[] = {
    "/generate_204",
    "/gen_204",
    "/hotspot-detect.html",
    "/connecttest.txt",
    "/redirect",
    "/success.txt",
    "/ncsi.txt",
    NULL,
};

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    /* 302 redirect works for most captive-portal detection (some phones need
     * a 200 with a body, but 302 is the lowest common denominator). */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* GET /scan → JSON array of {ssid, rssi, auth} for nearby APs.
 * Runs a fresh scan each call (small STA scan). Blocks the handler briefly. */
static esp_err_t scan_handler(httpd_req_t *req)
{
    /* Default scan config — active scan, ~1 s. */
    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = { .active = { .min = 100, .max = 300 } },
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"error\":\"scan failed\"}", HTTPD_RESP_USE_STRLEN);
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }
    /* Cap at 16 to keep the JSON response small on this little heap. */
    if (ap_count > 16) ap_count = 16;

    wifi_ap_record_t *records = calloc(ap_count, sizeof(*records));
    if (records == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
    }
    esp_wifi_scan_get_ap_records(&ap_count, records);

    /* Build JSON. Each entry: {"ssid":"...","rssi":-65,"auth":2}.
     * Escape backslash and double-quote in SSID. */
    char *buf = malloc(2048);
    if (buf == NULL) {
        free(records);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
    }
    size_t off = 0;
    off += snprintf(buf + off, 2048 - off, "[");
    for (uint16_t i = 0; i < ap_count && off < 2000; i++) {
        /* Skip hidden / empty SSIDs. */
        if (records[i].ssid[0] == 0) continue;

        /* JSON-escape the SSID. */
        char esc[80];
        size_t eo = 0;
        for (int j = 0; records[i].ssid[j] && eo < sizeof(esc) - 2; j++) {
            char ch = records[i].ssid[j];
            if (ch == '"' || ch == '\\') {
                esc[eo++] = '\\';
            }
            esc[eo++] = ch;
        }
        esc[eo] = 0;

        off += snprintf(buf + off, 2048 - off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                        (i == 0 ? "" : ","), esc,
                        records[i].rssi, records[i].authmode);
    }
    off += snprintf(buf + off, 2048 - off, "]");

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_ret = httpd_resp_send(req, buf, off);
    free(records);
    free(buf);
    return send_ret;
}

/* Minimal JSON parser for {"ssid":"X","pass":"Y"}. Avoids pulling in cJSON
 * to keep heap pressure low during provisioning. Returns ESP_OK on success;
 * outputs are NUL-terminated and bounded by the supplied sizes. */
static esp_err_t parse_wifi_post(const char *body, size_t body_len,
                                 char *out_ssid, size_t ssid_size,
                                 char *out_pass, size_t pass_size)
{
    if (body == NULL || body_len == 0) return ESP_ERR_INVALID_ARG;

    const char *p = body;
    const char *end = body + body_len;

    /* Find "ssid":"..." and "pass":"..." keys. */
    const char *ssid_key = strstr(p, "\"ssid\"");
    const char *pass_key = strstr(p, "\"pass\"");
    if (!ssid_key || !pass_key) return ESP_ERR_INVALID_ARG;

    /* Helper to extract the value following a key. */
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

static esp_err_t api_wifi_handler(httpd_req_t *req)
{
    /* Read body up to 256 bytes (SSID max 32, pass max 64, plus JSON overhead). */
    char body[256];
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no body\"}", HTTPD_RESP_USE_STRLEN);
    }
    body[recv_len] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err = parse_wifi_post(body, recv_len, ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad json\"}", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "POST /api/wifi ssid='%s' pass_len=%u", ssid, (unsigned)strlen(pass));

    wifi_creds_t creds = { .configured = true };
    strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
    strncpy(creds.pass, pass, sizeof(creds.pass) - 1);

    err = wifi_creds_save(&creds);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"save failed\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* Signal the runner task. */
    if (s_events) xEventGroupSetBits(s_events, WIFI_PROV_BIT_DONE);
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* Captive-portal probe handlers — these are the URLs phones fetch first to
 * detect that the network has a portal. They all redirect to /. */
static esp_err_t captive_probe_handler(httpd_req_t *req)
{
    return captive_redirect_handler(req);
}

static httpd_handle_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = WIFI_PROV_HTTP_PORT;
    cfg.ctrl_port      = WIFI_PROV_HTTP_PORT + 1;
    cfg.max_uri_handlers = 12;
    /* Smaller stack to keep provisioning lean — handlers are short. */
    cfg.stack_size     = 4096;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return NULL;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",         .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/scan",     .method = HTTP_GET,  .handler = scan_handler },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = api_wifi_handler },
    };
    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    /* Register all well-known captive-portal probe paths → redirect. */
    for (int i = 0; CAPTIVE_PORTAL_PATHS[i] != NULL; i++) {
        httpd_uri_t u = {
            .uri     = CAPTIVE_PORTAL_PATHS[i],
            .method  = HTTP_GET,
            .handler = captive_probe_handler,
        };
        httpd_register_uri_handler(server, &u);
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", WIFI_PROV_HTTP_PORT);
    return server;
}

static void stop_http_server(httpd_handle_t server)
{
    if (server) httpd_stop(server);
}

/* ---------- SoftAP setup --------------------------------------------------- */

static esp_err_t start_softap(const char *ssid, const char *pass)
{
    /* APSTA so we can still keep the STA interface initialised for the scan. */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG, "esp_wifi_set_mode APSTA failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "create_default_wifi_ap failed");
        return ESP_FAIL;
    }
    /* Stop DHCP server momentarily so we can apply a stable IP. */
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_ip_info_t ip = {
        .ip      = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) },
        .gw      = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
    };
    esp_netif_set_ip_info(s_ap_netif, &ip);
    esp_netif_dhcps_start(s_ap_netif);

    wifi_config_t ap_cfg = {
        .ap = {
            .channel       = 1,
            .max_connection = 4,
            .authmode      = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid,     ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);
    strncpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password) - 1);
    if (strlen(pass) < 8) ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SoftAP started: SSID='%s' (channel %d)", ssid, ap_cfg.ap.channel);
    return ESP_OK;
}

static void stop_softap(void)
{
    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
}

/* ---------- Public API ----------------------------------------------------- */

void wifi_provisioning_abort(void)
{
    if (s_events) {
        xEventGroupSetBits(s_events, WIFI_PROV_BIT_ABORT);
    } else {
        s_aborted = true;
    }
}

wifi_prov_result_t wifi_provisioning_run(void)
{
    ESP_LOGI(TAG, "Starting provisioning AP");

    /* Block the STA event handler from auto-connecting. Without this, the
     * moment we bring up APSTA mode the handler fires esp_wifi_connect()
     * from the WIFI_EVENT_STA_START callback, putting the radio in a
     * permanent "connecting" state that we can't escape to set config. */
    wifi_suppress_auto_connect(true);

    /* Ensure WiFi driver + STA netif are up before we try to add the AP netif
     * or call esp_wifi_set_mode(APSTA). On first boot (NVS empty), main.c
     * hasn't called wifi_init_sta() yet, so the driver is uninitialised —
     * which is what blew up on real hardware. */
    extern esp_err_t wifi_init_sta(void);   /* forward decl — see main/wifi.c */
    /* Best-effort: if NVS has creds, this connects; if not, it fails after 30 s
     * with menuconfig fallback. Either way, the driver is up afterward. */
    (void)wifi_init_sta();

    /* Build AP identity from device MAC. */
    char ap_ssid[32];
    build_ap_ssid(ap_ssid, sizeof(ap_ssid));
    const char *ap_pass = CONFIG_WIFI_PROV_AP_PASSWORD;

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        return WIFI_PROV_ERROR;
    }
    s_aborted = false;

    if (start_softap(ap_ssid, ap_pass) != ESP_OK) {
        vEventGroupDelete(s_events);
        s_events = NULL;
        return WIFI_PROV_ERROR;
    }
    start_dns_redirect();
    s_httpd = start_http_server();
    if (s_httpd == NULL) {
        stop_dns_redirect();
        stop_softap();
        vEventGroupDelete(s_events);
        s_events = NULL;
        return WIFI_PROV_ERROR;
    }

    /* Notify the UI layer to draw the QR + SSID on the OLED. The OLED update
     * is fire-and-forget — config_screen_show() runs in the caller's task
     * context, so the screen swap blocks us briefly but stays under 100 ms. */
    extern void config_screen_show(const char *ap_ssid, const char *ap_pass);
    config_screen_show(ap_ssid, ap_pass);

    ESP_LOGI(TAG, "Waiting for credentials (timeout %ds)", CONFIG_WIFI_PROV_TIMEOUT_SECS);

    TickType_t ticks = pdMS_TO_TICKS(CONFIG_WIFI_PROV_TIMEOUT_SECS * 1000);
    EventBits_t bits = xEventGroupWaitBits(s_events,
                                            WIFI_PROV_BIT_DONE | WIFI_PROV_BIT_ABORT,
                                            pdFALSE, pdFALSE, ticks);

    /* Tear everything down before returning, regardless of outcome. */
    stop_http_server(s_httpd);  s_httpd = NULL;
    stop_dns_redirect();
    stop_softap();

    /* Hand the screen back to the clock widget. */
    extern void config_screen_hide(void);
    config_screen_hide();

    /* Switch WiFi mode back to STA-only so the next wifi_init_sta() call
     * finds a clean state. esp_wifi_set_mode is safe to call repeatedly. */
    esp_wifi_set_mode(WIFI_MODE_STA);

    /* Re-enable auto-connect for the post-provisioning STA session. */
    wifi_suppress_auto_connect(false);

    wifi_prov_result_t result;
    if (bits & WIFI_PROV_BIT_DONE) {
        ESP_LOGI(TAG, "Credentials submitted, exiting");
        result = WIFI_PROV_OK;
    } else if (bits & WIFI_PROV_BIT_ABORT) {
        ESP_LOGW(TAG, "Provisioning aborted");
        result = WIFI_PROV_TIMEOUT;
    } else {
        ESP_LOGW(TAG, "Provisioning timeout, no submission");
        result = WIFI_PROV_TIMEOUT;
    }

    vEventGroupDelete(s_events);
    s_events = NULL;
    return result;
}