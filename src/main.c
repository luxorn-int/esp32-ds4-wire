#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_gap_bt_api.h"
#include "esp_hidh.h"
#include "esp_idf_version.h"
#include "esp_hid_common.h"
#include "esp_hidh_bluedroid.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/* --- ИСПРАВЛЕННЫЙ БЛОК НАСТРОЕК --- */
#define BRIDGE_UART             UART_NUM_0   // <--- ВЕРНУЛИ ОБРАТНО
#define BRIDGE_BAUD_RATE        921600     // Увеличено для плавности
#define UART_RX_BUFFER_SIZE     4096         // Увеличено
#define UART_TX_BUFFER_SIZE     16384        // Увеличено

#define MAX_FRAME_PAYLOAD       1024
#define MAX_TX_PAYLOAD          512
#define FRAME_OVERHEAD          12
#define TX_QUEUE_ITEM_SIZE      (FRAME_OVERHEAD + MAX_TX_PAYLOAD)
#define TX_QUEUE_DEPTH          32           // <--- ОСТАВИЛИ ТОЛЬКО ОДНО ОПРЕДЕЛЕНИЕ (удалите старое 8)
/* ---------------------------------- */

#define MAX_GAMEPAD_NAME        48
#define MAX_BONDED_DEVICES      4
#define MAX_GAMEPADS            2
#define SCAN_INQUIRY_UNITS      8
#define SCAN_WAIT_MS            15000
#define RETRY_DELAY_MS          1800
#define CONNECT_TIMEOUT_MS      20000

#define BACKGROUND_PAGE_INTERVAL_MS 30000
#define STABLE_BEFORE_PAGING_MS 15000
#define IDLE_PAGE_INTERVAL_MS   8000
#define FAILED_PAGE_BACKOFF_MS  15000
#define BONDED_RETRY_DELAY_MS   500

#define SEND_DEFAULT_LIGHTBAR   1
/* Если геймпады плохо переподключаются сами, попробуйте поставить здесь 0 */
#define ACTIVE_RECONNECT        0
/* Если хотите, чтобы ESP32 всегда ждала подключения от геймпада, поставьте 0 */
#define STAY_CONNECTABLE        1
#define PAGE_ALL_BONDS_AT_ONCE  0

static const uint8_t FRAME_MAGIC[] = {0xA5, 0x5A, 0xA5, 0x5A};
static const uint8_t PROTOCOL_VERSION = 2;
static const size_t FRAME_HEADER_SIZE = 10;

enum {
    FRAME_HELLO = 0x01,
    FRAME_STATUS = 0x02,
    FRAME_ACK = 0x03,
    FRAME_HID_INPUT = 0x10,
    CMD_PAIR_MODE = 0x20,
    CMD_HID_OUTPUT = 0x21,
    FRAME_LOG = 0x30,
};

enum {
    LINK_SCANNING = 1,
    LINK_CONNECTING = 2,
    LINK_CONNECTED = 3,
    LINK_PAIRING = 4,
    LINK_DISCONNECTED = 5,
};

static QueueHandle_t      s_tx_queue;
static SemaphoreHandle_t  s_state_mutex;
static SemaphoreHandle_t  s_scan_stopped;

#define STATE_LOCK()   do { if (s_state_mutex) (void)xSemaphoreTake(s_state_mutex, portMAX_DELAY); } while (0)
#define STATE_UNLOCK() do { if (s_state_mutex) (void)xSemaphoreGive(s_state_mutex); } while (0)

typedef struct {
    esp_hidh_dev_t *device;
    esp_bd_addr_t address;
    char name[MAX_GAMEPAD_NAME + 1];
    bool connecting;
    TickType_t connect_started;
} controller_slot_t;

static controller_slot_t s_slots[MAX_GAMEPADS];
static bool s_scanning;
static bool s_incoming_pending;
static esp_bd_addr_t s_incoming_addr;
static uint32_t s_dropped_inputs;
static uint32_t s_reported_drops;

/* Timing statistics, printed as one FRAME_LOG line every STATS_INTERVAL_MS. */
#define STATS_INTERVAL_MS       2000
/* Gaps longer than this are treated as a reconnect, not as a stall. */
#define STATS_IGNORE_GAP_MS     3000
typedef struct {
    int64_t last_us;       /* arrival time of the previous report of this pad */
    uint32_t max_gap_ms;   /* longest pause between two reports in the window */
    uint32_t count;        /* reports received in the window */
} input_stats_t;
static input_stats_t s_input_stats[MAX_GAMEPADS];
static uint32_t s_tx_max_block_ms;   /* longest uart_write_bytes() in the window */
static UBaseType_t s_tx_queue_peak;  /* highest TX queue fill in the window */
static bool s_candidate_found;
static bool s_pair_mode;
static esp_bd_addr_t s_candidate_address;

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void write_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFF);
    data[1] = (uint8_t)(value >> 8);
}

static uint16_t crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t crc32_ds4_update(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
    }
    return crc;
}

