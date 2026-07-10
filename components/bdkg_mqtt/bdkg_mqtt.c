// E7 (#BDKG-7): MQTT-паблишер БДКГ-05 + HA discovery
#include "bdkg_mqtt.h"
#include "bdkg_usb.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

static const char *TAG = "bdkg_mqtt";
#define NVS_NS "mqtt"
#define PUB_PERIOD_MS 10000

// ----------------------------------------------------------------------------
//  Статическое состояние
// ----------------------------------------------------------------------------
static SemaphoreHandle_t          s_lock;
static bdkg_mqtt_cfg_t            s_cfg;
static bdkg_mqtt_stat_t           s_stat;
static esp_mqtt_client_handle_t   s_client;
static bool                       s_started;
static TaskHandle_t               s_task;
static char                       s_devid[7];
static char                       s_topic_avail[40];
static char                       s_topic_state[40];

#define CFGLOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define CFGUNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

// ----------------------------------------------------------------------------
//  NVS (namespace "mqtt") — write-on-change (#NVS-1: не трогаем флэш зря)
// ----------------------------------------------------------------------------
static void nvs_load(bdkg_mqtt_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t en = 0;
    if (nvs_get_u8(h, "en", &en) == ESP_OK) c->enabled = en ? true : false;
    size_t l;
    l = sizeof(c->uri);  nvs_get_str(h, "uri",  c->uri,  &l);
    l = sizeof(c->user); nvs_get_str(h, "user", c->user, &l);
    l = sizeof(c->pass); nvs_get_str(h, "pass", c->pass, &l);
    nvs_close(h);
}

static int nvs_save(const bdkg_mqtt_cfg_t *c)
{
    bdkg_mqtt_cfg_t cur;
    nvs_load(&cur);
    if (memcmp(&cur, c, sizeof(cur)) == 0) {
        ESP_LOGD(TAG, "nvs_save: no change, skip (NVS wear guard)");
        return 0;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = ESP_OK;
    e |= nvs_set_u8 (h, "en",   c->enabled ? 1 : 0);
    e |= nvs_set_str(h, "uri",  c->uri);
    e |= nvs_set_str(h, "user", c->user);
    e |= nvs_set_str(h, "pass", c->pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "nvs_save failed (0x%x)", (int)e); return -1; }
    return 0;
}

// ----------------------------------------------------------------------------
//  Вспомогательные функции
// ----------------------------------------------------------------------------
static bool contains_ci(const char *hay, const char *needle_lc)
{
    size_t nl = strlen(needle_lc);
    if (!nl) return false;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && (char)tolower((unsigned char)p[i]) == needle_lc[i]) i++;
        if (i == nl) return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
//  HA discovery
// ----------------------------------------------------------------------------
static cJSON *disc_device(void)
{
    cJSON *dev = cJSON_CreateObject();
    if (!dev) return NULL;
    char dev_ids[64];
    snprintf(dev_ids, sizeof(dev_ids), "bdkg05_%s", s_devid);
    cJSON_AddStringToObject(dev, "ids", dev_ids);
    cJSON_AddStringToObject(dev, "name", "BDKG-05 Bridge");
    cJSON_AddStringToObject(dev, "mf",   "ATOMTEX");
    cJSON_AddStringToObject(dev, "mdl",  "BDKG-05 / ESP32-S3");
    return dev;
}

