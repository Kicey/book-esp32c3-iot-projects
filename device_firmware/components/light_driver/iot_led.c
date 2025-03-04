// Copyright 2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "errno.h"

#include "math.h"
#include "esp_log.h"
#include "soc/ledc_reg.h"
#include "soc/timer_group_struct.h"
#include "soc/ledc_struct.h"
#include "driver/timer.h"
#include "driver/ledc.h"
#include "iot_led.h"

#define LEDC_FADE_MARGIN (10)
#define LEDC_TIMER_PRECISION (LEDC_TIMER_13_BIT)
#define LEDC_VALUE_TO_DUTY(value) (value * ((1 << LEDC_TIMER_PRECISION)) / (UINT16_MAX))
#define LEDC_FIXED_Q (8)
#define FLOATINT_2_FIXED(X, Q) ((int)((X)*(0x1U << Q)))
#define FIXED_2_FLOATING(X, Q) ((int)((X)/(0x1U << Q)))
#define GET_FIXED_INTEGER_PART(X, Q) (X >> Q)
#define GET_FIXED_DECIMAL_PART(X, Q) (X & ((0x1U << Q) - 1))

typedef struct {
    int cur;
    int final;
    int step;
    int cycle;
    size_t num;
} ledc_fade_data_t;

typedef struct {
    timer_group_t timer_group;
    timer_idx_t timer_id;
} hw_timer_idx_t;

typedef struct {
    ledc_fade_data_t fade_data[LEDC_CHANNEL_MAX];
    ledc_mode_t speed_mode;
    ledc_timer_t timer_num;
    hw_timer_idx_t timer_id;
} iot_light_t;

static const char *TAG = "iot_light";
static DRAM_ATTR iot_light_t *g_light_config = NULL;
static DRAM_ATTR uint16_t *g_gamma_table = NULL;
static DRAM_ATTR bool g_hw_timer_started = false;
static DRAM_ATTR timg_dev_t *TG[2] = {&TIMERG0, &TIMERG1};

static IRAM_ATTR esp_err_t _timer_pause(timer_group_t group_num, timer_idx_t timer_num)
{
    TG[group_num]->hw_timer[timer_num].config.enable = 0;
    return ESP_OK;
}

static void iot_timer_create(hw_timer_idx_t *timer_id, bool auto_reload,
                             uint32_t timer_interval_ms, void *isr_handle)
{
    /* Select and initialize basic parameters of the timer */
    timer_config_t config = {0x00};
    config.divider     = HW_TIMER_DIVIDER;
    config.counter_dir = TIMER_COUNT_UP;
    config.counter_en  = TIMER_PAUSE;
    config.alarm_en    = TIMER_ALARM_EN;
    config.intr_type   = TIMER_INTR_LEVEL;
    config.auto_reload = auto_reload;
#if CONFIG_IDF_TARGET_ESP32C3
    config.clk_src = TIMER_SRC_CLK_APB;
#endif
    timer_init(timer_id->timer_group, timer_id->timer_id, &config);

    /* Timer's counter will initially start from value below.
       Also, if auto_reload is set, this value will be automatically reload on alarm */
    timer_set_counter_value(timer_id->timer_group, timer_id->timer_id, 0x00000000ULL);

    /* Configure the alarm value and the interrupt on alarm. */
    timer_set_alarm_value(timer_id->timer_group, timer_id->timer_id, timer_interval_ms * HW_TIMER_SCALE / 1000);
    timer_enable_intr(timer_id->timer_group, timer_id->timer_id);
    timer_isr_register(timer_id->timer_group, timer_id->timer_id, isr_handle,
                       (void *) timer_id->timer_id, ESP_INTR_FLAG_IRAM, NULL);
}

static void iot_timer_start(hw_timer_idx_t *timer_id)
{
    timer_start(timer_id->timer_group, timer_id->timer_id);
    g_hw_timer_started = true;
}

static IRAM_ATTR void iot_timer_stop(hw_timer_idx_t *timer_id)
{
    _timer_pause(timer_id->timer_group, timer_id->timer_id);
    g_hw_timer_started = false;
}

