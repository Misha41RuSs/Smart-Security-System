#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "cloud.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define EVENT_CAPACITY 64
#define TAG "cloud"
typedef struct {
    char ssid[33], password[65], url[193], id[37], token[65];
} node_config_t;
typedef struct {
    char id[33];
    uint8_t kind;
    int64_t occurred_ms, uptime_ms;
} stored_event_t;
typedef struct {
    uint32_t version, count, lost;
    stored_event_t events[EVENT_CAPACITY];
} event_store_t;
static node_config_t config;
static event_store_t outbox;
static nvs_handle_t storage;
static SemaphoreHandle_t lock;
static uint8_t latest[18];
static bool have_sample, configured, reconfigure, wifi_ready, cloud_ready, storage_failed;
static bool initialized;
static void (*command_handler)(security_command_t);
static const uint8_t alert_bits[] = {SECURITY_TOO_CLOSE, SECURITY_BRIGHT, SECURITY_OVERHEAT};
static const char *alert_names[] = {"DISTANCE", "LIGHT", "OVERHEAT"};

static void save_events(void)
{
    esp_err_t err = nvs_set_blob(storage, "events", &outbox, sizeof(outbox));
    if (err == ESP_OK) err = nvs_commit(storage);
    storage_failed = err != ESP_OK;
    if (err != ESP_OK) ESP_LOGE(TAG, "Could not persist alerts: %s", esp_err_to_name(err));
}

void cloud_publish(const uint8_t packet[18], uint8_t rising)
{
    if (!initialized) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(latest, packet, 18);
    have_sample = true;
    bool changed = false;
    for (int i = 0; i < 3; i++) {
        if (!(rising & alert_bits[i])) continue;
        changed = true;
        if (outbox.count == EVENT_CAPACITY) {
            outbox.lost++;
            ESP_LOGW(TAG, "Alert outbox full, newest event dropped");
            continue;
        }
        stored_event_t *event = &outbox.events[outbox.count++];
        memset(event, 0, sizeof(*event));
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        for (int j = 0; j < 16; j++) snprintf(event->id + 2*j, 3, "%02x", random[j]);
        event->kind = i;
        event->uptime_ms = esp_timer_get_time()/1000;
        time_t now = time(NULL);
        // A timestamp without SNTP/RTC synchronization would be misleading.
        event->occurred_ms = now > 1700000000 ? (int64_t)now*1000 : 0;
    }
    if (changed) save_events();
    xSemaphoreGive(lock);
}

static bool copy_string(cJSON *root, const char *name, char *dest, size_t capacity, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) return !required;
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= capacity) return false;
    strcpy(dest, item->valuestring);
    return true;
}

esp_err_t cloud_configure(const char *json)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }
    xSemaphoreTake(lock, portMAX_DELAY);
    node_config_t next = config;
    bool cloud_fields = cJSON_HasObjectItem(root, "url") || cJSON_HasObjectItem(root, "id") || cJSON_HasObjectItem(root, "token");
    bool valid = copy_string(root,"ssid",next.ssid,sizeof(next.ssid),true)
        && copy_string(root,"password",next.password,sizeof(next.password),true)
        && copy_string(root,"url",next.url,sizeof(next.url),cloud_fields)
        && copy_string(root,"id",next.id,sizeof(next.id),cloud_fields)
        && copy_string(root,"token",next.token,sizeof(next.token),cloud_fields)
        && strlen(next.ssid)>0
        && (strlen(next.password)==0 || (strlen(next.password)>=8 && strlen(next.password)<=63));
    if (cloud_fields) {
        valid = valid && strncmp(next.url,"https://",8)==0 && strlen(next.url)>8
            && !strpbrk(next.url,"?#@ \r\n") && strlen(next.id)==36
            && strspn(next.id,"0123456789abcdef-")==36
            && strlen(next.token)==64 && strspn(next.token,"0123456789abcdef")==64;
        size_t len = strlen(next.url);
        if (len && next.url[len-1]=='/') next.url[len-1]=0;
    }
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (valid) {
        err = nvs_set_blob(storage,"config",&next,sizeof(next));
        if (err == ESP_OK) err = nvs_commit(storage);
        if (err == ESP_OK) {
            config = next;
            configured = true;
            reconfigure = true;
            cloud_ready = false;
        }
    }
    xSemaphoreGive(lock);
    cJSON_Delete(root);
    return err;
}