static void publish_discovery(esp_mqtt_client_handle_t client)
{
    static const struct {
        const char *key, *name, *unit, *tpl, *dev_cla, *stat_cla, *ent_cat;
    } sensors[] = {
        {"med",     "МЭД",                     "µSv/h", "{{ value_json.med }}",       NULL,        "measurement",  NULL},
        {"med_err", "Погрешность МЭД",         "%",     "{{ value_json.med_err }}",   NULL,        "measurement",  "diagnostic"},
        {"cps",     "ЦПС",                     "cps",   "{{ value_json.cps }}",       NULL,        "measurement",  NULL},
        {"cps_err", "Погрешность ЦПС",         "%",     "{{ value_json.cps_err }}",   NULL,        "measurement",  "diagnostic"},
        {"temp",    "Температура детектора",  "°C",    "{{ value_json.temp }}",      "temperature", "measurement",  NULL},
        {"dose",    "Накопленная доза",        "µSv",   "{{ value_json.dose }}",      NULL,        "total_increasing", NULL},
        {NULL}
    };

    for (int i = 0; sensors[i].key; i++) {
        const __typeof__(sensors[0]) *s = &sensors[i];
        char topic[128];
        snprintf(topic, sizeof(topic), "homeassistant/sensor/bdkg05_%s/%s/config", s_devid, s->key);
        cJSON *obj = cJSON_CreateObject();
        if (!obj) continue;
        cJSON_AddStringToObject(obj, "name", s->name);
        char uniq_id[64];
        snprintf(uniq_id, sizeof(uniq_id), "bdkg05_%s_%s", s_devid, s->key);
        cJSON_AddStringToObject(obj, "uniq_id", uniq_id);
        cJSON_AddStringToObject(obj, "stat_t", s_topic_state);
        cJSON_AddStringToObject(obj, "avty_t", s_topic_avail);
        cJSON_AddStringToObject(obj, "unit_of_meas", s->unit);
        cJSON_AddStringToObject(obj, "val_tpl", s->tpl);
        if (s->dev_cla) cJSON_AddStringToObject(obj, "dev_cla", s->dev_cla);
        if (s->stat_cla) cJSON_AddStringToObject(obj, "stat_cla", s->stat_cla);
        if (s->ent_cat) cJSON_AddStringToObject(obj, "ent_cat", s->ent_cat);
        cJSON *dev = disc_device();
        if (dev) cJSON_AddItemToObjectCS(obj, "dev", dev);
        char *json = cJSON_PrintUnformatted(obj);
        if (json) {
            esp_mqtt_client_publish(client, topic, json, 0, 1, 1);
            free(json);
        }
        cJSON_Delete(obj);
    }

    // binary_sensor для подключения
    char topic[128];
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/bdkg05_%s/conn/config", s_devid);
    cJSON *obj = cJSON_CreateObject();
    if (obj) {
        cJSON_AddStringToObject(obj, "name", "Дозиметр подключён");
        char uniq_id[64];
        snprintf(uniq_id, sizeof(uniq_id), "bdkg05_%s_conn", s_devid);
        cJSON_AddStringToObject(obj, "uniq_id", uniq_id);
        cJSON_AddStringToObject(obj, "stat_t", s_topic_state);
        cJSON_AddStringToObject(obj, "avty_t", s_topic_avail);
        cJSON_AddStringToObject(obj, "val_tpl", "{{ value_json.conn }}");
        cJSON_AddStringToObject(obj, "pl_on", "1");
        cJSON_AddStringToObject(obj, "pl_off", "0");
        cJSON_AddStringToObject(obj, "dev_cla", "connectivity");
        cJSON_AddStringToObject(obj, "ent_cat", "diagnostic");
        cJSON *dev = disc_device();
        if (dev) cJSON_AddItemToObjectCS(obj, "dev", dev);
        char *json = cJSON_PrintUnformatted(obj);
        if (json) {
            esp_mqtt_client_publish(client, topic, json, 0, 1, 1);
            free(json);
        }
        cJSON_Delete(obj);
    }
}

// ----------------------------------------------------------------------------
//  MQTT event handler
// ----------------------------------------------------------------------------
static void mqtt_event_cb(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        CFGLOCK(); s_stat.connected = true; CFGUNLOCK();
        esp_mqtt_client_publish(ev->client, s_topic_avail, "online", 0, 1, 1);
        publish_discovery(ev->client);
        ESP_LOGI(TAG, "connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        CFGLOCK(); s_stat.connected = false; CFGUNLOCK();
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error event");
        break;
    default:
        break;
    }
}

// ----------------------------------------------------------------------------
//  Клиент
// ----------------------------------------------------------------------------
static void client_apply(const bdkg_mqtt_cfg_t *c)
{
    if (s_client && s_started) {
        esp_mqtt_client_stop(s_client);
        s_started = false;
        CFGLOCK(); s_stat.connected = false; CFGUNLOCK();
    }
    if (!c->enabled || !c->uri[0]) {
        ESP_LOGI(TAG, "MQTT disabled (no config)");
        return;
    }

    esp_mqtt_client_config_t mc = { 0 };
    mc.broker.address.uri = c->uri;
    if (c->user[0]) mc.credentials.username = c->user;
    if (c->pass[0]) mc.credentials.authentication.password = c->pass;

    mc.session.last_will.topic  = s_topic_avail;
    mc.session.last_will.msg    = "offline";
    mc.session.last_will.qos    = 1;
    mc.session.last_will.retain = 1;

    if (!s_client) {
        s_client = esp_mqtt_client_init(&mc);
        if (!s_client) {
            ESP_LOGW(TAG, "client init failed");
            return;
        }
        esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_cb, NULL);
    } else {
        esp_mqtt_set_config(s_client, &mc);
    }

    if (esp_mqtt_client_start(s_client) == ESP_OK) s_started = true;
    else ESP_LOGW(TAG, "client start failed");
}

