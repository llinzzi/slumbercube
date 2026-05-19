# 功能开发路线图

**日期:** 2026-05-19 | **基础版本:** v2.0 (固件基线)

---

## 阶段一：体验完善 (P1 · 短期)

优化现有功能的可用性和鲁棒性，降低用户上手成本。

---

### 1.1 天气数据缓存

**目标:** 网络不可用时仍能显示最近一次的天气数据，避免空屏。

#### 技术分析

**现状 (`weather_service.h`)**: `weather_data_t` 结构体包含 4 天预报，每个预报含 `int high/low`、`char day_text[16]`、`char night_text[16]`。总大小可算：
```
4 × (4 + 4 + 16 + 16 + 4 + 4) = 4 × 48 = 192 字节
+ fetch_time (8) + count (4) + valid (1)
≈ 205 字节
```
NVS 单 key 上限 1984 字节，203 字节放得下。

**`weather_fetch()` 当前仅填充传入的栈上 data 指针** — 在 `main.c:25` 声明为 `static weather_data_t s_weather`，每次唤醒重新 fetch。需要第一次 fetch 成功后持久化，后续唤醒先读 NVS 填充 `s_weather`，再尝试刷新。

#### 实现方案

**新增 `weather_cache.h`:**

```c
esp_err_t weather_cache_save(const weather_data_t *data);
esp_err_t weather_cache_load(weather_data_t *data);
bool weather_cache_has(void);
```

**新增 `weather_cache.c`:**

```c
#define WEATHER_NVS_NS "weather"
#define WEATHER_NVS_KEY "forecast"

esp_err_t weather_cache_save(const weather_data_t *data)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(WEATHER_NVS_NS, NVS_READWRITE, &h));
    esp_err_t err = nvs_set_blob(h, WEATHER_NVS_KEY, data, sizeof(*data));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t weather_cache_load(weather_data_t *data)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(WEATHER_NVS_NS, NVS_READONLY, &h));
    size_t sz = sizeof(*data);
    esp_err_t err = nvs_get_blob(h, WEATHER_NVS_KEY, data, &sz);
    nvs_close(h);
    return err;
}

bool weather_cache_has(void)
{
    nvs_handle_t h;
    if (nvs_open(WEATHER_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = 0;
    esp_err_t err = nvs_get_blob(h, WEATHER_NVS_KEY, NULL, &sz);
    nvs_close(h);
    return err == ESP_OK && sz > 0;
}
```

#### 修改点

| 文件 | 变更 |
|------|------|
| `weather_service.h` | 新加 `weather_cache_*` 声明 |
| **新增 `weather_cache.c`** | 序列化/反序列化实现 |
| `main.c` | init 阶段从 `weather_cache_load()` 填充 `s_weather` 并 `screens_set_weather_data_ptr()` → 接着网络刷新 → 成功后 `weather_cache_save()` |
| `CMakeLists.txt` | 添加 `weather_cache.c` 到源码列表 |

#### `main.c` 改动示意

```c
// 显示模式（夜间检查之后）
if (!clock_screen_is_night_time()) {
    // 1. 先加载缓存显示
    if (weather_cache_has()) {
        weather_cache_load(&s_weather);
        screens_set_weather_data_ptr(&s_weather);
    }

    // 2. 连接 WiFi
    clock_screen_set_station_name("Connecting WiFi...");
    wifi_ensure_netif();
    wifi_init_sta();

    // 3. 刷新天气并更新缓存
    clock_screen_set_station_name("Fetching weather...");
    for (int retry = 0; retry < 5; retry++) {
        if (weather_fetch(&s_weather) == ESP_OK) {
            weather_cache_save(&s_weather);
            screens_set_weather_data_ptr(&s_weather);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
} else {
    // 夜间模式也尝试加载缓存
    if (weather_cache_has()) {
        weather_cache_load(&s_weather);
        screens_set_weather_data_ptr(&s_weather);
    }
}
```

#### 边界情况

