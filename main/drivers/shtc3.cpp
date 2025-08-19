/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "esp_check.h"

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lib/support/CodeUtils.h>

#include "scd30.h"
#include <drivers/shtc3.h>
#include <esp_check.h>

static const char * TAG = "shtc3";

#define I2C_MASTER_SCL_IO           GPIO_NUM_1
#define I2C_MASTER_SDA_IO           GPIO_NUM_0
#define I2C_MASTER_NUM I2C_NUM_0    /*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ 100000   /*!< I2C master clock frequency */

#define SCD30_SENSOR_ADDR           0x61

typedef struct {
    scd30_sensor_config_t *config;
    esp_timer_handle_t timer;
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    bool is_initialized = false;
} scd30_sensor_ctx_t;

static scd30_sensor_ctx_t s_ctx;

static esp_err_t scd30_init_i2c(i2c_master_bus_handle_t *bus_handle, i2c_master_dev_handle_t *dev_handle)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true
        },
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, bus_handle);
    if (err != ESP_OK)
        return err;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SCD30_SENSOR_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
      };

    return i2c_master_bus_add_device(*bus_handle, &dev_config, dev_handle);
}

static void timer_cb_internal(void *arg)
{
    auto *ctx = (scd30_sensor_ctx_t *) arg;
    if (!(ctx && ctx->config)) {
        return;
    }

    bool data_ready;
    scd30_get_data_ready_status(s_ctx.dev_handle, &data_ready);

    if (!data_ready)
        return;

    float co2, temp, humidity;
    esp_err_t res = scd30_read_measurement(s_ctx.dev_handle, &co2, &temp, &humidity);
    if (res != ESP_OK)
    {
        ESP_LOGE(TAG, "Error reading results %d (%s)", res, esp_err_to_name(res));
        return;
    }

    ESP_LOGI(TAG, "CO2: %f", co2);
    ESP_LOGI(TAG, "temp: %f", temp);
    ESP_LOGI(TAG, "humidity: %f", humidity);

    if (ctx->config->air_quality.cb) {
        ctx->config->air_quality.cb(ctx->config->air_quality.endpoint_id, co2, ctx->config->user_data);
    }
    if (ctx->config->temperature.cb) {
        ctx->config->temperature.cb(ctx->config->temperature.endpoint_id, temp, ctx->config->user_data);
    }
    if (ctx->config->humidity.cb) {
        ctx->config->humidity.cb(ctx->config->humidity.endpoint_id, humidity, ctx->config->user_data);
    }
}

esp_err_t scd30_sensor_init(scd30_sensor_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // we need at least one callback so that we can start notifying application layer
    if (config->air_quality.cb == NULL || config->temperature.cb == NULL || config->humidity.cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = scd30_init_i2c(&s_ctx.bus_handle, &s_ctx.dev_handle);
    if (err != ESP_OK) {
        return err;
    }

    // keep the pointer to config
    s_ctx.config = config;

    scd30_set_measurement_interval(s_ctx.dev_handle, 2);

    esp_timer_create_args_t args = {
        .callback = timer_cb_internal,
        .arg = &s_ctx,
    };

    err = esp_timer_create(&args, &s_ctx.timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed, err:%d", err);
        return err;
    }

    err = esp_timer_start_periodic(s_ctx.timer, config->interval_ms * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %d", err);
        return err;
    }

    s_ctx.is_initialized = true;
    ESP_LOGI(TAG, "shtc3 initialized successfully");
    return ESP_OK;
}
