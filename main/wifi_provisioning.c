#include "wifi_provisioning.h"
#include "wifi.h"
#include "agent_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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

/* lwIP BSD sockets — for the DNS redirect task on UDP/53. */
#include "lwip/sockets.h"
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

        /* Yield between queries so we don't busy-loop on the phone's
         * aggressive captive-portal probe burst (10s of DNS queries/sec).
         * Without this, at priority 5 we'd preempt the lvgl_task (prio 1)
         * and IDLE long enough to trip the WDT. */
        taskYIELD();

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
 * user pick one + enter a password, plus configure the SlumberCube Agent.
 * Posted to /api/wifi as JSON. CSS is inlined and minified to keep the
 * flash footprint small (≈1.9KB). */
static const char INDEX_HTML[] =
"<!doctype html><html lang=zh-CN><head>"
"<meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>安睡小方 · 配网</title>"
"<style>"
":root{--bg:#f5f6fa;--card:#fff;--ink:#1a1d29;--ink2:#6b7283;--line:#e7e9f0;"
"--b:#5b6cff;--b2:#8c5bff;--soft:#eef0ff;--ok:#128a4a;--err:#d3302f}"
"*{box-sizing:border-box}html,body{margin:0;padding:0}"
"body{background:var(--bg);color:var(--ink);"
"font:15px/1.5 -apple-system,BlinkMacSystemFont,'PingFang SC','Hiragino Sans GB',"
"'Microsoft YaHei',sans-serif;-webkit-font-smoothing:antialiased;"
"min-height:100vh;padding:28px 16px 48px}"
".w{max-width:420px;margin:0 auto}"
".hd{display:flex;align-items:center;gap:12px;margin:4px 0 22px}"
".mk{width:42px;height:42px;border-radius:11px;color:#fff;font-weight:600;font-size:18px;"
"display:grid;place-items:center;letter-spacing:1px;"
"background:linear-gradient(135deg,var(--b),var(--b2));"
"box-shadow:0 6px 16px rgba(91,108,255,.35)}"
".hd h1{font-size:19px;font-weight:600;margin:0;letter-spacing:.5px}"
".hd .sub{color:var(--ink2);font-size:12px;margin-top:2px}"
".card{background:var(--card);border:1px solid var(--line);border-radius:14px;"
"box-shadow:0 1px 2px rgba(20,28,60,.04),0 8px 24px rgba(20,28,60,.05);"
"padding:18px 18px 6px;margin-bottom:14px}"
".card h2{font-size:11px;font-weight:600;letter-spacing:1.2px;"
"color:var(--ink2);text-transform:uppercase;margin:0 0 4px}"
"label{display:block;font-size:13px;color:var(--ink2);margin:14px 0 6px}"
"label:first-of-type{margin-top:6px}"
"select,input[type=text],input[type=password]{width:100%;padding:11px 12px;"
"border:1px solid var(--line);border-radius:9px;background:#fff;"
"font:inherit;color:var(--ink);-webkit-appearance:none;appearance:none;"
"transition:border-color .15s,box-shadow .15s}"
"select{padding-right:32px;"
"background-image:url(\"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 12 8'><path d='M1 1l5 5 5-5' fill='none' stroke='%236b7283' stroke-width='1.6' stroke-linecap='round'/></svg>\");"
"background-repeat:no-repeat;background-position:right 12px center;background-size:10px}"
"input:focus,select:focus{outline:0;border-color:var(--b);"
"box-shadow:0 0 0 3px rgba(91,108,255,.15)}"
"input:disabled{background:#f4f5f8;color:#aab0bd}"
".tg{display:flex;align-items:center;gap:10px;padding:12px 14px;border-radius:10px;"
"background:var(--soft);cursor:pointer;margin-top:6px;transition:background .15s}"
".tg.off{background:#f4f5f8}"
".tg input{width:18px;height:18px;margin:0;accent-color:var(--b)}"
".tg .lbl{display:flex;flex-direction:column;line-height:1.3}"
".tg .t1{font-weight:600;font-size:14px;color:var(--ink)}"
".tg .t2{font-size:12px;color:var(--ink2);margin-top:2px}"
".tg.off .t1,.tg.off .t2{color:var(--ink2)}"
"button{width:100%;padding:13px;color:#fff;border:0;border-radius:11px;"
"font:inherit;font-weight:600;font-size:15px;cursor:pointer;margin-top:6px;"
"background:linear-gradient(135deg,var(--b),var(--b2));"
"box-shadow:0 4px 14px rgba(91,108,255,.35);transition:transform .1s}"
"button:active{transform:scale(.985)}"
"button:disabled{opacity:.6;cursor:wait;box-shadow:none}"
".msg{margin-top:14px;padding:11px 14px;border-radius:10px;font-size:14px;display:none}"
".msg.ok{display:block;background:#e8f6ee;color:var(--ok)}"
".msg.err{display:block;background:#fde8e7;color:var(--err)}"
"</style></head><body><div class=w>"
"<div class=hd><div class=mk>安</div><div><h1>安睡小方</h1>"
"<div class=sub>WiFi 配网 · 首次使用设置</div></div></div>"
"<form id=f>"
"<div class=card><h2>① WiFi 网络</h2>"
"<label for=ssid>选择网络</label>"
"<select id=ssid name=ssid></select>"
"<label for=ssid_custom>或手动输入（隐藏网络）</label>"
"<input id=ssid_custom name=ssid_custom placeholder=SSID>"
"<label for=pass>密码</label>"
"<input id=pass name=pass type=password placeholder='••••••••'>"
"</div>"
"<div class=card><h2>② 安睡小方 Agent</h2>"
"<label class='tg' id=tg><input id=agent name=agent type=checkbox>"
"<div class=lbl><div class=t1>配置安睡小方Agent</div>"
"<div class=t2>关闭后只显示时间与室内温湿度</div></div></label>"
"<label for=host>服务器地址</label>"
"<input id=host name=host value='192.168.8.192' placeholder=192.168.8.192>"
"</div>"
"<button id=btn type=submit>保存并连接</button>"
"<div id=msg class=msg></div>"
"</form></div>"
"<script>"
"async function load(){const r=await fetch('/scan');let a=[];try{a=await r.json();}catch(e){console.warn('/scan parse failed',e);}"
"if(!Array.isArray(a))a=[];"
"const s=document.getElementById('ssid');"
"const opts=a.map(n=>`<option value=\"${n.ssid.replace(/\"/g,'&quot;')}\">${n.ssid} (${n.rssi}dBm)</option>`).join('');"
"s.innerHTML=opts+(opts?'<option value=\"\">(隐藏网络)</option>':'<option value=\"\">(未扫到网络 — 手动输入)</option>');}"
"load();"
"const cb=document.getElementById('agent'),hi=document.getElementById('host'),tg=document.getElementById('tg');"
"function sync(){hi.disabled=!cb.checked;tg.classList.toggle('off',!cb.checked);}"
"sync();cb.onchange=sync;"
/* If the user types anything into the host field, treat that as intent to
 * use the agent. The form's two-state UX (checkbox + host input) is easy
 * to misread: someone may edit the host IP, see the field become active,
 * and submit without explicitly ticking the checkbox. Auto-tick on host
 * input so the submitted JSON matches the user's actual intent. */