static void write_u32_le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void set_ds4_default_lightbar(esp_hidh_dev_t *device, uint8_t map_index,
                                     uint8_t slot)
{
    if (device == NULL || slot >= MAX_GAMEPADS) return;
    uint8_t report[77] = {0};
    report[0] = 0xC0;
    report[2] = 0x07;
    report[7] = 0;
    report[8] = slot == 0 ? 255 : 96;
    report[9] = slot == 0 ? 0 : 255;
    uint32_t crc = 0xFFFFFFFFU;
    crc = crc32_ds4_update(crc, 0xA2);
    crc = crc32_ds4_update(crc, 0x11);
    for (size_t i = 0; i < 73; ++i) crc = crc32_ds4_update(crc, report[i]);
    write_u32_le(&report[73], ~crc);
    (void)esp_hidh_dev_output_set(device, map_index, 0x11, report, sizeof(report));
}

static esp_err_t send_frame(uint8_t type, uint16_t sequence,
                            const uint8_t *payload, size_t payload_length)
{
    if (payload_length > MAX_TX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    if (s_tx_queue == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t frame[TX_QUEUE_ITEM_SIZE];
    memcpy(frame, FRAME_MAGIC, sizeof(FRAME_MAGIC));
    frame[4] = PROTOCOL_VERSION;
    frame[5] = type;
    write_u16_le(&frame[6], sequence);
    write_u16_le(&frame[8], (uint16_t)payload_length);
    if (payload_length > 0 && payload != NULL)
        memcpy(&frame[FRAME_HEADER_SIZE], payload, payload_length);
    const uint16_t crc = crc16_ccitt_false(&frame[4], 6 + payload_length);
    write_u16_le(&frame[FRAME_HEADER_SIZE + payload_length], crc);
    if (xQueueSend(s_tx_queue, frame, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    const UBaseType_t used = TX_QUEUE_DEPTH - uxQueueSpacesAvailable(s_tx_queue);
    if (used > s_tx_queue_peak) s_tx_queue_peak = used;
    return ESP_OK;
}

static void send_log(const char *format, ...)
{
    char text[160];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length <= 0) return;
    const size_t size = (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1;
    (void)send_frame(FRAME_LOG, 0, (const uint8_t *)text, size);
}

static int forward_esp_logs(const char *format, va_list args)
{
    if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        s_tx_queue == NULL ||
        uxQueueSpacesAvailable(s_tx_queue) < TX_QUEUE_DEPTH / 2) {
        char tmp[1];
        return vsnprintf(tmp, sizeof(tmp), format, args);
    }
    char raw[200];
    int length = vsnprintf(raw, sizeof(raw), format, args);
    if (length <= 0) return length;
    if ((size_t)length >= sizeof(raw)) length = (int)sizeof(raw) - 1;
    char text[200];
    size_t out = 0;
    for (int i = 0; i < length && out < sizeof(text) - 1; ++i) {
        const unsigned char c = (unsigned char)raw[i];
        if (c == 0x1B) { while (i < length && raw[i] != 'm') ++i; continue; }
        if (c == '\r' || c == '\n') continue;
        text[out++] = (char)c;
    }
    if (out == 0) return length;
    (void)send_frame(FRAME_LOG, 0, (const uint8_t *)text, out);
    return length;
}

static int get_bonded_devices(esp_bd_addr_t *addresses, int capacity)
{
    int count = esp_bt_gap_get_bond_device_num();
    if (count <= 0 || capacity <= 0) return 0;
    if (count > capacity) count = capacity;
    int output_count = count;
    if (esp_bt_gap_get_bond_device_list(&output_count, addresses) != ESP_OK) return 0;
    return output_count;
}

static void send_status(uint8_t state)
{
    uint8_t payload[3 + MAX_GAMEPADS * (9 + MAX_GAMEPAD_NAME)] = {0};
    payload[0] = state;
    esp_bd_addr_t bonds[MAX_BONDED_DEVICES];
    payload[1] = (uint8_t)get_bonded_devices(bonds, MAX_BONDED_DEVICES);
    size_t offset = 3;
    payload[2] = MAX_GAMEPADS;
    STATE_LOCK();
    for (uint8_t i = 0; i < MAX_GAMEPADS; ++i) {
        payload[offset++] = i;
        payload[offset++] = s_slots[i].device != NULL ? 1 : 0;
        memcpy(&payload[offset], s_slots[i].address, sizeof(esp_bd_addr_t));
        offset += sizeof(esp_bd_addr_t);
        const size_t name_length = strnlen(s_slots[i].name, MAX_GAMEPAD_NAME);
        payload[offset++] = (uint8_t)name_length;
        if (name_length > 0) {
            memcpy(&payload[offset], s_slots[i].name, name_length);
            offset += name_length;
        }
    }
    STATE_UNLOCK();
    (void)send_frame(FRAME_STATUS, 0, payload, offset);
}

static bool contains_case_insensitive(const uint8_t *text, size_t text_length,
                                      const char *needle)
{
    const size_t needle_length = strlen(needle);
    if (text == NULL || needle_length == 0 || text_length < needle_length) return false;
    for (size_t i = 0; i <= text_length - needle_length; ++i) {
        size_t j = 0;
        for (; j < needle_length; ++j) {
            uint8_t left = text[i + j];
            uint8_t right = (uint8_t)needle[j];
            if (left >= 'A' && left <= 'Z') left = (uint8_t)(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z') right = (uint8_t)(right - 'A' + 'a');
            if (left != right) break;
        }
        if (j == needle_length) return true;
    }
    return false;
}

static bool is_gamepad_name(const uint8_t *name, size_t length)
{
    static const char *const names[] = {
        "wireless controller", "controller", "dualshock", "dualsense", "xbox",
        "8bitdo", "dexp", "g13", "gamepad", "joystick",
        "pro controller", "switch controller"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (contains_case_insensitive(name, length, names[i])) return true;
    return false;
}

static void consider_discovered_device(esp_bt_gap_cb_param_t *param)
{
    const uint8_t *name = NULL;
    size_t name_length = 0;
    uint32_t cod = 0;
    bool has_cod = false;
    for (int i = 0; i < param->disc_res.num_prop; ++i) {
        esp_bt_gap_dev_prop_t *property = &param->disc_res.prop[i];
        if (property->type == ESP_BT_GAP_DEV_PROP_BDNAME && property->val != NULL) {
            name = (const uint8_t *)property->val;
            name_length = property->len;
            while (name_length > 0 && name[name_length - 1] == '\0') --name_length;
        } else if (property->type == ESP_BT_GAP_DEV_PROP_COD && property->val != NULL) {
            memcpy(&cod, property->val, sizeof(cod));
            has_cod = true;
        } else if (property->type == ESP_BT_GAP_DEV_PROP_EIR && property->val != NULL) {
            uint8_t eir_name_length = 0;
            uint8_t *eir_name = esp_bt_gap_resolve_eir_data(
                (uint8_t *)property->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,
                &eir_name_length);
            if (eir_name == NULL) {
                eir_name = esp_bt_gap_resolve_eir_data(
                    (uint8_t *)property->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME,
                    &eir_name_length);
            }
            if (eir_name != NULL && eir_name_length > 0) {
                name = eir_name;
                name_length = eir_name_length;
            }
        }
    }
    const bool cod_gamepad = has_cod && ((cod >> 8) & 0x1F) == 0x05 &&
                             (((cod >> 2) & 0x0F) == 0x01 || ((cod >> 2) & 0x0F) == 0x02);
    const bool name_gamepad = is_gamepad_name(name, name_length);
    send_log("inquiry %02x:%02x:%02x:%02x:%02x:%02x name='%.*s' cod=0x%06x gamepad=%d",
             param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
             param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5],
             (int)name_length, name != NULL ? (const char *)name : "",
             (unsigned)cod, (cod_gamepad || name_gamepad) ? 1 : 0);
    if (!cod_gamepad && !name_gamepad) return;
    STATE_LOCK();
    bool already_known = false;
    for (size_t i = 0; i < MAX_GAMEPADS; ++i) {
        if ((s_slots[i].device != NULL || s_slots[i].connecting) &&
            memcmp(s_slots[i].address, param->disc_res.bda, sizeof(esp_bd_addr_t)) == 0) {
            already_known = true;
            break;
        }
    }
    if (s_scanning && !s_candidate_found && !already_known) {
        memcpy(s_candidate_address, param->disc_res.bda, sizeof(s_candidate_address));
        s_candidate_found = true;
    }
    STATE_UNLOCK();
}

static bool is_pairing_candidate(const esp_bd_addr_t address)
{
    bool matches;
    STATE_LOCK();
    matches = s_candidate_found &&
              memcmp(s_candidate_address, address, sizeof(s_candidate_address)) == 0;
    STATE_UNLOCK();
    return matches;
}

static int find_slot_by_address_locked(const uint8_t *address)
{
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if ((s_slots[i].device != NULL || s_slots[i].connecting) &&
            memcmp(s_slots[i].address, address, sizeof(esp_bd_addr_t)) == 0) return i;
    }
    return -1;
}

static int find_slot_by_device_locked(esp_hidh_dev_t *device)
{
    if (device == NULL) return -1;
    for (int i = 0; i < MAX_GAMEPADS; ++i)
        if (s_slots[i].device == device) return i;
    return -1;
}

static int reserve_slot(const esp_bd_addr_t address)
{
    int slot = -1;
    STATE_LOCK();
    if (find_slot_by_address_locked(address) < 0) {
        for (int i = 0; i < MAX_GAMEPADS; ++i) {
            if (s_slots[i].device == NULL && !s_slots[i].connecting) {
                slot = i;
                memcpy(s_slots[i].address, address, sizeof(esp_bd_addr_t));
                s_slots[i].connecting = true;
                s_slots[i].connect_started = xTaskGetTickCount();
                s_slots[i].name[0] = '\0';
                break;
            }
        }
    }
    STATE_UNLOCK();
    return slot;
}

static void release_slot_reservation(int slot)
{
    if (slot < 0 || slot >= MAX_GAMEPADS) return;
    STATE_LOCK();
    if (s_slots[slot].device == NULL) memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
    STATE_UNLOCK();
}

static int occupied_slot_count(void)
{
    int count = 0;
    STATE_LOCK();
    for (int i = 0; i < MAX_GAMEPADS; ++i)
        if (s_slots[i].device != NULL || s_slots[i].connecting) ++count;
    STATE_UNLOCK();
    return count;
}

static int connected_slot_count(void)
{
    int count = 0;
    STATE_LOCK();
    for (int i = 0; i < MAX_GAMEPADS; ++i)
        if (s_slots[i].device != NULL) ++count;
    STATE_UNLOCK();
    return count;
}

static bool connected_stable(uint32_t min_ms)
{
    const TickType_t now = xTaskGetTickCount();
    bool stable = true;
    STATE_LOCK();
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if (s_slots[i].device != NULL &&
            (now - s_slots[i].connect_started) < pdMS_TO_TICKS(min_ms)) stable = false;
    }
    STATE_UNLOCK();
    return stable;
}

static bool has_pending_connection(void)
{
    bool pending = false;
    STATE_LOCK();
    for (int i = 0; i < MAX_GAMEPADS; ++i)
        if (s_slots[i].connecting) { pending = true; break; }
    STATE_UNLOCK();
    return pending;
}

static void expire_stale_connections(void)
{
    const TickType_t now = xTaskGetTickCount();
    bool expired = false;
    STATE_LOCK();
    for (int i = 0; i < MAX_GAMEPADS; ++i) {
        if (s_slots[i].connecting && s_slots[i].device == NULL &&
            (now - s_slots[i].connect_started) > pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            expired = true;
        }
    }
    STATE_UNLOCK();
    if (expired) send_log("connect timeout: reservation dropped");
}

static void forget_unconnected_bonds(void)
{
    esp_bd_addr_t bonds[MAX_BONDED_DEVICES];
    const int count = get_bonded_devices(bonds, MAX_BONDED_DEVICES);
    for (int i = 0; i < count; ++i) {
        bool in_use;
        STATE_LOCK();
        in_use = find_slot_by_address_locked(bonds[i]) >= 0;
        STATE_UNLOCK();
        if (!in_use) {
            (void)esp_bt_gap_remove_bond_device(bonds[i]);
            send_log("forgot stale bond %02x:%02x:%02x:%02x:%02x:%02x",
                     bonds[i][0], bonds[i][1], bonds[i][2],
                     bonds[i][3], bonds[i][4], bonds[i][5]);
        }
    }
}

static void bluetooth_gap_callback(esp_bt_gap_cb_event_t event,
                                   esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        consider_discovered_device(param);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            STATE_LOCK();
            s_scanning = false;
            STATE_UNLOCK();
            if (s_scan_stopped != NULL) xSemaphoreGive(s_scan_stopped);
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        send_log("auth %02x:%02x:%02x:%02x:%02x:%02x stat=%d",
                 param->auth_cmpl.bda[0], param->auth_cmpl.bda[1],
                 param->auth_cmpl.bda[2], param->auth_cmpl.bda[3],
                 param->auth_cmpl.bda[4], param->auth_cmpl.bda[5],
                 (int)param->auth_cmpl.stat);
        break;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT: {
        const unsigned hci = (unsigned)(param->acl_conn_cmpl_stat.stat & 0xFF);
        const char *hint = "";
        if (hci == 0x04) hint = " (Page Timeout)";
        else if (hci == 0x08) hint = " (Connection Timeout)";
        else if (hci == 0x13) hint = " (Remote User Terminated)";
        else if (hci == 0x16) hint = " (Local Host Terminated)";
        send_log("acl connect %02x:%02x:%02x:%02x:%02x:%02x stat=0x%x%s",
                 param->acl_conn_cmpl_stat.bda[0], param->acl_conn_cmpl_stat.bda[1],
                 param->acl_conn_cmpl_stat.bda[2], param->acl_conn_cmpl_stat.bda[3],
                 param->acl_conn_cmpl_stat.bda[4], param->acl_conn_cmpl_stat.bda[5],
                 (unsigned)param->acl_conn_cmpl_stat.stat, hint);
        if (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) {
            STATE_LOCK();
            if (find_slot_by_address_locked(param->acl_conn_cmpl_stat.bda) < 0) {
                memcpy(s_incoming_addr, param->acl_conn_cmpl_stat.bda,
                       sizeof(s_incoming_addr));
                s_incoming_pending = true;
            }
            STATE_UNLOCK();
        }
        break;
    }
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        send_log("acl disconnect %02x:%02x:%02x:%02x:%02x:%02x reason=0x%x (hci 0x%02x)",
                 param->acl_disconn_cmpl_stat.bda[0], param->acl_disconn_cmpl_stat.bda[1],
                 param->acl_disconn_cmpl_stat.bda[2], param->acl_disconn_cmpl_stat.bda[3],
                 param->acl_disconn_cmpl_stat.bda[4], param->acl_disconn_cmpl_stat.bda[5],
                 (unsigned)param->acl_disconn_cmpl_stat.reason,
                 (unsigned)(param->acl_disconn_cmpl_stat.reason & 0xFF));
        STATE_LOCK();
        int disconn_slot = find_slot_by_address_locked(param->acl_disconn_cmpl_stat.bda);
        if (disconn_slot >= 0 && s_slots[disconn_slot].connecting &&
            s_slots[disconn_slot].device == NULL) {
            memset(&s_slots[disconn_slot], 0, sizeof(s_slots[disconn_slot]));
            send_log("cleared stuck connecting slot %d after ACL disconnect", disconn_slot);
        }
        STATE_UNLOCK();
        break;
#endif
    case ESP_BT_GAP_CFM_REQ_EVT:
        (void)esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda,
                                            is_pairing_candidate(param->cfm_req.bda));
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
        (void)esp_bt_gap_pin_reply(param->pin_req.bda,
                                   is_pairing_candidate(param->pin_req.bda),
                                   sizeof(pin_code), pin_code);
        break;
    }
    default:
        break;
    }
}

