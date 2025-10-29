/*
 * Copyright (c) 2024
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zmk_ssd1351

#include "display_ssd1351.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

LOG_MODULE_REGISTER(display_ssd1351, CONFIG_DISPLAY_LOG_LEVEL);

struct ssd1351_config {
  struct spi_dt_spec bus;
  struct gpio_dt_spec cmd_data_gpio;
  struct gpio_dt_spec reset_gpio;
  uint16_t width;
  uint16_t height;
  uint16_t x_offset;
  uint16_t y_offset;
};

struct ssd1351_data {
  uint16_t x_offset;
  uint16_t y_offset;
  enum display_orientation orientation;
};

#define SSD1351_PIXEL_SIZE 2u

static void ssd1351_set_offsets(const struct device *dev, uint16_t x_offset,
                                uint16_t y_offset) {
  struct ssd1351_data *data = dev->data;

  data->x_offset = x_offset;
  data->y_offset = y_offset;
}

static int ssd1351_transmit(const struct device *dev, uint8_t cmd,
                            const uint8_t *tx_data, size_t tx_count) {
  const struct ssd1351_config *config = dev->config;
  struct spi_buf buf = {
      .buf = (void *)&cmd,
      .len = 1,
  };
  struct spi_buf_set buf_set = {
      .buffers = &buf,
      .count = 1,
  };
  int ret = 0;

  if (cmd != SSD1351_CMD_NONE) {
    if (config->cmd_data_gpio.port != NULL) {
      gpio_pin_set_dt(&config->cmd_data_gpio, 1);
    }
    ret = spi_write_dt(&config->bus, &buf_set);
    if (ret < 0) {
      return ret;
    }
  }

  if ((tx_data != NULL) && (tx_count > 0U)) {
    buf.buf = (void *)tx_data;
    buf.len = tx_count;
    if (config->cmd_data_gpio.port != NULL) {
      gpio_pin_set_dt(&config->cmd_data_gpio, 0);
    }
    ret = spi_write_dt(&config->bus, &buf_set);
  }

  return ret;
}

static void ssd1351_reset(const struct device *dev) {
  const struct ssd1351_config *config = dev->config;

  if (config->reset_gpio.port != NULL) {
    k_sleep(K_MSEC(1));
    gpio_pin_set_dt(&config->reset_gpio, 1);
    k_sleep(K_MSEC(10));
    gpio_pin_set_dt(&config->reset_gpio, 0);
    k_sleep(K_MSEC(10));
  }
}

static int ssd1351_blanking_on(const struct device *dev) {
  return ssd1351_transmit(dev, SSD1351_CMD_DISPLAYOFF, NULL, 0);
}

static int ssd1351_blanking_off(const struct device *dev) {
  return ssd1351_transmit(dev, SSD1351_CMD_DISPLAYON, NULL, 0);
}

static void ssd1351_set_mem_area(const struct device *dev, uint16_t x,
                                 uint16_t y, uint16_t w, uint16_t h) {
  struct ssd1351_data *data = dev->data;
  uint16_t x1 = x;
  uint16_t y1 = y;
  uint16_t x2 = x + w - 1;
  uint16_t y2 = y + h - 1;

  if ((data->orientation == DISPLAY_ORIENTATION_ROTATED_90) ||
      (data->orientation == DISPLAY_ORIENTATION_ROTATED_270)) {
    uint16_t tmp;

    tmp = x1;
    x1 = y1;
    y1 = tmp;
    tmp = x2;
    x2 = y2;
    y2 = tmp;
  }

  x1 += data->x_offset;
  x2 += data->x_offset;
  y1 += data->y_offset;
  y2 += data->y_offset;

  uint8_t col_param[2] = {x1 & 0xFF, x2 & 0xFF};
  uint8_t row_param[2] = {y1 & 0xFF, y2 & 0xFF};

  ssd1351_transmit(dev, SSD1351_CMD_SETCOLUMN, col_param, sizeof(col_param));
  ssd1351_transmit(dev, SSD1351_CMD_SETROW, row_param, sizeof(row_param));
}

static int ssd1351_write(const struct device *dev, uint16_t x, uint16_t y,
                         const struct display_buffer_descriptor *desc,
                         const void *buf) {
  const uint8_t *write_data_start = buf;
  uint16_t nbr_of_writes;
  uint16_t write_h;

  __ASSERT(desc->width <= desc->pitch, "Pitch is smaller then width");
  __ASSERT((desc->pitch * SSD1351_PIXEL_SIZE * desc->height) <= desc->buf_size,
           "Input buffer too small");

  ssd1351_set_mem_area(dev, x, y, desc->width, desc->height);

  if (desc->pitch > desc->width) {
    write_h = 1U;
    nbr_of_writes = desc->height;
  } else {
    write_h = desc->height;
    nbr_of_writes = 1U;
  }

  for (uint16_t write_cnt = 0U; write_cnt < nbr_of_writes; ++write_cnt) {
    ssd1351_transmit(
        dev, write_cnt == 0U ? SSD1351_CMD_WRITERAM : SSD1351_CMD_NONE,
        (void *)write_data_start, desc->width * SSD1351_PIXEL_SIZE * write_h);
    write_data_start += (desc->pitch * SSD1351_PIXEL_SIZE);
  }

  return 0;
}

static void
ssd1351_get_capabilities(const struct device *dev,
                         struct display_capabilities *capabilities) {
  const struct ssd1351_config *config = dev->config;
  const struct ssd1351_data *data = dev->data;

  memset(capabilities, 0, sizeof(struct display_capabilities));
  capabilities->x_resolution = config->width;
  capabilities->y_resolution = config->height;
  capabilities->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
  capabilities->current_pixel_format = PIXEL_FORMAT_RGB_565;
  capabilities->current_orientation = data->orientation;
}

static int ssd1351_set_pixel_format(const struct device *dev,
                                    enum display_pixel_format pixel_format) {
  if (pixel_format == PIXEL_FORMAT_RGB_565) {
    return 0;
  }

  return -ENOTSUP;
}

static int ssd1351_set_orientation(const struct device *dev,
                                   enum display_orientation orientation) {
  const struct ssd1351_config *config = dev->config;
  uint8_t remap = 0b01100100;
  uint8_t startline;
  uint8_t rotation;

  switch (orientation) {
  case DISPLAY_ORIENTATION_NORMAL:
    rotation = 0U;
    break;
  case DISPLAY_ORIENTATION_ROTATED_90:
    rotation = 1U;
    break;
  case DISPLAY_ORIENTATION_ROTATED_180:
    rotation = 2U;
    break;
  case DISPLAY_ORIENTATION_ROTATED_270:
    rotation = 3U;
    break;
  default:
    return -ENOTSUP;
  }

  switch (rotation) {
  case 0U:
    remap |= 0b00010000;
    ssd1351_set_offsets(dev, config->x_offset, config->y_offset);
    break;
  case 1U:
    remap |= 0b00010011;
    ssd1351_set_offsets(dev, config->y_offset, config->x_offset);
    break;
  case 2U:
    remap |= 0b00000010;
    ssd1351_set_offsets(dev, config->x_offset, config->y_offset);
    break;
  case 3U:
    remap |= 0b00000001;
    ssd1351_set_offsets(dev, config->y_offset, config->x_offset);
    break;
  default:
    return -ENOTSUP;
  }

  startline = (rotation < 2U) ? config->height : 0U;

  int ret = ssd1351_transmit(dev, SSD1351_CMD_SETREMAP, &remap, 1);
  if (ret < 0) {
    return ret;
  }

  ret = ssd1351_transmit(dev, SSD1351_CMD_STARTLINE, &startline, 1);
  if (ret < 0) {
    return ret;
  }

  struct ssd1351_data *data = dev->data;
  data->orientation = orientation;

  return 0;
}

static int ssd1351_lcd_init(const struct device *dev) {
  const struct ssd1351_config *config = dev->config;
  int ret;
  uint8_t param;

  param = 0x12;
  ret = ssd1351_transmit(dev, SSD1351_CMD_COMMANDLOCK, &param, 1);
  if (ret < 0) {
    return ret;
  }

  param = 0xB1;
  ret = ssd1351_transmit(dev, SSD1351_CMD_COMMANDLOCK, &param, 1);
  if (ret < 0) {
    return ret;
  }

  ret = ssd1351_transmit(dev, SSD1351_CMD_DISPLAYOFF, NULL, 0);
  if (ret < 0) {
    return ret;
  }

  param = 0xF1;
  ret = ssd1351_transmit(dev, SSD1351_CMD_CLOCKDIV, &param, 1);
  if (ret < 0) {
    return ret;
  }

  param = config->height - 1;
  ret = ssd1351_transmit(dev, SSD1351_CMD_MUXRATIO, &param, 1);
  if (ret < 0) {
    return ret;
  }

  param = 0x00;
  ret = ssd1351_transmit(dev, SSD1351_CMD_DISPLAYOFFSET, &param, 1);
  if (ret < 0) {
    return ret;
  }

  param = 0x00;
  ret = ssd1351_transmit(dev, SSD1351_CMD_SETGPIO, &param, 1);
  if (ret < 0) {
    return ret;
  }

  param = 0x01;
  ret = ssd1351_transmit(dev, SSD1351_CMD_FUNCTIONSELECT, &param, 1);
  if (ret < 0) {
    return ret;
  }

  param = 0x32;
  ret = ssd1351_transmit(dev, SSD1351_CMD_PRECHARGE, &param, 1);
  if (ret < 0) {
    return ret;
  }

  param = 0x05;
  ret = ssd1351_transmit(dev, SSD1351_CMD_VCOMH, &param, 1);
  if (ret < 0) {
    return ret;
  }

  uint8_t contrast[] = {0xC8, 0x80, 0xC8};
  ret = ssd1351_transmit(dev, SSD1351_CMD_CONTRASTABC, contrast,
                         sizeof(contrast));
  if (ret < 0) {
    return ret;
  }

  param = 0x0F;
  ret = ssd1351_transmit(dev, SSD1351_CMD_CONTRASTMASTER, &param, 1);
  if (ret < 0) {
    return ret;
  }

  uint8_t vsl[] = {0xA0, 0xB5, 0x55};
  ret = ssd1351_transmit(dev, SSD1351_CMD_SETVSL, vsl, sizeof(vsl));
  if (ret < 0) {
    return ret;
  }

  param = 0x01;
  ret = ssd1351_transmit(dev, SSD1351_CMD_PRECHARGE2, &param, 1);
  if (ret < 0) {
    return ret;
  }

  ret = ssd1351_transmit(dev, SSD1351_CMD_NORMALDISPLAY, NULL, 0);
  if (ret < 0) {
    return ret;
  }

  return ssd1351_set_orientation(dev, DISPLAY_ORIENTATION_NORMAL);
}

static int ssd1351_init(const struct device *dev) {
  const struct ssd1351_config *config = dev->config;
  int ret;

  if (!spi_is_ready_dt(&config->bus)) {
    LOG_ERR("SPI device not ready");
    return -ENODEV;
  }

  if (config->reset_gpio.port != NULL) {
    if (!gpio_is_ready_dt(&config->reset_gpio)) {
      LOG_ERR("Reset GPIO device not ready");
      return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
      LOG_ERR("Couldn't configure reset pin");
      return ret;
    }
  }

  if (config->cmd_data_gpio.port != NULL) {
    if (!gpio_is_ready_dt(&config->cmd_data_gpio)) {
      LOG_ERR("CMD/DATA GPIO device not ready");
      return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&config->cmd_data_gpio, GPIO_OUTPUT);
    if (ret < 0) {
      LOG_ERR("Couldn't configure CMD/DATA pin");
      return ret;
    }
  }

  ssd1351_reset(dev);

  ret = ssd1351_lcd_init(dev);
  if (ret < 0) {
    return ret;
  }

  return ssd1351_blanking_off(dev);
}

static int ssd1351_pm_action(const struct device *dev,
                             enum pm_device_action action) {
  switch (action) {
  case PM_DEVICE_ACTION_RESUME:
    return ssd1351_blanking_off(dev);
  case PM_DEVICE_ACTION_SUSPEND:
    return ssd1351_blanking_on(dev);
  default:
    return -ENOTSUP;
  }
}

#ifdef CONFIG_PM_DEVICE
#define SSD1351_PM_ACTION_DEFINE(inst)                                         \
  PM_DEVICE_DT_INST_DEFINE(inst, ssd1351_pm_action);
#define SSD1351_PM_ACTION_GET(inst) PM_DEVICE_DT_INST_GET(inst)
#else
#define SSD1351_PM_ACTION_DEFINE(inst)
#define SSD1351_PM_ACTION_GET(inst) NULL
#endif

static const struct display_driver_api ssd1351_api = {
    .blanking_on = ssd1351_blanking_on,
    .blanking_off = ssd1351_blanking_off,
    .write = ssd1351_write,
    .get_capabilities = ssd1351_get_capabilities,
    .set_pixel_format = ssd1351_set_pixel_format,
    .set_orientation = ssd1351_set_orientation,
};

#define SSD1351_INIT(inst)                                                     \
  static const struct ssd1351_config ssd1351_config_##inst = {                 \
      .bus =                                                                   \
          SPI_DT_SPEC_INST_GET(inst, SPI_OP_MODE_MASTER | SPI_WORD_SET(8), 0), \
      .cmd_data_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, cmd_data_gpios, {}),     \
      .reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {}),           \
      .width = DT_INST_PROP(inst, width),                                      \
      .height = DT_INST_PROP(inst, height),                                    \
      .x_offset = DT_INST_PROP(inst, x_offset),                                \
      .y_offset = DT_INST_PROP(inst, y_offset),                                \
  };                                                                           \
  static struct ssd1351_data ssd1351_data_##inst = {                           \
      .x_offset = DT_INST_PROP(inst, x_offset),                                \
      .y_offset = DT_INST_PROP(inst, y_offset),                                \
      .orientation = DISPLAY_ORIENTATION_NORMAL,                               \
  };                                                                           \
  SSD1351_PM_ACTION_DEFINE(inst);                                              \
  DEVICE_DT_INST_DEFINE(inst, &ssd1351_init, SSD1351_PM_ACTION_GET(inst),      \
                        &ssd1351_data_##inst, &ssd1351_config_##inst,          \
                        POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,             \
                        &ssd1351_api);

DT_INST_FOREACH_STATUS_OKAY(SSD1351_INIT)