"hi.oninput=()=>{if(hi.value.trim()){cb.checked=true;sync();}};"
"document.getElementById('f').onsubmit=async e=>{e.preventDefault();"
"const btn=document.getElementById('btn');btn.disabled=true;btn.textContent='保存中…';"
"const ssid=document.getElementById('ssid').value||document.getElementById('ssid_custom').value;"
"const pass=document.getElementById('pass').value;"
"const agent=cb.checked;const host=hi.value;"
"let j;try{const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass,agent,host})});j=await r.json();}catch(err){j={ok:false,error:'网络错误'};}"
"btn.disabled=false;btn.textContent='保存并连接';"
"const m=document.getElementById('msg');"
"m.className='msg '+(j.ok?'ok':'err');m.textContent=j.ok?'已保存，设备将在 3 秒后重启':(j.error||'保存失败');m.style.display='block';};"
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
    /* Default scan config — active scan, 100/300 ms dwell. This was the
     * value that reliably found "Happy" before; the longer 200/500 ms
     * dwell occasionally caused the scan to return 0 in APSTA mode (the
     * AP's own beacon floods the radio during the longer listen window).
     * Keep what worked. */
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
        /* Always return a JSON array so the JS .map() doesn't throw on a
         * non-array body — empty array = "no networks", the form still
         * offers the manual SSID input below. */
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "scan found %u APs", ap_count);
    if (ap_count == 0) {
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }
    /* Cap at 16 to keep the JSON response small on this little heap. */
    if (ap_count > 16) ap_count = 16;

    wifi_ap_record_t *records = calloc(ap_count, sizeof(*records));
    if (records == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }
    esp_wifi_scan_get_ap_records(&ap_count, records);

    /* Build JSON. Each entry: {"ssid":"...","rssi":-65,"auth":2}.
     * Escape backslash and double-quote in SSID. */
    char *buf = malloc(2048);
    if (buf == NULL) {
        free(records);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }
    size_t off = 0;
    off += snprintf(buf + off, 2048 - off, "[");
    bool first = true;
    for (uint16_t i = 0; i < ap_count && off < 2000; i++) {
        /* Skip hidden / empty SSIDs. */
        if (records[i].ssid[0] == 0) continue;

        ESP_LOGI(TAG, "  AP[%u]: ssid='%s' rssi=%d auth=%d",
                 i, records[i].ssid, records[i].rssi, records[i].authmode);

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
                        first ? "" : ",", esc,
                        records[i].rssi, records[i].authmode);
        first = false;
    }
    off += snprintf(buf + off, 2048 - off, "]");

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_ret = httpd_resp_send(req, buf, off);
    free(records);
    free(buf);
    return send_ret;
}

