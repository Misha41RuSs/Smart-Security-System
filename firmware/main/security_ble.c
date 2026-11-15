#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/endian.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "security_ble.h"
#include "cloud.h"

#ifdef SECURITY_CLIMATE_NODE
#define DEVICE_NAME "ESP32-CL"
#else
#define DEVICE_NAME "ESP32-SS"
#endif
#define PROTOCOL_VERSION 1
#define STATE_SIZE 18
#define NOTIFY_INTERVAL_MS 100

// UUID: d91fc001/2/3-cc92-4e99-9c67-2f7396311bce. NimBLE expects little endian.
#define SECURITY_UUID(id) BLE_UUID128_INIT(0xce, 0x1b, 0x31, 0x96, 0x73, 0x2f, 0x67, 0x9c, \
                                          0x99, 0x4e, 0x92, 0xcc, (id), 0xc0, 0x1f, 0xd9)
static const ble_uuid128_t service_uuid = SECURITY_UUID(0x01);
static const ble_uuid128_t command_uuid = SECURITY_UUID(0x02);
static const ble_uuid128_t state_uuid = SECURITY_UUID(0x03);
static const ble_uuid128_t config_uuid = SECURITY_UUID(0x04);
static const ble_uuid128_t network_uuid = SECURITY_UUID(0x05);
static char config_buffer[1024];
static size_t config_length;
static bool config_started;
static const char *TAG = "security_ble";

static void (*on_command)(security_command_t);
static uint8_t own_addr_type;
static uint16_t state_handle;
static uint16_t subscriber = BLE_HS_CONN_HANDLE_NONE;
static struct ble_npl_callout notify_timer;
static uint8_t last_notification[STATE_SIZE];
static bool sent_notification;

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static security_telemetry_t telemetry = {
#ifdef SECURITY_CLIMATE_NODE
    .flags = SECURITY_CLIMATE_ONLY,
#endif
    .distance_tenths_cm = UINT16_MAX,
    .light_adc = UINT16_MAX,
    .temperature_tenths_c = INT16_MIN,
    .humidity_tenths_pct = UINT16_MAX,
    .pressure_tenths_hpa = UINT16_MAX,
};
static const uint8_t alert_flags[] = { SECURITY_TOO_CLOSE, SECURITY_BRIGHT, SECURITY_OVERHEAT };
static uint16_t alert_counts[sizeof(alert_flags)];
static void encode_state(uint8_t packet[STATE_SIZE]);

void security_ble_publish(const security_telemetry_t *reading)
{
    taskENTER_CRITICAL(&state_lock);
    const uint8_t rising = reading->flags & ~telemetry.flags;
    for (size_t i = 0; i < sizeof(alert_flags); i++) {
        if (rising & alert_flags[i]) {
            alert_counts[i]++;
        }
    }
    telemetry = *reading;
    taskEXIT_CRITICAL(&state_lock);
    uint8_t packet[STATE_SIZE];
    encode_state(packet);
    cloud_publish(packet, rising);
}

static void encode_state(uint8_t packet[STATE_SIZE])
{
    taskENTER_CRITICAL(&state_lock);
    packet[0] = PROTOCOL_VERSION;
    packet[1] = telemetry.flags;
    put_le16(packet + 2, telemetry.distance_tenths_cm);
    put_le16(packet + 4, telemetry.light_adc);
    put_le16(packet + 6, (uint16_t)telemetry.temperature_tenths_c);
    put_le16(packet + 8, telemetry.humidity_tenths_pct);
    put_le16(packet + 10, telemetry.pressure_tenths_hpa);
    for (size_t i = 0; i < sizeof(alert_flags); i++) {
        put_le16(packet + 12 + 2 * i, alert_counts[i]);
    }
    taskEXIT_CRITICAL(&state_lock);
}

