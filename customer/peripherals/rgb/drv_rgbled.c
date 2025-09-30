/**
  ******************************************************************************
  * @file   drv_pwm.c
  * @author Sifli software development team
  * @brief PWM BSP driver
  * @{
  ******************************************************************************
*/
/**
 * @attention
 * Copyright (c) 2019 - 2022,  Sifli Technology
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Sifli integrated circuit
 *    in a product or a software update for such product, must reproduce the above
 *    copyright notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Sifli nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Sifli integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY SIFLI TECHNOLOGY "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SIFLI TECHNOLOGY OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <board.h>
#include "bf0_hal_rcc.h"
#include "drv_rgbled.h"

/** @addtogroup bsp_driver Driver IO
  * @{
  */

/** @defgroup drv_rgbled RGBLED driver
  * @ingroup bsp_driver
  * @brief RGBLED BSP driver
  * @{
  */

#if defined(RGB_SK6812MINI_HS_ENABLE) || defined(_SIFLI_DOXYGEN_)

#include "drv_config.h"
#include "mem_section.h"

//#define DRV_DEBUG
#define LOG_TAG             "drv.rgb"
#include <drv_log.h>

#define RGB_COLOR_BITS_PER_LED   24
#define RGB_REST_LEN   300
#define RGB_STOP_LEN   50

// Calculate total buffer size based on configured LED count
#define RGB_TOTAL_COLOR_LEN   (RGB_COLOR_BITS_PER_LED * BSP_RGB_LED_COUNT)
#define RGB_TOTAL_BUFFER_LEN  (RGB_REST_LEN + RGB_TOTAL_COLOR_LEN + RGB_STOP_LEN)

/* PWM timing parameters for SK6812 protocol
 * Based on 48MHz timer clock with prescaler resulting in 30MHz counter
 * Period = 1600 ticks = 53.3μs (close to SK6812's 1.25μs ± 0.6μs requirement)
 */
#define pwm_period  1600        /*!< PWM period in timer ticks */
#define pulse_period  800       /*!< PWM pulse width in timer ticks (50% duty cycle base) */

/* SK6812 protocol timing values (PWM compare register values)
 * reg_high: ~0.8μs high time for logic '1' (18/30MHz = 0.6μs + rise time)
 * reg_low:  ~0.4μs high time for logic '0' (7/30MHz = 0.23μs + rise time)
 * reg_end:  Long high pulse for reset/latch signal (50/30MHz = 1.67μs)
 */
#define reg_high 18    /*!< PWM compare value for logic '1' */
#define reg_low  7     /*!< PWM compare value for logic '0' */
#define reg_end  50    /*!< PWM compare value for end/reset signal */

#ifdef RGB_USING_SK6812MINI_HS_DEV_NAME
    #define RGBLED_NAME "rgbled"
#endif

struct bf0_rgbled
{
    struct rt_device device;                      /*!< RT-Thread device base */
    struct rt_device_pwm *pwm_device;             /*!< PWM device handle */
    char *name;                                   /*!< Device name */
    rt_uint16_t max_led_count;                    /*!< Maximum supported LED count */
    uint16_t rgb_buffer[RGB_TOTAL_BUFFER_LEN];    /*!< Static RGB buffer */
};

#ifdef RGB_USING_SK6812MINI_HS_DEV_NAME
static struct bf0_rgbled bf0_rgbled_obj =
{
    .name = RGBLED_NAME,
    .max_led_count = BSP_RGB_LED_COUNT,
    .rgb_buffer = {0},                            // Static buffer initialized to 0
    .pwm_device = RT_NULL
};
#endif

static rt_err_t drv_rgbled_control(rt_device_t device, int cmd, void *arg);

/**
 * @brief Create color array for LEDs
 * @param rgb_obj RGB LED device object
 * @param colors Array of RGB color values
 * @param led_count Number of LEDs
 */
static void create_color_array(struct bf0_rgbled *rgb_obj, rt_uint32_t *colors, rt_uint16_t led_count)
{
    if (led_count > rgb_obj->max_led_count)
    {
        LOG_E("LED count %d exceeds maximum %d", led_count, rgb_obj->max_led_count);
        led_count = rgb_obj->max_led_count;
    }

    rt_memset(rgb_obj->rgb_buffer, 0, sizeof(rgb_obj->rgb_buffer));

    uint16_t offset = RGB_REST_LEN;

    for (int led = 0; led < led_count; led++)
    {
        uint32_t color = colors[led];
        uint8_t red = (uint8_t)((color & 0x00ff0000) >> 16);
        uint8_t green = (uint8_t)((color & 0x0000ff00) >> 8);
        uint8_t blue = (uint8_t)((color & 0x000000ff) >> 0);

        uint16_t led_offset = offset + (led * RGB_COLOR_BITS_PER_LED);

        // Optimized: Encode all three colors in one loop (GRB order for SK6812)
        for (int i = 0; i < 8; i++)
        {
            // Encode Green (GRB order for SK6812)
            rgb_obj->rgb_buffer[led_offset + i] = ((green << i) & 0x80) ? reg_high : reg_low;
            // Encode Red
            rgb_obj->rgb_buffer[led_offset + i + 8] = ((red << i) & 0x80) ? reg_high : reg_low;
            // Encode Blue
            rgb_obj->rgb_buffer[led_offset + i + 16] = ((blue << i) & 0x80) ? reg_high : reg_low;
        }
    }

    // Add stop signal after all LEDs
    rgb_obj->rgb_buffer[RGB_REST_LEN + (led_count * RGB_COLOR_BITS_PER_LED)] = reg_end;

#ifdef DRV_DEBUG
    LOG_D("Color array created for %d LEDs", led_count);
#endif
}

