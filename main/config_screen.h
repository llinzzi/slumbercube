#ifndef CONFIG_SCREEN_H
#define CONFIG_SCREEN_H

/* Switch the OLED to the provisioning screen (AP SSID label + QR code +
 * "open 192.168.4.1" hint), and back to the clock screen afterwards.
 *
 * Implemented in config_screen.c; does not depend on EEZ-generated
 * screens.c (which lives on a sibling branch) — it builds its own
 * lv_obj root and lv_screen_load()s it directly.
 */
void config_screen_show(const char *ap_ssid, const char *ap_pass);
void config_screen_hide(void);

#endif // CONFIG_SCREEN_H