static IRAM_ATTR esp_err_t iot_ledc_duty_config(ledc_mode_t speed_mode, ledc_channel_t channel, int hpoint_val, int duty_val,
        uint32_t duty_direction, uint32_t duty_num, uint32_t duty_cycle, uint32_t duty_scale)
{
    if (hpoint_val >= 0) {
        LEDC.channel_group[speed_mode].channel[channel].hpoint.hpoint = hpoint_val & LEDC_HPOINT_LSCH1_V;
    }

    if (duty_val >= 0) {
        LEDC.channel_group[speed_mode].channel[channel].duty.duty = duty_val;
    }

    LEDC.channel_group[speed_mode].channel[channel].conf1.val = ((duty_direction & LEDC_DUTY_INC_LSCH0_V) << LEDC_DUTY_INC_LSCH0_S) |
            ((duty_num & LEDC_DUTY_NUM_LSCH0_V) << LEDC_DUTY_NUM_LSCH0_S) |
            ((duty_cycle & LEDC_DUTY_CYCLE_LSCH0_V) << LEDC_DUTY_CYCLE_LSCH0_S) |
            ((duty_scale & LEDC_DUTY_SCALE_LSCH0_V) << LEDC_DUTY_SCALE_LSCH0_S);

    LEDC.channel_group[speed_mode].channel[channel].conf0.sig_out_en = 1;
    LEDC.channel_group[speed_mode].channel[channel].conf1.duty_start = 1;

    if (speed_mode == LEDC_LOW_SPEED_MODE) {
        LEDC.channel_group[speed_mode].channel[channel].conf0.low_speed_update = 1;
    }

    return ESP_OK;
}

static IRAM_ATTR esp_err_t _iot_set_fade_with_step(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t target_duty, int scale, int cycle_num)
{
    uint32_t duty_cur = LEDC.channel_group[speed_mode].channel[channel].duty_rd.duty_read >> 4;
    int step_num = 0;
    int dir = LEDC_DUTY_DIR_DECREASE;

    if (scale > 0) {
        if (duty_cur > target_duty) {
            step_num = (duty_cur - target_duty) / scale;
            step_num = step_num > 1023 ? 1023 : step_num;
            scale = (step_num == 1023) ? (duty_cur - target_duty) / step_num : scale;
        } else {
            dir = LEDC_DUTY_DIR_INCREASE;
            step_num = (target_duty - duty_cur) / scale;
            step_num = step_num > 1023 ? 1023 : step_num;
            scale = (step_num == 1023) ? (target_duty - duty_cur) / step_num : scale;
        }
    }

    if (scale > 0 && step_num > 0) {
        iot_ledc_duty_config(speed_mode, channel, -1, duty_cur << 4, dir, step_num, cycle_num, scale);
    } else {
        iot_ledc_duty_config(speed_mode, channel, -1, target_duty << 4, dir, 0, 1, 0);
    }

    return ESP_OK;
}

static IRAM_ATTR esp_err_t _iot_set_fade_with_time(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t target_duty, int max_fade_time_ms)
{
    uint32_t freq = 0;
    uint32_t duty_cur = LEDC.channel_group[speed_mode].channel[channel].duty_rd.duty_read >> 4;
    uint32_t duty_delta = target_duty > duty_cur ? target_duty - duty_cur : duty_cur - target_duty;

    uint32_t timer_source_clk = LEDC.timer_group[speed_mode].timer[g_light_config->timer_num].conf.tick_sel;
    uint32_t duty_resolution = LEDC.timer_group[speed_mode].timer[g_light_config->timer_num].conf.duty_resolution;
    uint32_t clock_divider = LEDC.timer_group[speed_mode].timer[g_light_config->timer_num].conf.clock_divider;
    uint32_t precision = (0x1U << duty_resolution);

    if (timer_source_clk == LEDC_APB_CLK) {
        freq = ((uint64_t)LEDC_APB_CLK_HZ << 8) / precision / clock_divider;
    } else {
        freq = ((uint64_t)LEDC_REF_CLK_HZ << 8) / precision / clock_divider;
    }

    if (duty_delta == 0) {
        return _iot_set_fade_with_step(speed_mode, channel, target_duty, 0, 0);
    }

    int total_cycles = max_fade_time_ms * freq / 1000;

    if (total_cycles == 0) {
        return _iot_set_fade_with_step(speed_mode, channel, target_duty, 0, 0);
    }

    int scale, cycle_num;

    if (total_cycles > duty_delta) {
        scale = 1;
        cycle_num = total_cycles / duty_delta;

        if (cycle_num > LEDC_DUTY_NUM_LSCH0_V) {
            cycle_num = LEDC_DUTY_NUM_LSCH0_V;
        }
    } else {
        cycle_num = 1;
        scale = duty_delta / total_cycles;

        if (scale > LEDC_DUTY_SCALE_LSCH0_V) {
            scale = LEDC_DUTY_SCALE_LSCH0_V;
        }
    }

    return _iot_set_fade_with_step(speed_mode, channel, target_duty, scale, cycle_num);
}