static int read_state(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t packet[STATE_SIZE];
    encode_state(packet);
    return os_mbuf_append(ctxt->om, packet, sizeof(packet)) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int write_command(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (on_command == NULL) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    uint8_t command[2];
    if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(command) ||
        ble_hs_mbuf_to_flat(ctxt->om, command, sizeof(command), NULL) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (command[0] != PROTOCOL_VERSION || command[1] < SECURITY_DISARM ||
        command[1] > SECURITY_SIREN_ON) {
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    on_command((security_command_t)command[1]);
    return 0;
}

static int provision(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t frame[20];
    uint16_t length=OS_MBUF_PKTLEN(ctxt->om);
    if (length<2 || length>20 || ble_hs_mbuf_to_flat(ctxt->om,frame,sizeof(frame),NULL)!=0 || frame[0]!=1)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (frame[1]==0 && length==2) {
        memset(config_buffer,0,sizeof(config_buffer));
        config_length=0; config_started=true; return 0;
    }
    if (!config_started) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    if (frame[1]==1 && length>4) {
        size_t offset=frame[2]|((size_t)frame[3]<<8);
        if(offset!=config_length || config_length+length-4>=sizeof(config_buffer))
            return BLE_ATT_ERR_INVALID_OFFSET;
        memcpy(config_buffer+config_length,frame+4,length-4);
        config_length+=length-4; return 0;
    }
    if (frame[1]==2 && length==2) {
        config_buffer[config_length]=0;
        esp_err_t err=cloud_configure(config_buffer);
        memset(config_buffer,0,sizeof(config_buffer)); config_started=false; config_length=0;
        return err==ESP_OK ? 0 : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
}

static int read_network(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t status[4]; cloud_status(status);
    return os_mbuf_append(ctxt->om,status,sizeof(status))==0?0:BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (const struct ble_gatt_chr_def[]) {
            {
                .uuid = &command_uuid.u,
                .access_cb = write_command,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_AUTHEN,
                .min_key_size = 16,
            },
            {
                .uuid = &state_uuid.u,
                .access_cb = read_state,
                .val_handle = &state_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY,
                .min_key_size = 16,
            },
            {
                .uuid = &config_uuid.u,
                .access_cb = provision,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_AUTHEN,
                .min_key_size = 16,
            },
            {
                .uuid = &network_uuid.u,
                .access_cb = read_network,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN,
                .min_key_size = 16,
            },
            {0},
        },
    },
    {0},
};

static void notify_subscriber(struct ble_npl_event *event)
{
    if (subscriber == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct ble_gap_conn_desc conn;
    if (ble_gap_conn_find(subscriber, &conn) == 0 &&
        conn.sec_state.encrypted && conn.sec_state.authenticated && conn.sec_state.key_size == 16) {
        uint8_t packet[STATE_SIZE];
        encode_state(packet);
        if (!sent_notification || memcmp(packet, last_notification, sizeof(packet)) != 0) {
            struct os_mbuf *value = ble_hs_mbuf_from_flat(packet, sizeof(packet));
            if (value != NULL && ble_gatts_notify_custom(subscriber, state_handle, value) == 0) {
                memcpy(last_notification, packet, sizeof(packet));
                sent_notification = true;
            }
        }
    }
    ble_npl_callout_reset(&notify_timer, ble_npl_time_ms_to_ticks32(NOTIFY_INTERVAL_MS));
}

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    const struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &service_uuid,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
        .name = (const uint8_t *)DEVICE_NAME,
        .name_len = sizeof(DEVICE_NAME) - 1,
        .name_is_complete = 1,
    };
    const struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc == 0) {
        rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising failed: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
        } else {
            int rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "Security request failed: %d", rc);
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        memset(config_buffer,0,sizeof(config_buffer)); config_length=0; config_started=false;
        subscriber = BLE_HS_CONN_HANDLE_NONE;
        ble_npl_callout_stop(&notify_timer);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == state_handle) {
            if (event->subscribe.cur_notify) {
                subscriber = event->subscribe.conn_handle;
                sent_notification = false;
                ble_npl_callout_reset(&notify_timer, ble_npl_time_ms_to_ticks32(NOTIFY_INTERVAL_MS));
            } else {
                subscriber = BLE_HS_CONN_HANDLE_NONE;
                ble_npl_callout_stop(&notify_timer);
            }
        }
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io key = { .action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000 };
            ESP_LOGI(TAG, "BLE pairing PIN: %06lu", (unsigned long)key.passkey);
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &key);
            if (rc != 0) {
                ESP_LOGW(TAG, "Pairing PIN rejected: %d", rc);
            }
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status != 0) {
            ESP_LOGW(TAG, "BLE pairing/encryption failed: %d", event->enc_change.status);
        }
        break;
    default:
        break;
    }
    return 0;
}

static void host_reset(int reason)
{
    subscriber = BLE_HS_CONN_HANDLE_NONE;
    ble_npl_callout_stop(&notify_timer);
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

static void host_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc == 0) {
        advertise();
    } else {
        ESP_LOGE(TAG, "BLE address unavailable: %d", rc);
    }
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

esp_err_t security_ble_start(void (*command_handler)(security_command_t))
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }
    err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    on_command = command_handler;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc == 0) {
        rc = ble_gatts_count_cfg(services);
    }
    if (rc == 0) {
        rc = ble_gatts_add_svcs(services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT initialization failed: %d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    ble_npl_callout_init(&notify_timer, nimble_port_get_dflt_eventq(), notify_subscriber, NULL);
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
