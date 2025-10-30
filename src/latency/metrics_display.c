#include <zephyr/kernel.h>
#include <string.h>

#include "dongle_screen/latency/metrics.h"

void ds_latency_metrics_format(char *buffer, size_t buffer_len)
{
    if (buffer_len == 0U) {
        return;
    }

    struct ds_latency_display_snapshot snap = ds_latency_metrics_snapshot();
    size_t offset = 0U;

    for (size_t i = 0; i < snap.line_count; i++) {
        const char *line = snap.lines[i].text;
        size_t line_len = strlen(line);
        if (offset + line_len + 1 >= buffer_len) {
            break;
        }
        memcpy(&buffer[offset], line, line_len);
        offset += line_len;
        buffer[offset++] = '\n';
    }

    if (offset >= buffer_len) {
        offset = buffer_len - 1;
    }
    buffer[offset] = '\0';
}