static IRAM_ATTR esp_err_t _iot_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel)
{
    LEDC.channel_group[speed_mode].channel[channel].conf0.sig_out_en = 1;
    LEDC.channel_group[speed_mode].channel[channel].conf1.duty_start = 1;

    if (speed_mode == LEDC_LOW_SPEED_MODE) {
        LEDC.channel_group[speed_mode].channel[channel].conf0.low_speed_update = 1;
    }

    return ESP_OK;
}

static IRAM_ATTR esp_err_t iot_ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty)
{
    return iot_ledc_duty_config(speed_mode,
                                channel,         // uint32_t chan_num,
                                -1,
                                duty << 4,       // uint32_t duty_val,the least 4 bits are decimal part
                                1,               // uint32_t increase,
                                1,               // uint32_t duty_num,
                                1,               // uint32_t duty_cycle,
                                0                // uint32_t duty_scale
                               );
}

static void gamma_table_create(uint16_t *gamma_table, float correction)
{
    float value_tmp = 0;

    /**
     * @brief gamma curve formula: y=a*x^(1/gm)
     * x ∈ (0,(GAMMA_TABLE_SIZE-1)/GAMMA_TABLE_SIZE)
     * a = GAMMA_TABLE_SIZE
     */
    for (int i = 0; i < GAMMA_TABLE_SIZE; i++) {
        value_tmp = (float)(i) / (GAMMA_TABLE_SIZE - 1);
        value_tmp = powf(value_tmp, 1.0f / correction);
        gamma_table[i] = (uint16_t)FLOATINT_2_FIXED((value_tmp * GAMMA_TABLE_SIZE), LEDC_FIXED_Q);
    }

    if (gamma_table[255] == 0) {
        gamma_table[255] = __UINT16_MAX__;
    }
}

static IRAM_ATTR uint32_t gamma_value_to_duty(int value)
{
    uint32_t tmp_q = GET_FIXED_INTEGER_PART(value, LEDC_FIXED_Q);
    uint32_t tmp_r = GET_FIXED_DECIMAL_PART(value, LEDC_FIXED_Q);

    uint16_t cur = LEDC_VALUE_TO_DUTY(g_gamma_table[tmp_q]);
    uint16_t next = tmp_q < (GAMMA_TABLE_SIZE - 1) ? LEDC_VALUE_TO_DUTY(g_gamma_table[tmp_q + 1]) : cur;
    uint32_t tmp = (cur + (next - cur) * tmp_r / (0x1U << LEDC_FIXED_Q));
    return tmp;
}