static void hid_host_callback(void *handler_args, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_base;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch ((esp_hidh_event_t)event_id) {
    case ESP_HIDH_OPEN_EVENT: {
        const bool connected = param->open.status == ESP_OK && param->open.dev != NULL;
        esp_bd_addr_t address = {0};
        const uint8_t *device_address = param->open.dev != NULL
                                            ? esp_hidh_dev_bda_get(param->open.dev) : NULL;
        if (device_address != NULL) memcpy(address, device_address, sizeof(address));
        const char *device_name = connected ? esp_hidh_dev_name_get(param->open.dev) : NULL;
        STATE_LOCK();
        int slot = find_slot_by_address_locked(address);
        if (slot < 0 && !connected && param->open.dev == NULL) {
            for (int i = 0; i < MAX_GAMEPADS; ++i)
                if (s_slots[i].connecting) { slot = i; break; }
        }
        if (slot < 0 && connected) {
            for (int i = 0; i < MAX_GAMEPADS; ++i) {
                if (s_slots[i].device == NULL && !s_slots[i].connecting) {
                    slot = i;
                    memcpy(s_slots[i].address, address, sizeof(address));
                    break;
                }
            }
        }
        if (slot >= 0) {
            s_slots[slot].connecting = false;
            s_slots[slot].device = connected ? param->open.dev : NULL;
            s_slots[slot].connect_started = xTaskGetTickCount();
            s_slots[slot].name[0] = '\0';
            if (connected && device_name != NULL) {
                (void)snprintf(s_slots[slot].name, sizeof(s_slots[slot].name),
                               "%s", device_name);
            }
            if (!connected) memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
        }
        STATE_UNLOCK();
        send_log("hid open %02x:%02x:%02x:%02x:%02x:%02x status=%d slot=%d",
                 address[0], address[1], address[2], address[3], address[4], address[5],
                 (int)param->open.status, slot);
        if (connected) {
            esp_err_t err = esp_hidh_dev_get_report(param->open.dev, 0, 0x02,
                                                    ESP_HID_REPORT_TYPE_FEATURE, 37);
            if (err != ESP_OK) send_log("feature GET 0x02 failed: %d", (int)err);
        }
        send_status(connected ? LINK_CONNECTED : LINK_DISCONNECTED);
        break;
    }
    case ESP_HIDH_FEATURE_EVENT:
        if (param->feature.report_id == 0x02 && param->feature.status == ESP_OK &&
            param->feature.dev != NULL) {
            int slot;
            STATE_LOCK();
            slot = find_slot_by_device_locked(param->feature.dev);
            STATE_UNLOCK();
#if SEND_DEFAULT_LIGHTBAR
            if (slot >= 0) set_ds4_default_lightbar(param->feature.dev,
                                                    param->feature.map_index,
                                                    (uint8_t)slot);
#else
            (void)slot;
#endif
        }
        break;
    case ESP_HIDH_INPUT_EVENT: {
        const int64_t now_us = esp_timer_get_time();
        int slot;
        STATE_LOCK();
        slot = find_slot_by_device_locked(param->input.dev);
        STATE_UNLOCK();
        if (slot < 0) break;

        /* Per-pad inter-arrival time: shows whether the Bluetooth link itself
           pauses (pad -> ESP32), independent of the UART/PC side. */
        input_stats_t *stat = &s_input_stats[slot];
        if (stat->last_us != 0) {
            const uint32_t gap_ms = (uint32_t)((now_us - stat->last_us) / 1000);
            if (gap_ms < STATS_IGNORE_GAP_MS && gap_ms > stat->max_gap_ms) {
                stat->max_gap_ms = gap_ms;
            }
        }
        stat->last_us = now_us;
        ++stat->count;

        if (param->input.data != NULL && param->input.length > 0 &&
            param->input.length <= MAX_TX_PAYLOAD - 6) {
            uint8_t payload[MAX_TX_PAYLOAD];
            payload[0] = (uint8_t)slot;
            write_u16_le(&payload[1], param->input.report_id);
            payload[3] = param->input.map_index;
            write_u16_le(&payload[4], (uint16_t)param->input.usage);
            memcpy(&payload[6], param->input.data, param->input.length);
            if (send_frame(FRAME_HID_INPUT, 0, payload, 6 + param->input.length) != ESP_OK) {
                ++s_dropped_inputs; /* reported in the periodic stats line */
            }
        }
        break;
    }
    case ESP_HIDH_CLOSE_EVENT: {
        esp_hidh_dev_t *closed_device = param->close.dev;
        {
            const uint8_t *closed_bda = closed_device != NULL
                                            ? esp_hidh_dev_bda_get(closed_device) : NULL;
            send_log("hid close status=%d %02x:%02x:%02x:%02x:%02x:%02x",
                     (int)param->close.status,
                     closed_bda ? closed_bda[0] : 0, closed_bda ? closed_bda[1] : 0,
                     closed_bda ? closed_bda[2] : 0, closed_bda ? closed_bda[3] : 0,
                     closed_bda ? closed_bda[4] : 0, closed_bda ? closed_bda[5] : 0);
        }
        STATE_LOCK();
        int slot = find_slot_by_device_locked(closed_device);
        if (slot >= 0) memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
        STATE_UNLOCK();
        if (closed_device != NULL) (void)esp_hidh_dev_free(closed_device);
        send_status(LINK_DISCONNECTED);
        break;
    }
    default:
        break;
    }
}