| 场景 | 行为 |
|------|------|
| 首次上电，无缓存，WiFi 成功 | 无缓存跳过，正常 fetch 后保存 |
| 首次上电，无缓存，WiFi 失败 | 无缓存，跳过天气，显示无天气 |
| 有缓存，WiFi 成功 | 先显缓存，刷新后静默替换 |
| 有缓存，WiFi 失败 | 保持缓存显示，不显示"刷新失败" |
| 缓存中 fetch_time 为旧数据 | 刷新时才更新，缓存不关心时效（展示上次有效数据） |

**测试要点:** NVS blob 读写、空天/满 4 天数据的序列化一致性、无 WiFi 时屏幕仍有天气、wifi 闪断后缓存续用

**难度:** ★☆☆☆☆ | **估算:** 0.5 天

---

### 1.2 WiFi 配网页面 (Captive Portal)

**目标:** 新设备或 WiFi 信息变化时，用户无需重编译即可通过网络页面配置。

#### 方案选型

| 方案 | 复杂度 | 额外依赖 | 用户操作步骤 |
|------|--------|----------|-------------|
| **A. SoftAP + DNS 劫持 (推荐)** | ★★★☆☆ | 无（ESP-IDF 内置） | 连 AP → 浏览器 → 点提交 → 重启 |
| B. BLE 配网 | ★★★★☆ | NimBLE / Bluedroid | 打开 App → 扫描 → 输入 → 发送 |
| C. ESP IDF 原生 `wifi_provisioning` | ★★☆☆☆ | ESP-IDF `wifi_provisioning` 组件 | 手机 App 扫码配网 |

**推荐理由:** 方案 A 无需额外硬件或 App，浏览器是人类已知的 UI。方案 C (ESP-prov) 是封装好的，但依赖蓝牙和 protobuf，增加 flash 占用（2MB 下可能不够）。

#### 方案 A 详细设计

**启动流程:**

```
app_main()
  ├── wifi_config 是否存在于 NVS？
  │   ├── 是 → 正常启动（现有流程）
  │   └── 否 → 进入配网模式
  │         ├── 创建 SoftAP (ssid="SSD1322-Config", password 留空)
  │         ├── 启动 DNS 服务器，劫持所有域名 → 返回配置页面
  │         ├── HTTP Server 监听 80
  │         │   ├── GET  /  → 返回 HTML 配置页面
  │         │   ├── POST /configure
  │         │   │      body: ssid=xxx&password=yyy
  │         │   │      → 保存到 NVS
  │         │   │      → return {"status":"ok"}
  │         │   └── POST /skip
  │         │        → 写入空配置标记，重启后跳过配网
  │         ├── 用户提交后 → esp_restart()
  │         └── 超时 5min → 进入深度睡眠（下次唤醒再配）
  └── 正常启动（从 NVS 读取 ssid/pass 连接到 WiFi）
```

**配置页面 HTML** 需内嵌在固件中（常量字符串），极小化设计：
- 显示时钟名称 + 简单说明（中英文）
- SSID 输入框
- 密码输入框
- "连接" 按钮 + "跳过" 按钮

**页面设计:** 纯文本 + 输入框，无图无 CSS 框架，aprox. 2KB HTML。

**NVS 存储结构:**

| Key | 类型 | 说明 |
|-----|------|------|
| `wifi_ssid` | string | 最多 32 字节 |
| `wifi_pass` | string | 最多 64 字节 |
| `wifi_configured` | u8 | 1=已配置, 0=未配置 |

#### 修改点

| 文件 | 变更 |
|------|------|
| **新增 `wifi_provision.h`** | 声明 `provision_enter_portal_mode()`, `provision_is_configured()` |
| **新增 `wifi_provision.c`** | SoftAP + DNS + HTTP 服务器实现 |
| **新增 `wifi_provision_html.h`** | HTML 页面字符串（编译时可编辑） |
| `main.c` | `app_main()` 入口判断：`if (!provision_is_configured()) { provision_enter_portal_mode(); }` |
| `wifi.c` | `wifi_init_sta()` 改为从 NVS 读取 ssid/password，而非 Kconfig 宏（fallback 到 Kconfig） |

#### `wifi_provision.c` 核心实现

