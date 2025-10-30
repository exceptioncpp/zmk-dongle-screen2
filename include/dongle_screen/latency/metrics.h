#ifndef DONGLE_SCREEN_LATENCY_METRICS_H_
#define DONGLE_SCREEN_LATENCY_METRICS_H_

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS_LATENCY_WINDOW_SIZE CONFIG_DONGLE_SCREEN_LATENCY_WINDOW
#define DS_LATENCY_MAX_ORIGINS 4

#define DS_LATENCY_REMOTE_QUEUE_FLAG 0x80000000U
#define DS_LATENCY_REMOTE_QUEUE_PACK(depth, capacity)                                              \
    ((((uint32_t)(capacity) & 0xFFFFU) << 16) | ((uint32_t)(depth) & 0xFFFFU))
#define DS_LATENCY_REMOTE_QUEUE_DEPTH(value) ((uint16_t)((value) & 0xFFFFU))
#define DS_LATENCY_REMOTE_QUEUE_CAPACITY(value) ((uint16_t)(((value) >> 16) & 0xFFFFU))

enum ds_latency_origin {
    DS_LATENCY_ORIGIN_LOCAL = 0,
    DS_LATENCY_ORIGIN_REMOTE_0 = 1,
    DS_LATENCY_ORIGIN_REMOTE_1 = 2,
    DS_LATENCY_ORIGIN_REMOTE_2 = 3,
};

enum ds_latency_metric {
    DS_LAT_METRIC_DEBOUNCE_QUEUE = 0,
    DS_LAT_METRIC_SPLIT_TX_QUEUE,
    DS_LAT_METRIC_SPLIT_TX_NOTIFY,
    DS_LAT_METRIC_SPLIT_RX_QUEUE,
    DS_LAT_METRIC_HID_BLE_NOTIFY,
    DS_LAT_METRIC_USB_FRAME_WAIT,
    DS_LAT_METRIC_USB_TX,
    DS_LAT_METRIC_CPU_IDLE,
    DS_LAT_METRIC_COUNT,
};

struct ds_latency_stat {
    uint32_t current_us;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t avg_us;
    uint32_t p95_us;
    uint16_t sample_count;
};

struct ds_latency_queue_stat {
    uint16_t depth;
    uint16_t max_depth;
    uint16_t capacity;
    bool valid;
};

struct ds_latency_display_line {
    char text[48];
};

struct ds_latency_display_snapshot {
    struct ds_latency_display_line lines[8];
    size_t line_count;
};

void ds_latency_metrics_init(void);

void ds_latency_metrics_record_cycles(enum ds_latency_metric metric, uint8_t origin,
                                      uint32_t delta_cycles);
void ds_latency_metrics_record_us(enum ds_latency_metric metric, uint8_t origin,
                                  uint32_t delta_us);

bool ds_latency_metrics_stat(enum ds_latency_metric metric, uint8_t origin,
                             struct ds_latency_stat *out);

void ds_latency_metrics_note_queue(enum ds_latency_metric metric, uint8_t origin,
                                   uint16_t depth, uint16_t capacity);

bool ds_latency_metrics_queue_stat(enum ds_latency_metric metric, uint8_t origin,
                                   struct ds_latency_queue_stat *out);

void ds_latency_metrics_set_cpu_idle(uint8_t idle_pct);

uint32_t ds_latency_cycles_to_us(uint32_t cycles);

struct ds_latency_display_snapshot ds_latency_metrics_snapshot(void);

void ds_latency_metrics_process_remote(enum ds_latency_metric metric, uint8_t origin,
                                       uint32_t value_us);

void ds_latency_metrics_process_remote_queue(enum ds_latency_metric metric, uint8_t origin,
                                             uint16_t depth, uint16_t capacity);

void ds_latency_metrics_tick(void);

uint64_t ds_latency_wrappers_consume_idle_cycles(void);

void ds_latency_metrics_format(char *buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif /* DONGLE_SCREEN_LATENCY_METRICS_H_ */
