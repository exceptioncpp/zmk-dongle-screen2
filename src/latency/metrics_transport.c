#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <limits.h>

#include "dongle_screen/latency/metrics.h"

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>
#include <zephyr/drivers/sensor.h>
#endif

static struct k_work_delayable metrics_work;
static uint32_t last_cycle_snapshot;

static void update_cpu_stats(uint32_t now)
{
    if (last_cycle_snapshot == 0U) {
        last_cycle_snapshot = now;
        return;
    }

    uint32_t elapsed = now - last_cycle_snapshot;
    last_cycle_snapshot = now;
    if (elapsed == 0U) {
        return;
    }

    uint64_t idle_cycles = ds_latency_wrappers_consume_idle_cycles();
    uint64_t pct = (idle_cycles * 100ULL) / elapsed;
    if (pct > 100ULL) {
        pct = 100ULL;
    }

    ds_latency_metrics_set_cpu_idle((uint8_t)pct);
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
static void send_metric(enum ds_latency_metric metric)
{
    struct ds_latency_stat stat;
    if (!ds_latency_metrics_stat(metric, DS_LATENCY_ORIGIN_LOCAL, &stat)) {
        return;
    }

    struct zmk_split_transport_peripheral_event ev = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT,
        .data.sensor_event = {
            .sensor_index = CONFIG_DONGLE_SCREEN_LATENCY_SENSOR_INDEX,
        },
    };

    ev.data.sensor_event.channel_data.value.val1 = (int32_t)MIN(stat.avg_us, (uint32_t)INT32_MAX);
    ev.data.sensor_event.channel_data.value.val2 = (int32_t)metric;
    ev.data.sensor_event.channel_data.channel = SENSOR_CHAN_ACCEL_X;

    zmk_split_peripheral_report_event(&ev);
}
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
static void send_queue_metric(enum ds_latency_metric metric)
{
    struct ds_latency_queue_stat qstat;
    if (!ds_latency_metrics_queue_stat(metric, DS_LATENCY_ORIGIN_LOCAL, &qstat)) {
        return;
    }

    struct zmk_split_transport_peripheral_event ev = {
        .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT,
        .data.sensor_event = {
            .sensor_index = CONFIG_DONGLE_SCREEN_LATENCY_SENSOR_INDEX,
        },
    };

    uint32_t packed = DS_LATENCY_REMOTE_QUEUE_PACK(qstat.depth, qstat.capacity);
    ev.data.sensor_event.channel_data.value.val1 = (int32_t)packed;
    ev.data.sensor_event.channel_data.value.val2 =
        (int32_t)(DS_LATENCY_REMOTE_QUEUE_FLAG | metric);
    ev.data.sensor_event.channel_data.channel = SENSOR_CHAN_ACCEL_X;

    zmk_split_peripheral_report_event(&ev);
}
#endif

static void metrics_work_handler(struct k_work *work)
{
    uint32_t now = k_cycle_get_32();

    update_cpu_stats(now);
    ds_latency_metrics_tick();

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
    send_metric(DS_LAT_METRIC_DEBOUNCE_QUEUE);
    send_metric(DS_LAT_METRIC_SPLIT_TX_QUEUE);
    send_metric(DS_LAT_METRIC_SPLIT_TX_NOTIFY);
    send_metric(DS_LAT_METRIC_CPU_IDLE);
    send_queue_metric(DS_LAT_METRIC_DEBOUNCE_QUEUE);
    send_queue_metric(DS_LAT_METRIC_SPLIT_TX_QUEUE);
#endif

    k_work_schedule(&metrics_work,
                    K_MSEC(CONFIG_DONGLE_SCREEN_LATENCY_REMOTE_PERIOD_MS));
}

static int metrics_transport_init(void)
{
    ds_latency_metrics_init();
    k_work_init_delayable(&metrics_work, metrics_work_handler);
    k_work_schedule(&metrics_work,
                    K_MSEC(CONFIG_DONGLE_SCREEN_LATENCY_REMOTE_PERIOD_MS));
    return 0;
}

SYS_INIT(metrics_transport_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