```c
#include "esp_netif.h"
#include "esp_wifi.h"

static void start_softap(void)
{
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "SSD1322-Config",
            .ssid_len = 13,
            .max_connection = 1,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
}

// DNS 劫持: 所有 A 记录查询返回 SoftAP IP
// HTTP handler: GET / → 返回 HTML, POST /configure → 解析 form data

static esp_err_t dns_cb(dns_handle_t handle, const char *name, dns_type_t type, ...)
{
    // 对 A 类型查询返回 ap_ip
}
```

**DNS 劫持实现方式:**
- 使用 IDF 的 `lwip` DNS 服务器 API，或手动创建 UDP socket 监听 53
- 推荐用 `dns_server` API: `dns_server_create(dns_query_cb, ...)`
- 所有查询到的域名统一返回 SoftAP 的 IP

**HTTP 服务器实现:**
- 使用 `esp_http_server` 组件
- `httpd_start()` 创建实例
- `httpd_register_uri_handler()` 注册 GET / 和 POST /configure

**HTML 页面 (内联):**

```html
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<title>SSD1322 Clock Config</title>
<style>body{font-family:sans-serif;padding:20px;max-width:400px;margin:auto}
input{width:100%;padding:8px;margin:6px 0;box-sizing:border-box}
button{width:100%;padding:10px;margin:4px 0}</style>
</head><body>
<h2>SSD1322 Clock</h2>
<p>Configure WiFi to continue:</p>
<form method=POST action=/configure>
<input name=ssid placeholder="WiFi SSID" required>
<input type=password name=password placeholder="Password">
<button type=submit>Connect</button>
</form>
<form method=POST action=/skip>
<button type=submit>Skip (no network)</button>
</form>
</body></html>
```

#### 连接恢复流程

配网成功 → NVS 保存 → `esp_restart()` → `provision_is_configured()` 返回 true → 正常流程。

修改密码? 恢复出厂设置:
- 上电按住按钮保持 10 秒（在 `main.c` 初始化阶段检测）
- 擦除 NVS wifi 配置 key
- 重启进入配网模式

#### 边界情况

| 场景 | 行为 |
|------|------|
| 用户输入错误密码 | 配网保存后重启，WiFi 连接失败 → 回到配网模式（需要检测首次连接失败）|
| 用户不操作 | 5 分钟超时 → 深睡，下次唤醒继续配网 |
| 跳过配网 | 写入空标记，正常启动（无网络）|
| 已经配网但想更换 | 长按按钮 10s 擦除配置 → 重启进配网模式 |

**测试要点:** 正确连接 AP 并打开页面、DNS 劫持是否覆盖所有域名、POST form 数据解析、NVS 读写稳定性、长按擦除逻辑

**难度:** ★★★☆☆ | **估算:** 2-3 天

---

### 1.3 错误状态 UI 反馈

**目标:** 用户开机时能看到操作进展和错误信息，而非空等或直接跳过。

#### 技术分析

**现状 (`main.c:111-132`)**: 网络操作前设置 station name "Connecting WiFi..." 和 "Fetching weather..."。但 station name 位于屏幕底部（y=42），字号小（font_station 10px），容易被忽略。WiFi 连接失败或天气获取失败后，station name 会保持最后的错误文本，除非音频播放后 ICY 元数据覆盖。

**改进: 使用中间状态横幅**

在屏幕中央区域（原 weather icon + text 位置，y=2~36）显示大号状态提示，覆盖 weather 区域。支持自动消失。

#### 实现方案

**新增 `clock_screen_status.c/h`：**

```c
typedef enum {
    STATUS_INFO,
    STATUS_WARNING,
    STATUS_ERROR
} status_severity_t;

void status_show(const char *text, status_severity_t severity, uint32_t duration_ms);
void status_hide(void);
void status_tick(void);  // 在 ui_tick 中调用，管理自动消失计时
```

**内部实现:**

1. 创建全局 `lv_obj_t *status_label`，与 weather 区域位置大小匹配（置中，大号字体 font_weather 或 font_digital 的子集）
2. `status_show()`: 设置文本 + 颜色（绿/黄/红），覆盖 weather 区域
3. 设置定时器，`duration_ms` 后自动 `status_hide()` → 恢复 weather 显示
4. `status_hide()`: 主动刷新 weather 区域

