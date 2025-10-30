#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_BT)
#include <zephyr/bluetooth/gatt.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/split/transport/types.h>
#include <zmk/split/transport/central.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/hid.h>
#endif

#include "dongle_screen/latency/metrics.h"

/* Queues we want to track */
extern struct k_msgq physical_layouts_kscan_msgq;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
extern struct k_msgq position_state_msgq;
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
extern struct k_msgq peripheral_event_msgq;
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
extern struct k_msgq zmk_hog_keyboard_msgq;
extern struct bt_gatt_service_static hog_svc;
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL) && IS_ENABLED(CONFIG_BT)
extern struct bt_gatt_service_static split_svc;
#endif
#if IS_ENABLED(CONFIG_DONGLE_SCREEN_LATENCY_SHOW_USB)
extern struct k_sem hid_sem;
#endif

#define QUEUE_TRACK_DEPTH 64

struct queue_context {
    uint32_t timestamps[QUEUE_TRACK_DEPTH];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
};

static struct queue_context ctx_kscan;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
static struct queue_context ctx_split_tx;
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static struct queue_context ctx_split_rx;
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
static struct queue_context ctx_hog;
#endif

static inline void queue_push(struct queue_context *ctx, uint32_t ts)
{
    ctx->timestamps[ctx->head] = ts;
    ctx->head = (ctx->head + 1U) % QUEUE_TRACK_DEPTH;
    if (ctx->count < QUEUE_TRACK_DEPTH) {
        ctx->count++;
    } else {
        ctx->tail = (ctx->tail + 1U) % QUEUE_TRACK_DEPTH;
    }
}

static inline bool queue_pop(struct queue_context *ctx, uint32_t *ts)
{
    if (ctx->count == 0U) {
        return false;
    }

    *ts = ctx->timestamps[ctx->tail];
    ctx->tail = (ctx->tail + 1U) % QUEUE_TRACK_DEPTH;
    ctx->count--;
    return true;
}

static inline uint32_t cycle_delta(uint32_t now, uint32_t then)
{
    return now - then;
}

/* Idle accounting */
static atomic64_t idle_cycle_accum;

uint64_t ds_latency_wrappers_consume_idle_cycles(void)
{
    return atomic_set(&idle_cycle_accum, 0);
}

/* Hooks */
extern int __real_k_msgq_put(struct k_msgq *msgq, const void *data, k_timeout_t timeout);
extern int __real_k_msgq_get(struct k_msgq *msgq, void *data, k_timeout_t timeout);
extern void __real_k_cpu_idle(void);

#if IS_ENABLED(CONFIG_BT)
extern int __real_bt_gatt_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *data, uint16_t len);
extern int __real_bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
extern int __real_zmk_hog_send_keyboard_report(struct zmk_hid_keyboard_report_body *report);
#endif
#if IS_ENABLED(CONFIG_DONGLE_SCREEN_LATENCY_SHOW_USB)
extern int __real_k_sem_take(struct k_sem *sem, k_timeout_t timeout);
extern void __real_k_sem_give(struct k_sem *sem);
extern int __real_hid_int_ep_write(const struct device *dev, const uint8_t *report, size_t len,
                                   uint32_t *bytes_written);
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
extern int __real_zmk_split_transport_central_peripheral_event_handler(
    const struct zmk_split_transport_central *transport, uint8_t source,
    struct zmk_split_transport_peripheral_event ev);
#endif

static uint32_t last_split_ready_cycles;
#if IS_ENABLED(CONFIG_ZMK_BLE)
static uint32_t last_hog_send_cycles;
#endif
#if IS_ENABLED(CONFIG_DONGLE_SCREEN_LATENCY_SHOW_USB)
static uint32_t last_usb_send_cycles;
#endif

int __wrap_k_msgq_put(struct k_msgq *msgq, const void *data, k_timeout_t timeout)
{
    int rc = __real_k_msgq_put(msgq, data, timeout);
    if (rc == 0) {
        uint32_t now = k_cycle_get_32();
        if (msgq == &physical_layouts_kscan_msgq) {
            queue_push(&ctx_kscan, now);
            ds_latency_metrics_note_queue(DS_LAT_METRIC_DEBOUNCE_QUEUE, DS_LATENCY_ORIGIN_LOCAL,
                                          k_msgq_num_used_get(msgq), 0);
        }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
        else if (msgq == &position_state_msgq) {
            queue_push(&ctx_split_tx, now);
            ds_latency_metrics_note_queue(DS_LAT_METRIC_SPLIT_TX_QUEUE, DS_LATENCY_ORIGIN_LOCAL,
                                          k_msgq_num_used_get(msgq), 0);
        }
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        else if (msgq == &peripheral_event_msgq) {
            queue_push(&ctx_split_rx, now);
            ds_latency_metrics_note_queue(DS_LAT_METRIC_SPLIT_RX_QUEUE, DS_LATENCY_ORIGIN_LOCAL,
                                          k_msgq_num_used_get(msgq), 0);
        }
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
        else if (msgq == &zmk_hog_keyboard_msgq) {
            queue_push(&ctx_hog, now);
        }
#endif
    }
    return rc;
}

