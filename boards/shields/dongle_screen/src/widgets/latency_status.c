#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "latency_status.h"
#include "dongle_screen/latency/metrics.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

static void latency_status_timer_cb(lv_timer_t *timer)
{
    struct zmk_widget_latency_status *widget = timer->user_data;
    if (!widget || !widget->label) {
        return;
    }

    char buffer[192];
    ds_latency_metrics_format(buffer, sizeof(buffer));
    lv_label_set_text(widget->label, buffer[0] ? buffer : "latency\n---");
}

int zmk_widget_latency_status_init(struct zmk_widget_latency_status *widget, lv_obj_t *parent)
{
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 120, 90);

    widget->label = lv_label_create(widget->obj);
    lv_obj_set_width(widget->label, 118);
    lv_label_set_long_mode(widget->label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(widget->label, "latency\n...");

    widget->timer = lv_timer_create(latency_status_timer_cb, 200, widget);

    sys_slist_append(&widgets, &widget->node);
    return 0;
}

lv_obj_t *zmk_widget_latency_status_obj(struct zmk_widget_latency_status *widget)
{
    return widget->obj;
}
