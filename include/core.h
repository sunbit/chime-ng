#ifndef CHIME_CORE_H
#define CHIME_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_BASE 0x20010000u
#define RAM_END 0x20040000u
#define IMAGE_MAX 0x20000u
#define SOUND_MAX_BYTES (512u*1024u)
#define IMAGE_MAGIC "CHIMNG01"
#define BOARD_ID 0x43483332u
#define FW_VERSION "0.2.1"
#define FW_BUILD 12u
#define BOOT_MAGIC 0x43484231u
#define CONFIG_MAGIC 0x43484331u
#define NO_SLOT 255u
#define WAV_PATH "/chime/original.wav"
#define CONFIG_PATH "/chime/config.bin"
#define BOOT_PATH "/chime/boot.bin"
#define CA_PATH "/chime/ca.der"

typedef struct { uint32_t state[8]; uint64_t bytes; uint8_t block[64]; } Sha256;
void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);
bool constant_equal(const void *a, const void *b, size_t n);
uint32_t crc32(const void *p, size_t n);

typedef struct {
    char magic[8];
    uint32_t board, format, load_address, size, build, flags;
    uint8_t sha[32];
} ImageHeader;
typedef struct {
    uint32_t magic;
    uint8_t active, pending, tried, reserved;
    uint32_t generation, crc;
} BootRecord;
typedef struct {
    uint32_t magic, version;
    char ssid[33], wifi_password[65];
    char broker[128], mqtt_user[65], mqtt_password[129], ntp_server[16];
    char homie_id[49], name[97];
    uint16_t mqtt_port;
    uint8_t mqtt_tls, volume;
    uint8_t admin_hash[32];
    uint32_t crc;
} Config;

bool image_header_valid(const ImageHeader *h);
bool image_vectors_valid(const ImageHeader *h, const uint8_t vectors[8]);
bool boot_record_valid(const BootRecord *b);
void boot_record_seal(BootRecord *b);
int boot_running_slot(const BootRecord *b);
unsigned boot_select(BootRecord *b);
bool homie_id_valid(const char *s);
bool config_valid(const Config *c);
void config_seal(Config *c);
bool parse_uint(const char *s, uint32_t max, uint32_t *out);
void ip_format(uint32_t ip, char out[16]);
bool form_get(const char *form, const char *key, char *dst, size_t capacity);
size_t json_escape(char *out, size_t cap, const char *s);
bool basic_password(const char *authorization, char *out, size_t capacity);
bool wav_header(const uint8_t *p, size_t n, uint32_t file_size,
                uint32_t *offset, uint32_t *length);
size_t homie_description(char *out, size_t capacity, const char *name);

/* MQTT 5 wire framing: -1 invalid, 0 incomplete, >0 total packet size. */
int mqtt_frame_size(const uint8_t *p, size_t n, size_t max, size_t *header);
size_t mqtt_varint(uint8_t out[4], size_t value);
typedef struct {
    const uint8_t *topic, *payload;
    size_t topic_len, payload_len;
    uint8_t qos;
    bool retained, duplicate;
    uint16_t packet_id;
} MqttPublish;
bool mqtt_parse_publish(const uint8_t *packet, size_t n, MqttPublish *out);
bool play_command_valid(const MqttPublish *p, const char *topic, bool ready, bool busy);
typedef struct {
    char method[8],path[48],authorization[240],content_type[64];
    uint32_t length;
    bool length_present,custom_header;
} HttpRequest;
bool http_parse_headers(char *buffer,HttpRequest *request);
#endif