#### 修改点

| 文件 | 变更 |
|------|------|
| **新增 `clock_screen_status.c`** | 状态横幅实现 |
| **新增 `clock_screen_status.h`** | 状态 API 声明 |
| `clock_screen.c` | `clock_screen_create()` 中创建 status_label 对象（默认隐藏）|
| `main.c` | 关键点加状态调用（见下方）|
| `ui/ui.c` | `ui_tick()` 中调 `status_tick()` |

#### `main.c` 状态提示集成

```c
// 夜间模式检查之后，网络启动前
status_show("Connecting WiFi...", STATUS_INFO, 0);  // 0=永不过期

if (wifi_init_sta() == ESP_OK) {
    status_show("Fetching weather...", STATUS_INFO, 0);

    for (int retry = 0; retry < 5; retry++) {
        if (weather_fetch(&s_weather) == ESP_OK) {
            status_hide();
            break;
        }
        if (retry < 4) {
            status_show("Weather retrying...", STATUS_WARNING, 1000);
        } else {
            status_show("Weather failed", STATUS_ERROR, 3000);
        }
    }
} else {
    status_show("WiFi failed", STATUS_ERROR, 3000);
    // 仍然尝试从缓存加载天气
    if (weather_cache_has()) {
        weather_cache_load(&s_weather);
        screens_set_weather_data_ptr(&s_weather);
        status_hide();
    }
}
```

夜间模式所有 status 调用跳过（不走网络，无状态的必要）。

#### 状态信息总表

| 阶段 | 消息 | 严重性 | 持续 |
|------|------|--------|------|
| 时间/时区 | — | — | 不显示（成功不是新闻）|
| 初始化 LVGL | — | — | 不显示 |
| 正在连接 WiFi | "Connecting WiFi..." | info | 完成前一直显示 |
| WiFi 连接成功 | — | — | 隐式清除 |
| WiFi 连接失败 | "WiFi failed" | error | 3s 后消失 |
| 正在获取天气 | "Fetching weather..." | info | 完成前一直显示 |
| 天气获取成功 | — | — | 隐式清除 |
| 天气重试 | "Weather retry n/5" | warning | 1-2s |
| 天气最终失败 | "Weather unavailable" | error | 3s |
| 音频初始化失败 | "Audio init failed" | warning | 2s |
| 网络不可用 | "No network" | warning | 3s（音频阶段）|

**测试要点:** 所有状态按照预期显示/消失、夜间模式不显示状态、状态文字不超出屏幕、WiFi 快速成功时状态一闪即逝是否可接受

**难度:** ★☆☆☆☆ | **估算:** 0.5 天

---

### 1.4 按钮交互增强

**目标:** 手势极简，只做两件事——短按睡觉，长按在夜间手动启动网络和音乐。

#### 设计理念

两个手势，不做多层级，零误触：

- **短按** — 任何时候都是"关掉"，立即深度睡眠
- **长按** — 夜间模式下连接 WiFi + 播放音乐（日间 WiFi/音频已自动启动，长按无操作）

不需要停止播放的手势——短按深睡就是最强的停止。不需要双击刷新天气——每次唤醒已经自动刷新。

#### 手势定义

| 手势 | 日间模式 (6:00-22:00) | 夜间模式 (22:00-6:00) |
|------|-----------------------|-----------------------|
| **短按** | 立即深度睡眠 | 立即深度睡眠 |
| **长按 (~1s)** | 无操作 | **连接 WiFi + 播放音乐** |

#### 修改点

| 文件 | 变更 |
|------|------|
| `main.c` | 注册 `BUTTON_SINGLE_CLICK` 和 `BUTTON_LONG_PRESS_START` 两个回调 |
| `main.c` | `app_main()` 夜间分支不自动启动 WiFi/音频，等长按触发 |
| `audio_player_wrapper.h` | 保持现有 API（`audio_init`、`audio_play_url` 即可） |

#### `main.c` 实现