void cloud_status(uint8_t result[4])
{
    memset(result,0,4); result[0]=1;
    if (!initialized) { result[1]=8; return; }
    xSemaphoreTake(lock,portMAX_DELAY);
    result[1] = (configured?1:0) | (wifi_ready?2:0) | (cloud_ready?4:0) | (storage_failed?8:0);
    result[2] = outbox.count;
    result[3] = outbox.lost > 255 ? 255 : outbox.lost;
    xSemaphoreGive(lock);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xSemaphoreTake(lock,portMAX_DELAY);
    if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) wifi_ready=true;
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) { wifi_ready=false; cloud_ready=false; }
    xSemaphoreGive(lock);
}

typedef struct { char bytes[2048]; size_t size; bool overflow; } http_response_t;
static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_response_t *r = event->user_data;
    if (event->event_id==HTTP_EVENT_ON_DATA) {
        if (r->size+event->data_len >= sizeof(r->bytes)) { r->overflow=true; return ESP_FAIL; }
        memcpy(r->bytes+r->size,event->data,event->data_len);
        r->size+=event->data_len; r->bytes[r->size]=0;
    }
    return ESP_OK;
}

static cJSON *exchange(const node_config_t *cfg, cJSON *payload)
{
    char url[256], authorization[80];
    snprintf(url,sizeof(url),"%s/api/nodes/%s/poll",cfg->url,cfg->id);
    snprintf(authorization,sizeof(authorization),"Bearer %s",cfg->token);
    http_response_t response = {0};
    esp_http_client_config_t settings = {
        .url=url, .method=HTTP_METHOD_POST, .timeout_ms=8000,
        .crt_bundle_attach=esp_crt_bundle_attach, .event_handler=http_event,
        .user_data=&response, .disable_auto_redirect=true,
        .buffer_size=1024, .buffer_size_tx=1024,
    };
    esp_http_client_handle_t client=esp_http_client_init(&settings);
    if (!client) return NULL;
    char *body=cJSON_PrintUnformatted(payload);
    if (!body) { esp_http_client_cleanup(client); return NULL; }
    esp_http_client_set_header(client,"Content-Type","application/json");
    esp_http_client_set_header(client,"Authorization",authorization);
    esp_http_client_set_post_field(client,body,strlen(body));
    esp_err_t err=esp_http_client_perform(client);
    int status=esp_http_client_get_status_code(client);
    cJSON *result = err==ESP_OK && status==200 && !response.overflow ? cJSON_Parse(response.bytes) : NULL;
    if (!result) ESP_LOGW(TAG,"Cloud request failed: %s HTTP %d",esp_err_to_name(err),status);
    esp_http_client_cleanup(client); free(body);
    return result;
}