/**
* @brief  Send RGB data to LEDs
* @param[in]  rgb_obj: RGB LED object handle.
* @param[in]  led_count: Number of LEDs to update.
* @retval RT_EOK if success, otherwise -RT_ERROR
*/
static rt_err_t drv_rgbled_send_data(struct bf0_rgbled *rgb_obj, rt_uint16_t led_count)
{
    struct rt_pwm_configuration config;
    rt_err_t result;

    rt_memset((void *)&config, 0, sizeof(config));

#ifdef BSP_USING_RGBLED_CH
    config.channel = BSP_USING_RGBLED_CH;
#else
    LOG_E("NO CONFIG BSP_USING_RGBLED_CH");
    return -RT_ERROR;
#endif

    config.period = pwm_period;
    config.pulse = pulse_period;
    config.dma_type = 0;
    config.dma_data = (rt_uint16_t *)rgb_obj->rgb_buffer;
    config.data_len = RGB_REST_LEN + (led_count * RGB_COLOR_BITS_PER_LED) + RGB_STOP_LEN;

    // Set PWM configuration with error checking
    result = rt_device_control((struct rt_device *)rgb_obj->pwm_device, PWM_CMD_SET, (void *)&config);
    if (result != RT_EOK)
    {
        LOG_E("PWM_CMD_SET failed with error %d", result);
        return result;
    }

    // Enable PWM output with error checking
    result = rt_device_control((struct rt_device *)rgb_obj->pwm_device, PWM_CMD_ENABLE, (void *)&config);
    if (result != RT_EOK)
    {
        LOG_E("PWM_CMD_ENABLE failed with error %d", result);
        return result;
    }

    return RT_EOK;
}

/**
* @brief  rgbled controls.
* @param[in]  device: rgb device handle.
* @param[in]  cmd: control commands.
* @param[in]  arg: control command arguments.
* @retval RT_EOK if success, otherwise -RT_ERROR
*/
static rt_err_t drv_rgbled_control(rt_device_t device, int cmd, void *arg)
{
    struct bf0_rgbled *rgb_obj = (struct bf0_rgbled *) device->user_data;

    switch (cmd)
    {
    case RGB_CMD_SET_SINGLE_COLOR:
    case PWM_CMD_SET_COLOR:
    {
        struct rt_rgbled_configuration *single_config = (struct rt_rgbled_configuration *)arg;
        if (!single_config)
        {
            LOG_E("Invalid single color configuration");
            return -RT_ERROR;
        }

        rt_uint32_t color = single_config->color_rgb;

        create_color_array(rgb_obj, &color, 1);
        return drv_rgbled_send_data(rgb_obj, 1);
    }
    case RGB_CMD_SET_MULTI_COLOR:
    {
        struct rt_rgbled_multi_configuration *multi_config = (struct rt_rgbled_multi_configuration *)arg;
        if (!multi_config || !multi_config->color_array || multi_config->led_count == 0)
        {
            LOG_E("Invalid multi-color configuration");
            return -RT_ERROR;
        }

        create_color_array(rgb_obj, multi_config->color_array, multi_config->led_count);
        return drv_rgbled_send_data(rgb_obj, multi_config->led_count);
    }
    case RGB_CMD_GET_CAPABILITY:
    {
        rt_uint32_t *led_count = (rt_uint32_t *)arg;
        if (!led_count)
        {
            LOG_E("Invalid capability query parameter");
            return -RT_ERROR;
        }

        // Return maximum LED count
        *led_count = BSP_RGB_LED_COUNT;
        return RT_EOK;
    }
    default:
        LOG_W("Unknown command: %d", cmd);
        return -RT_EINVAL;
    }
}

/**
* @brief RGBLED device driver initialization.
* This is entry function of RGBLED device driver.
* @retval RT_EOK if success, otherwise -RT_ERROR
*/
static int bf0_rgbled_init(void)
{
    int result = RT_EOK;

#ifdef RGB_USING_SK6812MINI_HS_PWM_DEV_NAME
    // Find PWM device
    bf0_rgbled_obj.pwm_device = (struct rt_device_pwm *)rt_device_find(RGB_USING_SK6812MINI_HS_PWM_DEV_NAME);
    if (!bf0_rgbled_obj.pwm_device)
    {
        LOG_E("find pwm device failed");
        return -RT_ERROR;
    }
#else
    LOG_E("NO CONFIG RGB_USING_SK6812MINI_HS_PWM_DEV_NAME");
    return -RT_ERROR;
#endif

    // Register RGB device
    bf0_rgbled_obj.device.control = (rt_err_t (*)(rt_device_t, int, void *))drv_rgbled_control;
    bf0_rgbled_obj.device.user_data = &bf0_rgbled_obj;

    if (rt_device_register(&bf0_rgbled_obj.device, bf0_rgbled_obj.name, RT_DEVICE_FLAG_RDWR))
    {
        LOG_E("%s register failed", RGBLED_NAME);
        result = -RT_ERROR;
    }
    else
    {
        LOG_I("%s register success, supporting %d LEDs", RGBLED_NAME, BSP_RGB_LED_COUNT);
    }

    return result;
}
INIT_COMPONENT_EXPORT(bf0_rgbled_init);

// @} drv_rgbled
// @} bsp_driver

#endif /* RGB_SK6812MINI_HS_ENABLE */

/// @} file
/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/