```c
/* ── 按钮事件处理 ── */

static void on_short_press(void *arg, void *usr)
{
    ESP_LOGI(TAG, "Short press → sleep");
    s_sleep_pending = true;
}

// 长按：夜间模式下手动启动网络+音频
static void on_long_press(void *arg, void *usr)
{
#if CONFIG_AUDIO_ENABLE
    if (!clock_screen_is_night_time()) {
        return;  // 日间已自动启动，长按无操作
    }

    ESP_LOGI(TAG, "Long press → manual WiFi + audio in night mode");

    status_show("Connecting...", STATUS_INFO, 0);

    // wifi_init_sta() 内部会调 sntp_init_time() 自动校时
    wifi_ensure_netif();
    if (wifi_init_sta() != ESP_OK) {
        status_show("WiFi failed", STATUS_ERROR, 3000);
        return;
    }

    status_show("Starting audio...", STATUS_INFO, 0);

    if (audio_init() == ESP_OK) {
        audio_play_url(CONFIG_AUDIO_MUSIC_URL);
        status_hide();
    } else {
        status_show("Audio failed", STATUS_ERROR, 2000);
    }
#endif
}

void app_main(void)
{
    // ...
    button_config_t btn_cfg = {
        .short_press_time = 200,
        .long_press_time = 1000,
    };
    iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &g_btn);

    iot_button_register_cb(g_btn, BUTTON_SINGLE_CLICK, NULL, on_short_press, NULL);
    iot_button_register_cb(g_btn, BUTTON_LONG_PRESS_START, NULL, on_long_press, NULL);
}
```

#### `main.c` 夜间分支改动

```c
if (!clock_screen_is_night_time()) {
    /* 日间：自动 WiFi + 天气 + 音频（与现状一致） */
    wifi_ensure_netif();
    wifi_init_sta();

    for (int retry = 0; retry < 5; retry++) {
        if (weather_fetch(&s_weather) == ESP_OK) {
            weather_cache_save(&s_weather);
            screens_set_weather_data_ptr(&s_weather);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

#if CONFIG_AUDIO_ENABLE
    if (audio_init() == ESP_OK) {
        audio_play_url(CONFIG_AUDIO_MUSIC_URL);
    }
#endif
} else {
    /* 夜间：不启动 WiFi/音频，等长按 */
    ESP_LOGI(TAG, "Night mode — long press to start network & audio");

    if (weather_cache_has()) {
        weather_cache_load(&s_weather);
        screens_set_weather_data_ptr(&s_weather);
    }
}
```

#### 边界情况

| 场景 | 行为 |
|------|------|
| 夜间长按后 WiFi 失败 | 显示 "WiFi failed"，3s 后消失，保持纯时钟 |
| 夜间长按后音频失败 | WiFi 和天气正常，显示 "Audio failed" |
| 夜间长按后再次长按 | 无操作（已在运行） |
| 日间长按 | 无操作 |
| 任意时刻短按 | 立即设 `s_sleep_pending`，主循环退出后深睡 |

**测试要点:** 夜间长按触发 WiFi+音频、短按睡眠始终响应、日间长按不影响功能

**难度:** ★★☆☆☆ | **估算:** 1 天

---

### 1.5 Flash 容量确认与修复

**目标:** 消除 README 和 sdkconfig 之间的 flash 容量矛盾。

#### 技术分析

**现状:**
- `README.md`: 声称 "4MB Flash"
- `sdkconfig`: `CONFIG_ESPTOOLPY_FLASHSIZE="2MB"`
- `sdkconfig`: `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`
  - single_app_large 分区表: app 0x1E0000 (~1.9MB), 其他约 0x10000
- 编译产物大小: ~1.37 MB (当前固件)

若实际 flash 为 **2MB**:
- single_app_large 分区表 OK（app 分区 ~1.9MB > 1.37MB）
- 切换到 OTA 双分区则不够（每分区 < 1MB）
- 若后续要加 OTA，必须换 4MB

若实际 flash 为 **4MB**:
- 应将 `CONFIG_ESPTOOLPY_FLASHSIZE="4MB"` 修正
- 可以规划 OTA 分区

#### 操作步骤

1. **物理检查:** 查看 ESP32-C3-MINI-1 模组丝印或 PCB 上 flash 芯片型号，确认容量。在 C3 上 `esptool.py flash_id` 也可识别 flash 容量。