/* Minimal JSON parser for {"ssid":"X","pass":"Y","agent":<bool>,"host":"Z"}.
 * Avoids pulling in cJSON to keep heap pressure low during provisioning.
 * Returns ESP_OK on success; outputs are NUL-terminated and bounded by the
 * supplied sizes.
 *
 *   out_agent  : defaults to false if "agent" key is missing
 *   out_host   : NUL-terminated; leading/trailing whitespace stripped
 *                (SSID/pass keep their whitespace — see test_wifi_creds.c) */
static esp_err_t parse_provisioning_post(const char *body, size_t body_len,
                                         char *out_ssid, size_t ssid_size,
                                         char *out_pass, size_t pass_size,
                                         char *out_host, size_t host_size,
                                         bool *out_agent)
{
    if (body == NULL || body_len == 0) return ESP_ERR_INVALID_ARG;
    if (out_agent) *out_agent = false;
    if (out_host) out_host[0] = '\0';

    const char *p = body;
    const char *end = body + body_len;

    /* Find required "ssid" / "pass" keys. The agent fields are optional. */
    const char *ssid_key  = strstr(p, "\"ssid\"");
    const char *pass_key  = strstr(p, "\"pass\"");
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

    /* Optional: "agent":<bool|int|"on">. Accepts the JSON literals true/false,
     * the integer 1, and the HTML form-encoded string "on". Anything else
     * (including a missing key) leaves *out_agent at its false default. */
    if (out_agent) {
        const char *agent_key = strstr(p, "\"agent\"");
        if (agent_key) {
            const char *colon = strchr(agent_key, ':');
            if (colon) {
                colon++;
                while (colon < end && (*colon == ' ' || *colon == '\t')) colon++;
                if (colon < end && (*colon == 't' || *colon == '1')) {
                    *out_agent = true;
                }
                /* 'f' / '0' / '"on"' with leading quote → leave at false. */
            }
        }
    }

    /* Optional: "host":"<bare host>". Strip leading/trailing whitespace so a
     * stray space doesn't make esp_http_client fail with a malformed URL. */
    if (out_host) {
        const char *host_key = strstr(p, "\"host\"");
        if (host_key) {
            const char *colon = strchr(host_key, ':');
            if (!colon) return ESP_ERR_INVALID_ARG;
            colon++;
            while (colon < end && (*colon == ' ' || *colon == '\t')) colon++;
            if (colon >= end || *colon != '"') return ESP_ERR_INVALID_ARG;
            colon++;

            /* Skip leading whitespace inside the value. */
            while (colon < end && (*colon == ' ' || *colon == '\t')) colon++;

            size_t oi = 0;
            while (colon < end && *colon != '"' && oi < host_size - 1) {
                if (*colon == '\\' && colon + 1 < end) colon++;
                out_host[oi++] = *colon++;
            }
            out_host[oi] = '\0';

            /* Trim trailing whitespace. */
            while (oi > 0 && (out_host[oi - 1] == ' ' || out_host[oi - 1] == '\t')) {
                out_host[--oi] = '\0';
            }
        }
    }

    return ESP_OK;
}

/* Validate a parsed host string. Returns the default if the input is empty
 * or contains characters that would break the URL (':', '/', or any whitespace).
 * Used by api_wifi_handler() before agent_config_save().
 *
 * Mirrored in tests/test_agent_config.c. */
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

