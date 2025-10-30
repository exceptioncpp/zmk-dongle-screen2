#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/__assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "dongle_screen/latency/metrics.h"

#include <zephyr/sys/printk.h>

struct ds_latency_window {
    uint32_t samples[DS_LATENCY_WINDOW_SIZE];
    uint16_t count;
    uint16_t next;
    uint32_t current;
    uint64_t sum;
    uint32_t min;
    uint32_t max;
    bool dirty;
};

struct ds_latency_state {
    struct ds_latency_window windows[DS_LAT_METRIC_COUNT][DS_LATENCY_MAX_ORIGINS];
    struct ds_latency_queue_stat queues[DS_LAT_METRIC_COUNT][DS_LATENCY_MAX_ORIGINS];
    struct k_spinlock lock;
};

static struct ds_latency_state latency_state;

static inline struct ds_latency_window *get_window(enum ds_latency_metric metric, uint8_t origin)
{
    __ASSERT_NO_MSG(metric < DS_LAT_METRIC_COUNT);
    __ASSERT_NO_MSG(origin < DS_LATENCY_MAX_ORIGINS);
    return &latency_state.windows[metric][origin];
}

static void recompute_min_max(struct ds_latency_window *win)
{
    if (win->count == 0) {
        win->min = 0;
        win->max = 0;
        win->dirty = false;
        return;
    }

    uint32_t min_val = UINT32_MAX;
    uint32_t max_val = 0;
    for (uint16_t i = 0; i < win->count; i++) {
        uint16_t idx = (win->next + DS_LATENCY_WINDOW_SIZE - win->count + i) % DS_LATENCY_WINDOW_SIZE;
        uint32_t v = win->samples[idx];
        if (v < min_val) {
            min_val = v;
        }
        if (v > max_val) {
            max_val = v;
        }
    }
    win->min = min_val;
    win->max = max_val;
    win->dirty = false;
}