2. **修正配置:**
   - 2MB → 保持 `single_app_large`，但更新 README 为 2MB
   - 4MB → 更新 sdkconfig `CONFIG_ESPTOOLPY_FLASHSIZE="4MB"`，保留 single_app_large 或预留 OTA 分区

3. **验证:** `idf.py build` 无错误，`esptool.py flash_id` 输出与配置一致

#### 修改点

| 文件 | 条件 | 变更 |
|------|------|------|
| `README.md` | 确认为 2MB | 修正硬件规格为 2MB |
| `sdkconfig` | 确认为 4MB | `CONFIG_ESPTOOLPY_FLASHSIZE="4MB"` |
| `sdkconfig` | 若要预留 OTA | 更新分区表为 `partitions_ota.csv` |

**难度:** ★☆☆☆☆ | **估算:** 0.5 天

---

## 阶段二：功能扩展 (P2· 中期)

增加核心功能缺口，完善交互体验。

### 2.1 闹钟功能

- **描述:** 用户设置闹钟时刻，到点播放蜂鸣音或自动开启网络电台
- **方案:**
  - 在 NVS 中持久化闹钟设置（小时/分钟/启用/音源选择）
  - 唤醒后检查是否到达闹钟时间
  - 闹钟触发：播放蜂鸣音（PWM 到 NS4168）或自动播放电台 URL
  - 按按钮关闭闹钟
- **影响文件:** 新增 `alarm.c/h`, 修改 `main.c`, `Kconfig.projbuild`
- **难度:** ★★★☆☆
- **估算:** 2-3 天

### 2.2 OTA 固件升级

- **问题:** 固件更新需 USB 串口烧录，不方便
- **方案:**
  - 分区表切换为 OTA 分区方案（otadata + 2x app 分区）
  - 检查当前固件能否容纳双分区（需确认 2MB Flash 是否够用，不够需考虑压缩或升级 4MB Flash）
  - 实现 OTA 检查/下载逻辑：启动时检查服务器是否有新版本
  - 触发方式：WiFi 连接成功后后台检查，或下次编译后手动触发
- **影响文件:** 新增 `ota.c/h`, 修改分区表、`sdkconfig`、`main.c`
- **注意:** 2MB Flash 下 OTA 可能空间紧张（每分区 < 1MB），建议硬件确认后升级 4MB Flash
- **难度:** ★★★★★
- **估算:** 3-5 天

### 2.3 FM116C LED 灯效

- **问题:** 硬件预留的 LED 驱动无固件支持
- **方案:**
  - 实现 FM116C 双 H 桥的 GPIO 控制
  - 夜间模式：低亮度氛围灯
  - 闹钟唤醒：渐亮呼吸灯
  - 音频播放：节奏光效（低精度，简单的平均振幅→亮度映射）
- **影响文件:** 新增 `led_driver.c/h`, 修改 `main.c`, `clock_screen.c`
- **难度:** ★★☆☆☆
- **估算:** 1-2 天

### 2.4 多屏切换

- **问题:** 目前仅有一屏（主时钟/天气界面）
- **方案:**
  - 利用现有 EEZ Studio UI 框架的多屏支持
  - 新增屏：纯日期屏、天气详情屏、设置屏
  - 通过按钮长按或双击切换
  - 支持屏间平滑过渡（现有 fade 动画可用）
- **影响文件:** `ui/screens.c`, `clock_screen.c`（拆分）, 新增 `settings_screen.c`, `forecast_screen.c`
- **难度:** ★★★☆☆
- **估算:** 2-3 天

### 2.5 运行时持久化设置

- **问题:** 所有配置仅编译时确定，无法在运行时调整
- **方案:**
  - 实现 NVS 键值存储层：亮度、音量、夜间模式开关、音频 URL
  - 初始化时读取 NVS，覆盖 Kconfig 默认值
  - 通过按钮组合或设置屏调整数值并保存
- **影响文件:** 新增 `settings.c/h`, 修改 `clock_screen.c`, `audio_player_wrapper.c`, `main.c`
- **难度:** ★★★☆☆
- **估算:** 1-2 天

---

## 阶段三：进阶特性 (P3 · 长期)

