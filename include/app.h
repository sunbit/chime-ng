#ifndef CHIME_APP_H
#define CHIME_APP_H
#include "platform.h"
#include "led.h"
#include "codec_bus.h"
extern Config config;
extern bool configured, ap_mode;
extern char ap_name[32];
bool app_save_config(Config *c);
bool app_set_volume(unsigned volume);
bool app_busy(void);
void app_reboot_later(void);
bool app_time_valid(void);
void app_set_time(uint32_t unix_seconds);
bool audio_init(void);
bool audio_ready(void);
bool audio_play(void);
bool audio_playing(void);
const char *audio_error(void);
const char *audio_diagnostic(void);
void audio_poll(void);
void audio_use_sound(uint32_t offset,uint32_t length);
void sound_init(void);
const char *sound_path(void);
bool sound_begin(uint32_t bytes);
bool sound_write(const void *data,size_t n);
bool sound_finish(void);
void sound_abort(void);
bool sound_busy(void);
bool http_start(void);
void http_stop(void);
bool http_running(void);
const char *http_status(void);
void http_poll(void);
bool ota_begin(uint32_t bytes);
bool ota_write(const void *p,size_t n);
bool ota_finish(void);
void ota_abort(void);
bool ota_busy(void);
bool ota_confirm(void);
bool ota_confirmed(void);
void mqtt_poll(void);
void mqtt_shutdown(void);
bool mqtt_ready(void);
const char *mqtt_status(void);
#define MQTT_LED_COMMAND_SIZE 160
const char *mqtt_led_command(void);
#endif