static void window_push(struct ds_latency_window *win, uint32_t value)
{
    if (win->count == DS_LATENCY_WINDOW_SIZE) {
        uint32_t old = win->samples[win->next];
        if (old == win->min || old == win->max) {
            win->dirty = true;
        }
        win->sum -= old;
    } else {
        win->count++;
    }

    win->samples[win->next] = value;
    win->next = (win->next + 1) % DS_LATENCY_WINDOW_SIZE;
    win->current = value;
    win->sum += value;

    if (!win->dirty) {
        if (win->count == 1) {
            win->min = value;
            win->max = value;
        } else {
            if (value < win->min) {
                win->min = value;
            }
            if (value > win->max) {
                win->max = value;
            }
        }
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    if (va < vb) {
        return -1;
    } else if (va > vb) {
        return 1;
    }
    return 0;
}

static uint32_t buffer_percentile(uint32_t *buffer, uint16_t count, uint8_t percentile)
{
    if (count == 0U) {
        return 0U;
    }

    qsort(buffer, count, sizeof(uint32_t), cmp_u32);

    uint32_t rank = ((uint32_t)(count - 1U) * percentile) / 100U;
    return buffer[rank];
}

uint32_t ds_latency_cycles_to_us(uint32_t cycles)
{
    uint64_t hz = sys_clock_hw_cycles_per_sec();
    if (hz == 0U) {
        return 0U;
    }
    uint64_t us = ((uint64_t)cycles * 1000000ULL) / hz;
    return (uint32_t)us;
}

void ds_latency_metrics_init(void)
{
    memset(&latency_state, 0, sizeof(latency_state));
}

void ds_latency_metrics_record_cycles(enum ds_latency_metric metric, uint8_t origin,
                                      uint32_t delta_cycles)
{
    ds_latency_metrics_record_us(metric, origin, ds_latency_cycles_to_us(delta_cycles));
}

void ds_latency_metrics_record_us(enum ds_latency_metric metric, uint8_t origin,
                                  uint32_t delta_us)
{
    struct k_spinlock_key key = k_spin_lock(&latency_state.lock);
    struct ds_latency_window *win = get_window(metric, origin);
    window_push(win, delta_us);
    k_spin_unlock(&latency_state.lock, key);
}

bool ds_latency_metrics_stat(enum ds_latency_metric metric, uint8_t origin,
                             struct ds_latency_stat *out)
{
    uint32_t samples[DS_LATENCY_WINDOW_SIZE];
    uint16_t count;
    uint32_t current;
    uint32_t min_val;
    uint32_t max_val;
    uint64_t sum;

    struct k_spinlock_key key = k_spin_lock(&latency_state.lock);
    struct ds_latency_window *win = get_window(metric, origin);
    if (win->count == 0U) {
        memset(out, 0, sizeof(*out));
        k_spin_unlock(&latency_state.lock, key);
        return false;
    }

    if (win->dirty) {
        recompute_min_max(win);
    }

    count = win->count;
    current = win->current;
    min_val = win->min;
    max_val = win->max;
    sum = win->sum;

    uint16_t start = (win->next + DS_LATENCY_WINDOW_SIZE - count) % DS_LATENCY_WINDOW_SIZE;
    for (uint16_t i = 0U; i < count; i++) {
        uint16_t idx = (start + i) % DS_LATENCY_WINDOW_SIZE;
        samples[i] = win->samples[idx];
    }

    k_spin_unlock(&latency_state.lock, key);

    uint32_t p95 = buffer_percentile(samples, count, 95U);

    out->current_us = current;
    out->min_us = min_val;
    out->max_us = max_val;
    out->avg_us = (uint32_t)(sum / count);
    out->p95_us = p95;
    out->sample_count = count;

    return true;
}

void ds_latency_metrics_note_queue(enum ds_latency_metric metric, uint8_t origin,
                                   uint16_t depth, uint16_t capacity)
{
    struct k_spinlock_key key = k_spin_lock(&latency_state.lock);
    struct ds_latency_queue_stat *stat = &latency_state.queues[metric][origin];
    stat->depth = depth;
    if (stat->max_depth < depth) {
        stat->max_depth = depth;
    }
    stat->capacity = capacity;
    stat->valid = true;
    k_spin_unlock(&latency_state.lock, key);
}

bool ds_latency_metrics_queue_stat(enum ds_latency_metric metric, uint8_t origin,
                                   struct ds_latency_queue_stat *out)
{
    struct k_spinlock_key key = k_spin_lock(&latency_state.lock);
    if (!latency_state.queues[metric][origin].valid) {
        memset(out, 0, sizeof(*out));
        k_spin_unlock(&latency_state.lock, key);
        return false;
    }

    *out = latency_state.queues[metric][origin];
    k_spin_unlock(&latency_state.lock, key);
    return true;
}

void ds_latency_metrics_set_cpu_idle(uint8_t idle_pct)
{
    ds_latency_metrics_record_us(DS_LAT_METRIC_CPU_IDLE, DS_LATENCY_ORIGIN_LOCAL, idle_pct);
}

static void append_line(struct ds_latency_display_snapshot *snap, const char *fmt, ...)
{
    if (snap->line_count >= ARRAY_SIZE(snap->lines)) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintk(snap->lines[snap->line_count].text,
              sizeof(snap->lines[snap->line_count].text), fmt, args);
    va_end(args);
    snap->line_count++;
}

struct ds_latency_display_snapshot ds_latency_metrics_snapshot(void)
{
    struct ds_latency_display_snapshot snap = {0};
    struct ds_latency_stat stat;

    if (ds_latency_metrics_stat(DS_LAT_METRIC_DEBOUNCE_QUEUE, DS_LATENCY_ORIGIN_LOCAL, &stat)) {
        append_line(&snap, "Scan L cur:%u avg:%u p95:%u", stat.current_us, stat.avg_us,
                    stat.p95_us);
    }

    if (ds_latency_metrics_stat(DS_LAT_METRIC_SPLIT_TX_QUEUE, DS_LATENCY_ORIGIN_LOCAL, &stat)) {
        append_line(&snap, "Split TX q cur:%u avg:%u", stat.current_us, stat.avg_us);
    }

    if (ds_latency_metrics_stat(DS_LAT_METRIC_SPLIT_TX_NOTIFY, DS_LATENCY_ORIGIN_LOCAL, &stat)) {
        append_line(&snap, "Split TX air cur:%u p95:%u", stat.current_us, stat.p95_us);
    }

    if (ds_latency_metrics_stat(DS_LAT_METRIC_SPLIT_RX_QUEUE, DS_LATENCY_ORIGIN_LOCAL, &stat)) {
        append_line(&snap, "Split RX q cur:%u avg:%u", stat.current_us, stat.avg_us);
    }

    if (ds_latency_metrics_stat(DS_LAT_METRIC_HID_BLE_NOTIFY, DS_LATENCY_ORIGIN_LOCAL, &stat)) {
        append_line(&snap, "Host BLE cur:%u avg:%u", stat.current_us, stat.avg_us);
    }

    if (ds_latency_metrics_stat(DS_LAT_METRIC_USB_FRAME_WAIT, DS_LATENCY_ORIGIN_LOCAL, &stat)) {
        append_line(&snap, "USB wait cur:%u avg:%u", stat.current_us, stat.avg_us);
    }

    if (ds_latency_metrics_stat(DS_LAT_METRIC_USB_TX, DS_LATENCY_ORIGIN_LOCAL, &stat)) {
        append_line(&snap, "USB tx cur:%u avg:%u", stat.current_us, stat.avg_us);
    }

    struct ds_latency_queue_stat local_queue;
    if (ds_latency_metrics_queue_stat(DS_LAT_METRIC_DEBOUNCE_QUEUE, DS_LATENCY_ORIGIN_LOCAL,
                                      &local_queue)) {
        append_line(&snap, "Scan q %u/%u (max %u)", local_queue.depth, local_queue.capacity,
                    local_queue.max_depth);
    }
    if (ds_latency_metrics_queue_stat(DS_LAT_METRIC_SPLIT_TX_QUEUE, DS_LATENCY_ORIGIN_LOCAL,
                                      &local_queue)) {
        append_line(&snap, "Split TX q %u/%u (max %u)", local_queue.depth, local_queue.capacity,
                    local_queue.max_depth);
    }
    if (ds_latency_metrics_queue_stat(DS_LAT_METRIC_SPLIT_RX_QUEUE, DS_LATENCY_ORIGIN_LOCAL,
                                      &local_queue)) {
        append_line(&snap, "Split RX q %u/%u (max %u)", local_queue.depth, local_queue.capacity,
                    local_queue.max_depth);
    }

    if (ds_latency_metrics_stat(DS_LAT_METRIC_CPU_IDLE, DS_LATENCY_ORIGIN_LOCAL, &stat)) {
        append_line(&snap, "CPU idle cur:%u avg:%u", stat.current_us, stat.avg_us);
    }

    for (uint8_t origin = DS_LATENCY_ORIGIN_REMOTE_0; origin < DS_LATENCY_MAX_ORIGINS; origin++) {
        if (ds_latency_metrics_stat(DS_LAT_METRIC_DEBOUNCE_QUEUE, origin, &stat)) {
            append_line(&snap, "Scan P%u cur:%u avg:%u", origin - 1, stat.current_us, stat.avg_us);
        }
        if (ds_latency_metrics_stat(DS_LAT_METRIC_SPLIT_TX_QUEUE, origin, &stat)) {
            append_line(&snap, "P%u TX q cur:%u avg:%u", origin - 1, stat.current_us, stat.avg_us);
        }
        if (ds_latency_metrics_stat(DS_LAT_METRIC_SPLIT_TX_NOTIFY, origin, &stat)) {
            append_line(&snap, "P%u air cur:%u avg:%u", origin - 1, stat.current_us, stat.avg_us);
        }
        if (ds_latency_metrics_stat(DS_LAT_METRIC_CPU_IDLE, origin, &stat)) {
            append_line(&snap, "P%u CPU cur:%u avg:%u", origin - 1, stat.current_us, stat.avg_us);
        }

        struct ds_latency_queue_stat remote_queue;
        if (ds_latency_metrics_queue_stat(DS_LAT_METRIC_DEBOUNCE_QUEUE, origin, &remote_queue)) {
            append_line(&snap, "P%u scan q %u/%u (max %u)", origin - 1, remote_queue.depth,
                        remote_queue.capacity, remote_queue.max_depth);
        }
        if (ds_latency_metrics_queue_stat(DS_LAT_METRIC_SPLIT_TX_QUEUE, origin, &remote_queue)) {
            append_line(&snap, "P%u tx q %u/%u (max %u)", origin - 1, remote_queue.depth,
                        remote_queue.capacity, remote_queue.max_depth);
        }
    }

    return snap;
}

void ds_latency_metrics_process_remote(enum ds_latency_metric metric, uint8_t origin,
                                       uint32_t value_us)
{
    if (origin >= DS_LATENCY_MAX_ORIGINS) {
        return;
    }
    ds_latency_metrics_record_us(metric, origin, value_us);
}

void ds_latency_metrics_process_remote_queue(enum ds_latency_metric metric, uint8_t origin,
                                             uint16_t depth, uint16_t capacity)
{
    if (origin >= DS_LATENCY_MAX_ORIGINS) {
        return;
    }

    struct k_spinlock_key key = k_spin_lock(&latency_state.lock);
    struct ds_latency_queue_stat *stat = &latency_state.queues[metric][origin];
    stat->depth = depth;
    if (stat->max_depth < depth) {
        stat->max_depth = depth;
    }
    stat->capacity = capacity;
    stat->valid = true;
    k_spin_unlock(&latency_state.lock, key);
}

void ds_latency_metrics_tick(void)
{
    /* Intentionally empty for now. Placeholder for future periodic updates. */
}