static void send_ack(uint8_t command, uint16_t sequence, uint8_t result)
{
    const uint8_t payload[] = {command, result};
    (void)send_frame(FRAME_ACK, sequence, payload, sizeof(payload));
}

static void handle_command(uint8_t type, uint16_t sequence,
                           const uint8_t *payload, size_t length)
{
    if (type == FRAME_HELLO && length == 0) {
        send_log("firmware build %s %s (v11: ACL_DISCONN clears stuck slot)",
                 __DATE__, __TIME__);
        send_status(LINK_DISCONNECTED);
        send_ack(type, sequence, 0);
        return;
    }
    if (type == CMD_PAIR_MODE && length == 1) {
        const bool enabled = payload[0] != 0;
        STATE_LOCK();
        s_pair_mode = enabled;
        STATE_UNLOCK();
        if (enabled) forget_unconnected_bonds();
        const esp_err_t result = esp_bt_gap_set_scan_mode(
            (enabled || STAY_CONNECTABLE) ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
            enabled ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
        send_ack(type, sequence, result == ESP_OK ? 0 : 1);
        send_status(enabled ? LINK_PAIRING : LINK_SCANNING);
        return;
    }
    if (type == CMD_HID_OUTPUT && length >= 6) {
        const uint8_t slot = payload[0];
        const uint8_t map_index = payload[1];
        const uint16_t report_id = read_u16_le(&payload[2]);
        const uint16_t data_length = read_u16_le(&payload[4]);
        esp_hidh_dev_t *device;
        STATE_LOCK();
        device = slot < MAX_GAMEPADS ? s_slots[slot].device : NULL;
        STATE_UNLOCK();
        if (data_length != length - 6 || device == NULL) {
            send_ack(type, sequence, 1);
            return;
        }
        const esp_err_t result = esp_hidh_dev_output_set(
            device, map_index, report_id, (uint8_t *)&payload[6], data_length);
        send_ack(type, sequence, result == ESP_OK ? 0 : 1);
        return;
    }
    send_ack(type, sequence, 2);
}

static void process_received_bytes(const uint8_t *incoming, size_t incoming_length)
{
    static uint8_t rx_buffer[2 * (FRAME_OVERHEAD + MAX_FRAME_PAYLOAD)];
    static size_t buffered = 0;
    if (incoming_length > sizeof(rx_buffer) - buffered) buffered = 0;
    memcpy(&rx_buffer[buffered], incoming, incoming_length);
    buffered += incoming_length;
    for (;;) {
        size_t marker = 0;
        while (marker + sizeof(FRAME_MAGIC) <= buffered &&
               memcmp(&rx_buffer[marker], FRAME_MAGIC, sizeof(FRAME_MAGIC)) != 0) ++marker;
        if (marker > 0) {
            memmove(rx_buffer, &rx_buffer[marker], buffered - marker);
            buffered -= marker;
        }
        if (buffered < FRAME_HEADER_SIZE) return;
        const uint16_t payload_length = read_u16_le(&rx_buffer[8]);
        if (rx_buffer[4] != PROTOCOL_VERSION || payload_length > MAX_FRAME_PAYLOAD) {
            memmove(rx_buffer, &rx_buffer[1], --buffered);
            continue;
        }
        const size_t frame_length = FRAME_OVERHEAD + payload_length;
        if (buffered < frame_length) return;
        const uint16_t expected_crc =
            read_u16_le(&rx_buffer[FRAME_HEADER_SIZE + payload_length]);
        const uint16_t actual_crc = crc16_ccitt_false(&rx_buffer[4], 6 + payload_length);
        if (expected_crc == actual_crc) {
            handle_command(rx_buffer[5], read_u16_le(&rx_buffer[6]),
                           &rx_buffer[FRAME_HEADER_SIZE], payload_length);
            memmove(rx_buffer, &rx_buffer[frame_length], buffered - frame_length);
            buffered -= frame_length;
        } else {
            memmove(rx_buffer, &rx_buffer[1], --buffered);
        }
    }
}

static void uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t buffer[256];
    for (;;) {
        const int received = uart_read_bytes(BRIDGE_UART, buffer, sizeof(buffer),
                                             pdMS_TO_TICKS(100));
        if (received > 0) process_received_bytes(buffer, (size_t)received);
    }
}