static esp_err_t api_wifi_handler(httpd_req_t *req)
{
    /* Read body up to 384 bytes (SSID 32, pass 64, host 63, plus JSON keys
     * and overhead). */
    char body[384];
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no body\"}", HTTPD_RESP_USE_STRLEN);
    }
    body[recv_len] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    char host[AGENT_HOST_MAX + 1] = {0};
    bool agent_enabled = false;
    esp_err_t err = parse_provisioning_post(body, recv_len,
                                            ssid, sizeof(ssid),
                                            pass, sizeof(pass),
                                            host, sizeof(host),
                                            &agent_enabled);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad json\"}", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "POST /api/wifi ssid='%s' pass_len=%u agent=%d host='%s'",
             ssid, (unsigned)strlen(pass), agent_enabled, host);

    /* Save WiFi creds first — if this fails, the user has bigger problems
     * than a missing agent URL. */
    wifi_creds_t creds = { .configured = true };
    strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
    strncpy(creds.pass, pass, sizeof(creds.pass) - 1);

    err = wifi_creds_save(&creds);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"save failed\"}", HTTPD_RESP_USE_STRLEN);
    }

    /* Save agent config. If the host is empty or contains URL-breaking
     * characters (':', '/', whitespace), fall back to the default. Preserving
     * the host when the checkbox is unchecked is intentional — re-checking
     * later brings the user's typed host back. */
    const char *safe_host = sanitize_host(host);
    agent_config_t agent = {0};
    strncpy(agent.host, safe_host, sizeof(agent.host) - 1);
    agent.host[sizeof(agent.host) - 1] = '\0';
    agent.enabled = agent_enabled;

    esp_err_t agent_err = agent_config_save(&agent);
    if (agent_err != ESP_OK) {
        /* Don't fail the whole submission — WiFi creds are already saved
         * and the user expects a reboot. */
        ESP_LOGW(TAG, "agent_config_save failed: %s, continuing",
                 esp_err_to_name(agent_err));
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
    cfg.max_open_sockets = 2;   /* provisioning only needs 1-2 clients */
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

    /* Limit both interfaces to 802.11b/g. Excluding 802.11n (HT) avoids
     * compatibility issues with routers — notably Xiaomi — that don't respond
     * to HT probe requests. wifi_sta_connect() does the same for the STA-only
     * path; here we apply it before the first esp_wifi_start() in the APSTA
     * path (wifi_sta_connect(30000) returned early when NVS was empty, so the protocol
     * was never configured — defaulted to 11b/g/n). */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));

    /* Start the radio. wifi_sta_connect() (called just above) initialised the
     * driver and netif but — when NVS is empty — returns early without calling
     * esp_wifi_start(). Without this the AP is configured but silent: no
     * beacons, invisible to phones. */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }
    wifi_mark_radio_started();  /* keep wifi.c's s_wifi_started in sync */

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

    /* Ensure WiFi driver + STA netif are up before we try to add the AP netif
     * or call esp_wifi_set_mode(APSTA). On first boot (NVS empty), main.c
     * hasn't called wifi_sta_connect() yet, so the driver is uninitialised —
     * which is what blew up on real hardware. */
    /* Best-effort: if NVS has creds, this connects; if not, it fails after 30 s
     * ESP_FAIL if NVS is empty (no menuconfig fallback any more — the device
     * refuses to silently connect to a baked-in SSID). Either way, the driver
     * is up afterward. */
    (void)wifi_sta_connect(30000);

    /* Block the STA event handler from auto-connecting. Must come AFTER
     * wifi_sta_connect() — the handler skips esp_wifi_connect() when this flag
     * is set, so calling wifi_sta_connect() with suppression on would deadlock
     * wifi_wait_connected() for 30s (STA_START fires, no connect, no IP). */
    wifi_suppress_auto_connect(true);

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

    /* The QR page was already initialised and loaded by main.c at boot
     * (no NVS creds → routed to the QR page). Nothing to do here. */

    ESP_LOGI(TAG, "Waiting for credentials (timeout %ds)", CONFIG_WIFI_PROV_TIMEOUT_SECS);

    TickType_t ticks = pdMS_TO_TICKS(CONFIG_WIFI_PROV_TIMEOUT_SECS * 1000);
    EventBits_t bits = xEventGroupWaitBits(s_events,
                                            WIFI_PROV_BIT_DONE | WIFI_PROV_BIT_ABORT,
                                            pdFALSE, pdFALSE, ticks);

    /* Tear everything down before returning, regardless of outcome. */
    stop_http_server(s_httpd);  s_httpd = NULL;
    stop_dns_redirect();
    stop_softap();

    /* The QR page stays loaded — caller decides whether to reboot (success)
     * or to keep it visible while main.c falls through to clock-only mode. */

    /* Switch WiFi mode back to STA-only so the next wifi_sta_connect() call
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