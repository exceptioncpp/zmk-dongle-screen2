#pragma once

#include <lvgl.h>
#include <zephyr/sys/slist.h>

struct zmk_widget_latency_status {
    lv_obj_t *obj;
    lv_obj_t *label;
    lv_timer_t *timer;
    sys_snode_t node;
};

int zmk_widget_latency_status_init(struct zmk_widget_latency_status *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_latency_status_obj(struct zmk_widget_latency_status *widget);