static void uart_tx_task(void *arg)
{
    (void)arg;
    uint8_t frame[TX_QUEUE_ITEM_SIZE];
    for (;;) {
        if (xQueueReceive(s_tx_queue, frame, portMAX_DELAY) != pdTRUE) continue;
        const uint16_t payload_length = read_u16_le(&frame[8]);
        const size_t frame_length = FRAME_OVERHEAD + payload_length;

        TickType_t start = xTaskGetTickCount();
        (void)uart_write_bytes(BRIDGE_UART, (const char *)frame, frame_length);
        TickType_t elapsed = xTaskGetTickCount() - start;

        const uint32_t elapsed_ms = (uint32_t)(elapsed * portTICK_PERIOD_MS);
        if (elapsed_ms > s_tx_max_block_ms) s_tx_max_block_ms = elapsed_ms;
    }
}

static bool is_bonded_address(const esp_bd_addr_t address)
{
    esp_bd_addr_t bonds[MAX_BONDED_DEVICES];
    const int count = get_bonded_devices(bonds, MAX_BONDED_DEVICES);
    for (int i = 0; i < count; ++i)
        if (memcmp(bonds[i], address, sizeof(esp_bd_addr_t)) == 0) return true;
    return false;
}

static bool take_incoming(esp_bd_addr_t out)
{
    bool pending;
    STATE_LOCK();
    pending = s_incoming_pending;
    if (pending) {
        memcpy(out, s_incoming_addr, sizeof(esp_bd_addr_t));
        s_incoming_pending = false;
    }
    STATE_UNLOCK();
    return pending;
}