static void cloud_task(void *arg)
{
    int64_t lease_until=0, last_command=0;
    const char *last_status="APPLIED";
    bool wifi_started=false;
    unsigned retry_seconds=3;
    while (1) {
        node_config_t cfg;
        xSemaphoreTake(lock,portMAX_DELAY);
        cfg=config;
        bool apply=reconfigure, ready=wifi_ready;
        reconfigure=false;
        xSemaphoreGive(lock);
        if (apply && cfg.ssid[0]) {
            if (wifi_started) { esp_wifi_stop(); }
            xSemaphoreTake(lock,portMAX_DELAY); wifi_ready=false; cloud_ready=false; xSemaphoreGive(lock);
            wifi_config_t wifi={0};
            memcpy(wifi.sta.ssid,cfg.ssid,strlen(cfg.ssid));
            memcpy(wifi.sta.password,cfg.password,strlen(cfg.password));
            wifi.sta.pmf_cfg.capable=true;
            wifi.sta.threshold.authmode=cfg.password[0]?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
            esp_err_t err=esp_wifi_set_config(WIFI_IF_STA,&wifi);
            if (err==ESP_OK) err=esp_wifi_start();
            wifi_started=err==ESP_OK;
            if (wifi_started) esp_wifi_connect();
            ready=false; lease_until=0; last_command=0;
        }
        if (!ready) {
            if (wifi_started) esp_wifi_connect();
            vTaskDelay(pdMS_TO_TICKS(3000)); continue;
        }
        if (!cfg.url[0] || !cfg.token[0]) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
        cJSON *request=cJSON_CreateObject();
        if (!request) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
        cJSON *events=cJSON_AddArrayToObject(request,"alerts");
        xSemaphoreTake(lock,portMAX_DELAY);
        if (have_sample && esp_timer_get_time()/1000<lease_until) {
            char hex[37];
            for (int i=0;i<18;i++) snprintf(hex+2*i,3,"%02x",latest[i]);
            cJSON_AddStringToObject(request,"packet",hex);
        }
        cJSON_AddNumberToObject(request,"lostAlerts",outbox.lost);
        for (unsigned i=0;i<outbox.count && i<8;i++) {
            const stored_event_t *e=&outbox.events[i];
            cJSON *item=cJSON_CreateObject();
            cJSON_AddStringToObject(item,"id",e->id);
            cJSON_AddStringToObject(item,"kind",alert_names[e->kind]);
            if(e->occurred_ms) cJSON_AddNumberToObject(item,"occurredAt",e->occurred_ms);
            cJSON_AddNumberToObject(item,"uptimeMs",e->uptime_ms);
            cJSON_AddItemToArray(events,item);
        }
        xSemaphoreGive(lock);
        cJSON_AddNumberToObject(request,"ackId",last_command);
        cJSON_AddStringToObject(request,"ackStatus",last_status);
        int64_t request_started=esp_timer_get_time()/1000;
        cJSON *reply=exchange(&cfg,request);
        int64_t request_duration=esp_timer_get_time()/1000-request_started;
        cJSON_Delete(request);
        bool ok=cJSON_IsObject(reply) && cJSON_IsBool(cJSON_GetObjectItem(reply,"telemetry"))
            && cJSON_IsArray(cJSON_GetObjectItem(reply,"ackEvents"));
        xSemaphoreTake(lock,portMAX_DELAY);
        // Ignore in-flight replies after provisioning to another cloud/device.
        if (memcmp(&cfg,&config,sizeof(cfg))!=0) ok=false;
        cloud_ready=ok;
        if (ok) {
            unsigned previous=outbox.count;
            cJSON *id;
            cJSON_ArrayForEach(id,cJSON_GetObjectItem(reply,"ackEvents")) {
                if (!cJSON_IsString(id)) continue;
                for (unsigned i=0;i<outbox.count;i++) {
                    if (strcmp(outbox.events[i].id,id->valuestring)!=0) continue;
                    memmove(&outbox.events[i],&outbox.events[i+1],(outbox.count-i-1)*sizeof(stored_event_t));
                    outbox.count--; break;
                }
            }
            if (previous!=outbox.count) save_events();
        }
        xSemaphoreGive(lock);
        if (ok) {
            cJSON *lease=cJSON_GetObjectItem(reply,"leaseMs");
            double ms=cJSON_IsNumber(lease)?lease->valuedouble:0;
            if(ms<0 || ms>15000) ms=0;
            lease_until=request_started+(cJSON_IsTrue(cJSON_GetObjectItem(reply,"telemetry"))?(int64_t)ms:0);
            cJSON *command=cJSON_GetObjectItem(reply,"command");
            cJSON *id=cJSON_GetObjectItem(command,"id"), *code=cJSON_GetObjectItem(command,"code");
            cJSON *ttl=cJSON_GetObjectItem(command,"ttlMs");
            if(cJSON_IsNumber(id) && cJSON_IsNumber(code) && cJSON_IsNumber(ttl)
                && ttl->valuedouble>request_duration && id->valuedouble>last_command) {
                last_command=(int64_t)id->valuedouble;
                bool supported=command_handler && code->valueint>=1 && code->valueint<=4;
                last_status=supported?"APPLIED":"REJECTED";
                if(supported) command_handler((security_command_t)code->valueint);
            }
            retry_seconds=3;
        } else {
            lease_until=0;
            retry_seconds=retry_seconds<30?retry_seconds*2:30;
        }
        cJSON_Delete(reply);
        vTaskDelay(pdMS_TO_TICKS(retry_seconds*1000));
    }
}

esp_err_t cloud_start(void (*handler)(security_command_t))
{
    lock=xSemaphoreCreateMutex();
    if(!lock) return ESP_ERR_NO_MEM;
    command_handler=handler;
    ESP_RETURN_ON_ERROR(nvs_flash_init(),TAG,"default NVS init");
    ESP_RETURN_ON_ERROR(nvs_flash_init_partition("cloud"),TAG,"cloud NVS init");
    ESP_RETURN_ON_ERROR(nvs_open_from_partition("cloud","node",NVS_READWRITE,&storage),TAG,"cloud NVS open");
    size_t length=sizeof(config);
    if(nvs_get_blob(storage,"config",&config,&length)==ESP_OK && length==sizeof(config)) {
        configured=true; reconfigure=true;
    }
    length=sizeof(outbox);
    if(nvs_get_blob(storage,"events",&outbox,&length)!=ESP_OK || length!=sizeof(outbox) || outbox.version!=1 || outbox.count>EVENT_CAPACITY) {
        memset(&outbox,0,sizeof(outbox)); outbox.version=1;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(),TAG,"netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(),TAG,"event loop");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init),TAG,"wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM),TAG,"wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),TAG,"wifi mode");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL),TAG,"wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL),TAG,"IP events");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0,"pool.ntp.org");
    esp_sntp_init();
    if (xTaskCreate(cloud_task,"cloud",8192,NULL,3,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    initialized=true;
    return ESP_OK;
}