提升产品的差异化价值和品质感。

### 3.1 月相与天文数据

- **描述:** 获取并显示月相、日出日落时间
- **方案:**
  - AMAP API 支持获取天文数据（需确认接口是否对免费用户开放）
  - 或本地根据日期计算月相（算法简单，无需 API）和日出日落（需经纬度）
  - 在时钟屏角落或天气预报旁显示
- **影响文件:** 新增 `astronomy.c/h`, 修改 `clock_screen.c`
- **难度:** ★★☆☆☆

### 3.2 多语言支持

- **问题:** 字体仅支持中文 weather 描述
- **方案:**
  - 分离用户界面字符串和字体
  - 增加英文字体配置（现有 digital-7 已是英文数字）
  - 下拉语言选择（中/英）
- **影响文件:** 字体配置、`clock_screen.c`
- **难度:** ★★☆☆☆

### 3.3 低功耗分析与优化

- **问题:** 未做显式功耗测量和优化
- **方案:**
  - 测量各阶段功耗：初始化、WiFi 连接、天气获取、音频播放、深睡
  - 优化：
    - WiFi 扫描跳过（已做 — channel 1 直连）
    - 缩短 SNTP 轮询超时
    - 减少 I2S 音频运行时的 CPU 占用
    - 优化深睡电流（确认 GPIO hold 的功耗贡献）
- **影响文件:** 跨多个模块
- **难度:** ★★★☆☆

### 3.4 音频播放增强

- **方案:**
  - 支持本地 microSD 播放（需添加 SPI SD 卡驱动）
  - 支持播放列表（NVS 或 URL 列表）
  - 播控：切歌、快进/退（通过按钮组合）
- **难度:** ★★★★☆

### 3.5 网络增强

- **方案:**
  - WiFi 自动回退：连接失败后开启 AP 模式供诊断
  - 多 WiFi 配置存储（家庭/办公室自动切换）
  - 断线自动重连（现有实现，可增加指数退避）
- **难度:** ★★☆☆☆

---

## 优先级矩阵

| 功能 | 用户价值 | 实现成本 | 技术风险 | 优先级 |
|------|----------|----------|----------|--------|
| 天气缓存 | 高 | 低 | 低 | P1 |
| WiFi 配网 | 高 | 中 | 中 | P1 |
| 错误状态 UI | 中 | 低 | 低 | P1 |
| 按钮增强 | 中 | 低 | 低 | P1 |
| 闹钟 | 高 | 中 | 中 | P2 |
| OTA 升级 | 中 | 高 | 高 | P2 |
| LED 灯效 | 中 | 低 | 低 | P2 |
| 多屏切换 | 中 | 中 | 中 | P2 |
| 持久化设置 | 中 | 中 | 低 | P2 |
| 月相 | 低 | 低 | 低 | P3 |
| 多语言 | 低 | 中 | 低 | P3 |
| 功耗优化 | 中 | 中 | 中 | P3 |

---

## 版本规划建议

```
v2.1 (体验改进)      ← P1 功能
├── 天气数据缓存
├── WiFi 配网页面
├── 错误状态 UI
├── 按钮交互增强
└── Flash 容量修复

v2.2 (核心补充)      ← P2 高价值功能
├── 闹钟
├── FM116C LED 灯效
├── 多屏切换
└── 持久化设置

v2.3 (进阶)          ← P2/P3 混合
├── OTA 升级
├── 月相显示
└── 低功耗优化

v3.0 (品质提升)      ← P3 差异化
├── 多语言
├── 本地 SD 播放
├── 网络增强
└── 综合打磨
```

---

## 依赖管理与技术债

| 项 | 说明 |
|---|------|
| ESP-IDF 版本 | 当前 v5.5.2，保持跟随 Espressif 官方 release |
| LVGL 版本 | ^9.4.0，需关注 9.x 的 breaking changes |
| esp-audio-player | Git submodule，需注意上游更新 |
| 天气 API 依赖 | AMAP 免费 API 有调用频次限制，更改服务商需调整解析逻辑 |
| 分区表 | 从 singleapp 迁移到 OTA 方案时需重建分区表 |