static void wait_or_incoming(uint32_t milliseconds)
{
    for (uint32_t waited = 0; waited < milliseconds; waited += 100) {
        bool pending;
        STATE_LOCK();
        pending = s_incoming_pending;
        STATE_UNLOCK();
        if (pending) return;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void scan_and_connect_task(void *arg)
{
    (void)arg;
    static int consecutive_page_failures = 0;
    static int next_bond = 0;
    send_log("!!! bg_task: trying to page bonds");
    static TickType_t last_background_page = 0;

    for (;;) {
        expire_stale_connections();
        if (occupied_slot_count() >= MAX_GAMEPADS || has_pending_connection()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        esp_bd_addr_t incoming;
        if (take_incoming(incoming)) {
            if (is_bonded_address(incoming)) {
                const int slot = reserve_slot(incoming);
                if (slot >= 0) {
                    send_log("incoming link from %02x:%02x:%02x:%02x:%02x:%02x, "
                             "opening HID (NB: esp_hidh may internally page)",
                             incoming[0], incoming[1], incoming[2],
                             incoming[3], incoming[4], incoming[5]);
                    send_status(LINK_CONNECTING);
                    esp_hidh_dev_t *dev = esp_hidh_dev_open(incoming,
                                                            ESP_HID_TRANSPORT_BT, 0);
                    if (dev == NULL) {
                        send_log("esp_hidh_dev_open returned NULL for incoming link");
                        release_slot_reservation(slot);
                    } else {
                        send_log("incoming HID open queued, waiting up to 10s");
                        for (int wait = 0; wait < 40 && has_pending_connection(); ++wait) {
                            vTaskDelay(pdMS_TO_TICKS(250));
                        }
                        if (has_pending_connection()) {
                            send_log("incoming HID open still pending after 10s");
                        }
                    }
                }
            }
            continue;
        }

        esp_bd_addr_t bonded[MAX_BONDED_DEVICES];
        const int bond_count = get_bonded_devices(bonded, MAX_BONDED_DEVICES);
        bool opened_bonded_device = false;
        bool pairing_now;
        STATE_LOCK();
        pairing_now = s_pair_mode;
        STATE_UNLOCK();

        const bool someone_connected = connected_slot_count() > 0;
        const bool page_allowed =
            !someone_connected ||
            (BACKGROUND_PAGE_INTERVAL_MS > 0 &&
             connected_stable(STABLE_BEFORE_PAGING_MS) &&
             (xTaskGetTickCount() - last_background_page) >
                 pdMS_TO_TICKS(BACKGROUND_PAGE_INTERVAL_MS));

        if (ACTIVE_RECONNECT && bond_count > 0 && !pairing_now && page_allowed) {
            last_background_page = xTaskGetTickCount();
            for (int tries = 0; tries < bond_count; ++tries) {
                const int i = (next_bond + tries) % bond_count;
                const int slot = reserve_slot(bonded[i]);
                if (slot < 0) continue;
                send_log("reconnecting to bonded device %d", i);
                send_status(LINK_CONNECTING);
                if (esp_hidh_dev_open(bonded[i], ESP_HID_TRANSPORT_BT, 0) == NULL) {
                    send_log("esp_hidh_dev_open returned NULL for bond %d", i);
                    release_slot_reservation(slot);
                    continue;
                }
                opened_bonded_device = true;
                next_bond = (i + 1) % bond_count;
                if (!PAGE_ALL_BONDS_AT_ONCE) break;
            }
            for (int wait = 0; wait < 48 && has_pending_connection(); ++wait)
                vTaskDelay(pdMS_TO_TICKS(250));
            if (opened_bonded_device) {
                if (connected_slot_count() > 0) {
                    consecutive_page_failures = 0;
                } else {
                    ++consecutive_page_failures;
                    send_log("page attempt %u failed (no pad answered)",
                             (unsigned)consecutive_page_failures);
                }
            }

            /* Даём стеку BT время полностью завершить SDP-процедуру
               (или упасть по таймауту) перед следующей попыткой. */
            uint32_t pause_ms = BONDED_RETRY_DELAY_MS;
            if (connected_slot_count() == 0) {
                pause_ms = 15000; // 15 секунд, если никого нет
                if (consecutive_page_failures >= 2) {
                    pause_ms = 25000; // 25 секунд, если уже были неудачи
                }
            }

            if (opened_bonded_device) {
                wait_or_incoming(pause_ms);
            } else {
                vTaskDelay(pdMS_TO_TICKS(pause_ms));
            }
            continue;
        }

        bool pairing;
        STATE_LOCK();
        pairing = s_pair_mode;
        s_candidate_found = false;
        s_scanning = pairing;
        STATE_UNLOCK();

        if (!pairing || occupied_slot_count() >= MAX_GAMEPADS) {
            send_status(connected_slot_count() > 0 ? LINK_CONNECTED : LINK_DISCONNECTED);
            wait_or_incoming(RETRY_DELAY_MS);
            continue;
        }

        while (xSemaphoreTake(s_scan_stopped, 0) == pdTRUE) {}
        send_status(LINK_PAIRING);
        send_log("!!! bg_task: starting inquiry (SCAN)");
        if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                       SCAN_INQUIRY_UNITS, 0) != ESP_OK) {
            STATE_LOCK();
            s_scanning = false;
            STATE_UNLOCK();
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }
        send_log("inquiry started");
        if (xSemaphoreTake(s_scan_stopped, pdMS_TO_TICKS(SCAN_WAIT_MS)) != pdTRUE) {
            (void)esp_bt_gap_cancel_discovery();
            (void)xSemaphoreTake(s_scan_stopped, pdMS_TO_TICKS(1000));
        }
        esp_bd_addr_t candidate;
        bool found;
        STATE_LOCK();
        found = s_candidate_found;
        if (found) memcpy(candidate, s_candidate_address, sizeof(candidate));
        STATE_UNLOCK();
        if (found) {
            const int slot = reserve_slot(candidate);
            if (slot >= 0) {
                send_status(LINK_CONNECTING);
                if (esp_hidh_dev_open(candidate, ESP_HID_TRANSPORT_BT, 0) == NULL)
                    release_slot_reservation(slot);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
}

static void initialize_uart(void)
{
    const uart_config_t config = {
        .baud_rate = BRIDGE_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART, UART_RX_BUFFER_SIZE,
                                        UART_TX_BUFFER_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART, &config));
    s_state_mutex = xSemaphoreCreateMutex();
    s_scan_stopped = xSemaphoreCreateBinary();
    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, TX_QUEUE_ITEM_SIZE);
    ESP_ERROR_CHECK(s_state_mutex != NULL && s_scan_stopped != NULL &&
                    s_tx_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    esp_log_set_vprintf(forward_esp_logs);
}

static void initialize_bluetooth(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&controller_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bluetooth_gap_callback));
    esp_bt_sp_param_t security_parameter = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t io_capability = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(
        security_parameter, &io_capability, sizeof(io_capability)));
    esp_bt_pin_code_t legacy_pin = {'0', '0', '0', '0'};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, sizeof(legacy_pin),
                                      legacy_pin));
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(
        STAY_CONNECTABLE ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
        ESP_BT_NON_DISCOVERABLE));
    const esp_hidh_config_t hid_config = {
        .callback = hid_host_callback,
        .event_stack_size = 8192,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&hid_config));
    s_pair_mode = esp_bt_gap_get_bond_device_num() == 0;
}