// ----------------------------------------------------------------------------
//  Publish task
// ----------------------------------------------------------------------------
static void pub_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(PUB_PERIOD_MS));
        CFGLOCK();
        bool conn = s_stat.connected;
        esp_mqtt_client_handle_t cl = s_client;
        CFGUNLOCK();
        if (!cl || !conn) continue;

        bdkg_snapshot_t s;
        bool valid = bdkg_usb_get_latest(&s);
        char js[192];
        int n;
        if (valid)
            n = snprintf(js, sizeof(js),
                "{\"med\":%.3f,\"med_err\":%.1f,\"cps\":%.1f,\"cps_err\":%.1f,\"temp\":%.1f,\"dose\":%.2f,\"conn\":\"1\"}",
                s.med_sv_h * 1e6, s.med_err_pct, s.cps_inst, s.cps_err_pct, s.temp_c, s.dose_sv * 1e6);
        else
            n = snprintf(js, sizeof(js), "{\"conn\":\"0\"}");

        int res = esp_mqtt_client_publish(cl, s_topic_state, js, n, 0, 0);
        if (res >= 0) {
            CFGLOCK();
            s_stat.published++;
            s_stat.last_pub_at = (int64_t)time(NULL);
            CFGUNLOCK();
        }
    }
}

// ----------------------------------------------------------------------------
//  Public API
// ----------------------------------------------------------------------------
void bdkg_mqtt_init(void)
{
    if (s_task) return;
    s_lock = xSemaphoreCreateMutex();
    memset(&s_stat, 0, sizeof(s_stat));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_devid, sizeof(s_devid), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_topic_avail, sizeof(s_topic_avail), "bdkg05/%s/availability", s_devid);
    snprintf(s_topic_state, sizeof(s_topic_state), "bdkg05/%s/state", s_devid);

    nvs_load(&s_cfg);
    ESP_LOGI(TAG, "init: enabled=%d uri=%s auth=%s id=%s", s_cfg.enabled, s_cfg.uri[0] ? s_cfg.uri : "(none)", s_cfg.user[0] ? "user" : "none", s_devid);

    client_apply(&s_cfg);
    xTaskCreate(pub_task, "bdkg_mqtt", 4096, NULL, 5, &s_task);
}

void bdkg_mqtt_get_cfg(bdkg_mqtt_cfg_t *out)
{
    if (!out) return;
    CFGLOCK();
    *out = s_cfg;
    CFGUNLOCK();
}

int bdkg_mqtt_set_cfg(const bdkg_mqtt_cfg_t *c)
{
    if (!c) return BDKG_MQTT_ERR_INVALID;

    bdkg_mqtt_cfg_t n;
    memset(&n, 0, sizeof(n));
    n.enabled = c->enabled;
    snprintf(n.uri,  sizeof(n.uri),  "%s", c->uri);
    snprintf(n.user, sizeof(n.user), "%s", c->user);
    snprintf(n.pass, sizeof(n.pass), "%s", c->pass);

    if (contains_ci(n.uri, "narodmon")) {
        ESP_LOGW(TAG, "set_cfg: refused narodmon uri (ban)");
        return BDKG_MQTT_ERR_BLOCKED;
    }

    if (n.enabled && strncmp(n.uri, "mqtt://", 7) != 0 && strncmp(n.uri, "mqtts://", 8) != 0)
        return BDKG_MQTT_ERR_INVALID;

    if (nvs_save(&n) != 0) return BDKG_MQTT_ERR_NVS;

    CFGLOCK();
    s_cfg = n;
    CFGUNLOCK();

    client_apply(&n);
    return BDKG_MQTT_OK;
}

void bdkg_mqtt_get_stat(bdkg_mqtt_stat_t *out)
{
    if (!out) return;
    CFGLOCK();
    *out = s_stat;
    CFGUNLOCK();
}