static IRAM_ATTR void fade_timercb(void *para)
{
    int timer_idx = (int) para;
    int idle_channel_num = 0;

    if (HW_TIMER_GROUP == TIMER_GROUP_0) {
        /* Retrieve the interrupt status */
#if CONFIG_IDF_TARGET_ESP32
        uint32_t intr_status = TIMERG0.int_st_timers.val;
        TIMERG0.hw_timer[timer_idx].update = 1;
#elif CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32C3
        uint32_t intr_status = TIMERG0.int_st.val;
        TIMERG0.hw_timer[timer_idx].update.val = 1;
#endif

        /* Clear the interrupt */
        if ((intr_status & BIT(timer_idx)) && timer_idx == TIMER_0) {
#if CONFIG_IDF_TARGET_ESP32
            TIMERG0.int_clr_timers.t0 = 1;
#elif CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32C3
            TIMERG0.int_clr.t0 = 1;
#endif
        } 
#ifndef CONFIG_IDF_TARGET_ESP32C3
        else if ((intr_status & BIT(timer_idx)) && timer_idx == TIMER_1) {
#if CONFIG_IDF_TARGET_ESP32
            TIMERG0.int_clr_timers.t1 = 1;
#elif CONFIG_IDF_TARGET_ESP32S2
            TIMERG0.int_clr.t1 = 1;
#endif
        }
#endif

        /* After the alarm has been triggered
          we need enable it again, so it is triggered the next time */
        TIMERG0.hw_timer[timer_idx].config.alarm_en = TIMER_ALARM_EN;
    } 
#ifndef CONFIG_IDF_TARGET_ESP32C3
    else if (HW_TIMER_GROUP == TIMER_GROUP_1) {
#if CONFIG_IDF_TARGET_ESP32
        uint32_t intr_status = TIMERG1.int_st_timers.val;
        TIMERG1.hw_timer[timer_idx].update = 1;
#elif CONFIG_IDF_TARGET_ESP32S2
        uint32_t intr_status = TIMERG1.int_st.val;
        TIMERG1.hw_timer[timer_idx].update.val = 1;
#endif

        if ((intr_status & BIT(timer_idx)) && timer_idx == TIMER_0) {
#if CONFIG_IDF_TARGET_ESP32
            TIMERG1.int_clr_timers.t0 = 1;
#elif CONFIG_IDF_TARGET_ESP32S2
            TIMERG1.int_clr.t0 = 1;
#endif
        } else if ((intr_status & BIT(timer_idx)) && timer_idx == TIMER_1) {
#if CONFIG_IDF_TARGET_ESP32
            TIMERG1.int_clr_timers.t1 = 1;
#elif CONFIG_IDF_TARGET_ESP32S2
            TIMERG1.int_clr.t1 = 1;
#endif
        }

        TIMERG1.hw_timer[timer_idx].config.alarm_en = TIMER_ALARM_EN;
    }
#endif

    for (int channel = 0; channel < LEDC_CHANNEL_MAX; channel++) {
        ledc_fade_data_t *fade_data = g_light_config->fade_data + channel;

        if (fade_data->num > 0) {
            fade_data->num--;

            if (fade_data->step) {
                fade_data->cur += fade_data->step;

                if (fade_data->num != 0) {
                    _iot_set_fade_with_time(g_light_config->speed_mode, channel,
                                            gamma_value_to_duty(fade_data->cur),
                                            DUTY_SET_CYCLE - LEDC_FADE_MARGIN);
                } else {
                    iot_ledc_set_duty(g_light_config->speed_mode, channel, gamma_value_to_duty(fade_data->cur));
                }

                _iot_update_duty(g_light_config->speed_mode, channel);
            } else {
                iot_ledc_set_duty(g_light_config->speed_mode, channel, gamma_value_to_duty(fade_data->cur));
                _iot_update_duty(g_light_config->speed_mode, channel);
            }
        } else if (fade_data->cycle) {
            fade_data->num = fade_data->cycle - 1;

            if (fade_data->step) {
                fade_data->step *= -1;
                fade_data->cur  += fade_data->step;
            } else {
                fade_data->cur = (fade_data->cur == fade_data->final) ? 0 : fade_data->final;
            }

            _iot_set_fade_with_time(g_light_config->speed_mode, channel,
                                    gamma_value_to_duty(fade_data->cur),
                                    DUTY_SET_CYCLE - LEDC_FADE_MARGIN);
            _iot_update_duty(g_light_config->speed_mode, channel);

        } else {
            idle_channel_num++;
        }
    }

    if (idle_channel_num >= LEDC_CHANNEL_MAX) {
        iot_timer_stop(&g_light_config->timer_id);
    }
}

esp_err_t iot_led_init(ledc_timer_t timer_num, ledc_mode_t speed_mode, uint32_t freq_hz, ledc_clk_cfg_t clk_cfg, ledc_timer_bit_t duty_resolution)
{
    esp_err_t ret = ESP_OK;
    const ledc_timer_config_t ledc_time_config = {
        .speed_mode      = speed_mode,
        .duty_resolution = duty_resolution,
        .timer_num       = timer_num,
        .freq_hz         = freq_hz,
        .clk_cfg         = clk_cfg,
    };

    ret = ledc_timer_config(&ledc_time_config);
    LIGHT_ERROR_CHECK(ret != ESP_OK, ret, "LEDC timer configuration");

    if (g_gamma_table == NULL) {
        /* g_gamma_table[GAMMA_TABLE_SIZE] must be 0 */
        g_gamma_table = calloc(GAMMA_TABLE_SIZE + 1, sizeof(uint16_t));
        gamma_table_create(g_gamma_table, GAMMA_CORRECTION);
    } else {
        ESP_LOGE(TAG, "gamma_table has been initialized");
    }

    if (g_light_config == NULL) {
        g_light_config = calloc(1, sizeof(iot_light_t));
        g_light_config->timer_num  = timer_num;
        g_light_config->speed_mode = speed_mode;


        hw_timer_idx_t hw_timer = {
            .timer_group = HW_TIMER_GROUP,
            .timer_id    = HW_TIMER_ID,
        };
        g_light_config->timer_id = hw_timer;
        iot_timer_create(&hw_timer, 1, DUTY_SET_CYCLE, fade_timercb);
    } else {
        ESP_LOGE(TAG, "g_light_config has been initialized");
    }

    return ESP_OK;
}

esp_err_t iot_led_deinit()
{
    if (g_gamma_table) {
        free(g_gamma_table);
    }

    if (g_light_config) {
        free(g_light_config);
    }

    timer_disable_intr(g_light_config->timer_id.timer_group, g_light_config->timer_id.timer_id);

    return ESP_OK;
}