static void stats_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_INTERVAL_MS));
        uint32_t count[MAX_GAMEPADS];
        uint32_t gap[MAX_GAMEPADS];
        for (int i = 0; i < MAX_GAMEPADS; ++i) {
            count[i] = s_input_stats[i].count;
            gap[i] = s_input_stats[i].max_gap_ms;
            s_input_stats[i].count = 0;
            s_input_stats[i].max_gap_ms = 0;
        }
        const uint32_t drops = s_dropped_inputs - s_reported_drops;
        s_reported_drops = s_dropped_inputs;
        const uint32_t block_ms = s_tx_max_block_ms;
        s_tx_max_block_ms = 0;
        const UBaseType_t peak = s_tx_queue_peak;
        s_tx_queue_peak = 0;
        if (count[0] == 0 && count[1] == 0) continue; /* nothing connected: stay quiet */
        send_log("ESP/2s: s0 n=%u gap=%ums | s1 n=%u gap=%ums | drop=%u txq=%u/%d uart_block=%ums",
                 (unsigned)count[0], (unsigned)gap[0], (unsigned)count[1], (unsigned)gap[1],
                 (unsigned)drops, (unsigned)peak, (int)TX_QUEUE_DEPTH, (unsigned)block_ms);
    }
}

void app_main(void)
{
    initialize_uart();
    initialize_bluetooth();
    ESP_ERROR_CHECK(xTaskCreate(uart_tx_task, "bridge_uart_tx", 3072, NULL, 7, NULL)
                        == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(uart_rx_task, "bridge_uart_rx", 4096, NULL, 8, NULL)
                        == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(stats_task, "bridge_stats", 3072, NULL, 3, NULL)
                        == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(scan_and_connect_task, "hid_reconnect", 6144,
                                NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    send_status(s_pair_mode ? LINK_PAIRING : LINK_SCANNING);
}