int __wrap_k_msgq_get(struct k_msgq *msgq, void *data, k_timeout_t timeout)
{
    int rc = __real_k_msgq_get(msgq, data, timeout);
    if (rc == 0) {
        uint32_t now = k_cycle_get_32();
        uint32_t start;
        if (msgq == &physical_layouts_kscan_msgq) {
            if (queue_pop(&ctx_kscan, &start)) {
                ds_latency_metrics_record_cycles(DS_LAT_METRIC_DEBOUNCE_QUEUE,
                                                 DS_LATENCY_ORIGIN_LOCAL,
                                                 cycle_delta(now, start));
            }
        }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
        else if (msgq == &position_state_msgq) {
            if (queue_pop(&ctx_split_tx, &start)) {
                ds_latency_metrics_record_cycles(DS_LAT_METRIC_SPLIT_TX_QUEUE,
                                                 DS_LATENCY_ORIGIN_LOCAL,
                                                 cycle_delta(now, start));
            }
            last_split_ready_cycles = now;
        }
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        else if (msgq == &peripheral_event_msgq) {
            if (queue_pop(&ctx_split_rx, &start)) {
                ds_latency_metrics_record_cycles(DS_LAT_METRIC_SPLIT_RX_QUEUE,
                                                 DS_LATENCY_ORIGIN_LOCAL,
                                                 cycle_delta(now, start));
            }
        }
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
        else if (msgq == &zmk_hog_keyboard_msgq) {
            queue_pop(&ctx_hog, &start);
        }
#endif
    }
    return rc;
}

void __wrap_k_cpu_idle(void)
{
    uint32_t start = k_cycle_get_32();
    __real_k_cpu_idle();
    uint32_t end = k_cycle_get_32();
    atomic_add(&idle_cycle_accum, cycle_delta(end, start));
}

#if IS_ENABLED(CONFIG_BT)
int __wrap_bt_gatt_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *data,
                          uint16_t len)
{
    int rc = __real_bt_gatt_notify(conn, attr, data, len);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
    if (rc == 0 && attr == &split_svc.attrs[1]) {
        uint32_t now = k_cycle_get_32();
        if (last_split_ready_cycles != 0U) {
            ds_latency_metrics_record_cycles(DS_LAT_METRIC_SPLIT_TX_NOTIFY, DS_LATENCY_ORIGIN_LOCAL,
                                             cycle_delta(now, last_split_ready_cycles));
        }
    }
#endif
    return rc;
}

int __wrap_bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params)
{
    int rc = __real_bt_gatt_notify_cb(conn, params);
#if IS_ENABLED(CONFIG_ZMK_BLE)
    if (rc == 0 && params && params->attr == &hog_svc.attrs[5]) {
        uint32_t now = k_cycle_get_32();
        if (last_hog_send_cycles != 0U) {
            ds_latency_metrics_record_cycles(DS_LAT_METRIC_HID_BLE_NOTIFY,
                                             DS_LATENCY_ORIGIN_LOCAL,
                                             cycle_delta(now, last_hog_send_cycles));
        }
    }
#endif
    return rc;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
int __wrap_zmk_hog_send_keyboard_report(struct zmk_hid_keyboard_report_body *report)
{
    last_hog_send_cycles = k_cycle_get_32();
    return __real_zmk_hog_send_keyboard_report(report);
}
#endif

#if IS_ENABLED(CONFIG_DONGLE_SCREEN_LATENCY_SHOW_USB)
int __wrap_k_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
    if (sem == &hid_sem) {
        uint32_t start = k_cycle_get_32();
        int rc = __real_k_sem_take(sem, timeout);
        uint32_t end = k_cycle_get_32();
        if (rc == 0) {
            ds_latency_metrics_record_cycles(DS_LAT_METRIC_USB_FRAME_WAIT, DS_LATENCY_ORIGIN_LOCAL,
                                             cycle_delta(end, start));
        }
        return rc;
    }
    return __real_k_sem_take(sem, timeout);
}

void __wrap_k_sem_give(struct k_sem *sem)
{
    if (sem == &hid_sem && last_usb_send_cycles != 0U) {
        uint32_t now = k_cycle_get_32();
        ds_latency_metrics_record_cycles(DS_LAT_METRIC_USB_TX, DS_LATENCY_ORIGIN_LOCAL,
                                         cycle_delta(now, last_usb_send_cycles));
    }
    __real_k_sem_give(sem);
}

int __wrap_hid_int_ep_write(const struct device *dev, const uint8_t *report, size_t len,
                            uint32_t *bytes_written)
{
    last_usb_send_cycles = k_cycle_get_32();
    return __real_hid_int_ep_write(dev, report, len, bytes_written);
}
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
int __wrap_zmk_split_transport_central_peripheral_event_handler(
    const struct zmk_split_transport_central *transport, uint8_t source,
    struct zmk_split_transport_peripheral_event ev)
{
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (ev.type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT &&
        ev.data.sensor_event.sensor_index == CONFIG_DONGLE_SCREEN_LATENCY_SENSOR_INDEX) {
        int32_t raw_metric = ev.data.sensor_event.channel_data.value.val2;
        uint32_t value = (uint32_t)MAX(ev.data.sensor_event.channel_data.value.val1, 0);
        if (raw_metric < 0 || raw_metric >= DS_LAT_METRIC_COUNT) {
            return 0;
        }
        enum ds_latency_metric metric = (enum ds_latency_metric)raw_metric;
        uint8_t origin = source + DS_LATENCY_ORIGIN_REMOTE_0;
        if (metric < DS_LAT_METRIC_COUNT) {
            ds_latency_metrics_process_remote(metric, origin, value);
        }
        return 0;
    }
#endif
    return __real_zmk_split_transport_central_peripheral_event_handler(transport, source, ev);
}
#endif