esp_err_t iot_led_regist_channel(ledc_channel_t channel, gpio_num_t gpio_num)
{
    esp_err_t ret = ESP_OK;
    LIGHT_ERROR_CHECK(g_light_config == NULL, ESP_ERR_INVALID_ARG, "iot_led_init() must be called first");
#ifdef CONFIG_SPIRAM_SUPPORT
    LIGHT_ERROR_CHECK(gpio_num != GPIO_NUM_16 || gpio_num != GPIO_NUM_17, ESP_ERR_INVALID_ARG,
                    "gpio_num must not conflict to PSRAM(IO16 && IO17)");
#endif
    const ledc_channel_config_t ledc_ch_config = {
        .gpio_num   = gpio_num,
        .channel    = channel,
        .intr_type  = LEDC_INTR_DISABLE,
        .speed_mode = g_light_config->speed_mode,
        .timer_sel  = g_light_config->timer_num,
    };

    ret = ledc_channel_config(&ledc_ch_config);
    LIGHT_ERROR_CHECK(ret != ESP_OK, ret, "LEDC channel configuration");

    return ESP_OK;
}

esp_err_t iot_led_get_channel(ledc_channel_t channel, uint8_t *dst)
{
    LIGHT_ERROR_CHECK(g_light_config == NULL, ESP_ERR_INVALID_ARG, "iot_led_init() must be called first");
    LIGHT_ERROR_CHECK(dst == NULL, ESP_ERR_INVALID_ARG, "dst should not be NULL");
    int cur = g_light_config->fade_data[channel].cur;
    *dst = FIXED_2_FLOATING(cur, LEDC_FIXED_Q);
    return ESP_OK;
}

esp_err_t iot_led_set_channel(ledc_channel_t channel, uint8_t value, uint32_t fade_ms)
{
    LIGHT_ERROR_CHECK(g_light_config == NULL, ESP_ERR_INVALID_ARG, "iot_led_init() must be called first");
    ledc_fade_data_t *fade_data = g_light_config->fade_data + channel;

    fade_data->final = FLOATINT_2_FIXED(value, LEDC_FIXED_Q);

    if (fade_ms < DUTY_SET_CYCLE) {
        fade_data->num = 1;
    } else {
        fade_data->num   = fade_ms / DUTY_SET_CYCLE;
    }

    fade_data->step  = abs(fade_data->cur - fade_data->final) / fade_data->num;

    if (fade_data->cur > fade_data->final) {
        fade_data->step *= -1;
    }

    if (fade_data->cycle != 0) {
        fade_data->cycle = 0;
    }

    if (g_hw_timer_started != true) {
        iot_timer_start(&g_light_config->timer_id);
    }

    return ESP_OK;
}

esp_err_t iot_led_start_blink(ledc_channel_t channel, uint8_t value, uint32_t period_ms, bool fade_flag)
{
    LIGHT_ERROR_CHECK(g_light_config == NULL, ESP_ERR_INVALID_ARG, "iot_led_init() must be called first");
    ledc_fade_data_t *fade_data = g_light_config->fade_data + channel;

    fade_data->final = fade_data->cur = FLOATINT_2_FIXED(value, LEDC_FIXED_Q);
    fade_data->cycle = period_ms / 2 / DUTY_SET_CYCLE;
    fade_data->num = (fade_flag) ? period_ms / 2 / DUTY_SET_CYCLE : 0;
    fade_data->step  = (fade_flag) ? fade_data->cur / fade_data->num * -1 : 0;

    if (g_hw_timer_started != true) {
        iot_timer_start(&g_light_config->timer_id);
    }

    return ESP_OK;

}

esp_err_t iot_led_stop_blink(ledc_channel_t channel)
{
    LIGHT_ERROR_CHECK(g_light_config == NULL, ESP_ERR_INVALID_ARG, "iot_led_init() must be called first");
    ledc_fade_data_t *fade_data = g_light_config->fade_data + channel;
    fade_data->cycle = fade_data->num = 0;

    return ESP_OK;
}

esp_err_t iot_led_set_gamma_table(const uint16_t gamma_table[GAMMA_TABLE_SIZE])
{
    LIGHT_ERROR_CHECK(g_gamma_table == NULL, ESP_ERR_INVALID_ARG, "iot_led_init() must be called first");
    memcpy(g_gamma_table, gamma_table, GAMMA_TABLE_SIZE * sizeof(uint16_t));
    return ESP_OK;
}
