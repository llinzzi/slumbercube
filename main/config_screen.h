#ifndef CONFIG_SCREEN_H
#define CONFIG_SCREEN_H

/* Standalone QR-config page used on first boot (no NVS creds). The page
 * is its own LVGL screen — clock and QR are two separate pages, not a
 * swap on top of one. Boot routes to one or the other based on NVS. */
void config_screen_init(const char *ap_ssid, const char *ap_pass);
void config_screen_show(void);

#endif
