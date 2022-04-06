// Copyright (c) 2022 XMOS LIMITED. This Software is subject to the terms of the
// XMOS Public License: Version 1

/* STD headers */
#include <platform.h>
#include <xs1.h>
#include <xcore/hwtimer.h>

/* FreeRTOS headers */
#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

/* App headers */
#include "app_conf.h"
#include "platform/driver_instances.h"
#include "keyword_inference.h"
#include "inference_hmi/inference_hmi.h"

#define KEYWORD_DETECT_DEBOUNCE       3
#define KEYWORD_DETECT_UNKNOWN_RESET  10

typedef enum inference_state {
    STATE_WAIT_FOR_ANY,
    STATE_WAIT_FOR_ACTION,
    STATE_WAIT_FOR_OBJECT,
} inference_state_t;

rtos_gpio_port_id_t gpo_port = 0;

#if XK_VOICE_L71
#define gpo_setup()     {                                       \
    gpo_port = rtos_gpio_port(PORT_GPO);                        \
    rtos_gpio_port_enable(gpio_ctx_t0, gpo_port);               \
}
#define green_led_on()  {                                       \
    uint32_t val = rtos_gpio_port_in(gpio_ctx_t0, gpo_port);    \
    rtos_gpio_port_out(gpio_ctx_t0, gpo_port, val &= ~(1<<5));  \
}
#define green_led_off()  {                                      \
    uint32_t val = rtos_gpio_port_in(gpio_ctx_t0, gpo_port);    \
    rtos_gpio_port_out(gpio_ctx_t0, gpo_port, val |= (1<<5));   \
}
#define red_led_on()  {                                         \
    uint32_t val = rtos_gpio_port_in(gpio_ctx_t0, gpo_port);    \
    rtos_gpio_port_out(gpio_ctx_t0, gpo_port, val &= ~(1<<4));  \
}
#define red_led_off()  {                                        \
    uint32_t val = rtos_gpio_port_in(gpio_ctx_t0, gpo_port);    \
    rtos_gpio_port_out(gpio_ctx_t0, gpo_port, val |= (1<<4));   \
}

#elif XCOREAI_EXPLORER
#define gpo_setup()     {                               \
    gpio_port = rtos_gpio_port(PORT_LEDS);              \
    rtos_gpio_port_enable(gpio_ctx_t0, gpio_port);      \
}
#define green_led_on()  {rtos_printf("Green on\n");}
#define green_led_off() {rtos_printf("Green off\n");}
#define red_led_on()    {rtos_printf("Red on\n");}
#define red_led_off()   {rtos_printf("Red off\n");}
#endif

void inference_hmi_task(void *args)
{
    EventGroupHandle_t egrp_intent = (EventGroupHandle_t) args;
    EventBits_t rx_bits = 0;
    EventBits_t last_rx_bits = 0;
    EventBits_t successive_cnt = 0;
    inference_state_t state = STATE_WAIT_FOR_ANY;
    inference_state_t last_state = STATE_WAIT_FOR_ANY;
    uint32_t intent = 0;

    gpo_setup();

    while(1) {
        /* Wait forever for a bit change.  Clear changed bit on exit */
        rx_bits = xEventGroupWaitBits(
                egrp_intent,
                INFERENCE_BIT_ALL,
                pdTRUE,
                pdFALSE,
                portMAX_DELAY);

        /* If there have been KEYWORD_DETECT_DEBOUNCE successive
         * keywords spotted, assume it to be valid and run the state machine */
        if (rx_bits == last_rx_bits) {
            successive_cnt++;
        } else {
            successive_cnt = 0;
        }
        last_rx_bits = rx_bits;

        if ((rx_bits & INFERENCE_BIT_SPOTTED_UNKNOWN) != 0) {
            if (successive_cnt >= KEYWORD_DETECT_UNKNOWN_RESET) {
                intent = 0;
                state = STATE_WAIT_FOR_ANY;
                rtos_printf("Unknown threshold passed.  Reset intent state\n");
            }
        } else {
            /* Only run the state machine once per non-unknown keyword */
            if (successive_cnt == KEYWORD_DETECT_DEBOUNCE) {
                last_state = state;
                switch (state) {
                    default:
                    case STATE_WAIT_FOR_ANY:
                        if ( ((rx_bits & INFERENCE_BIT_ACTIVATE) != 0)
                          || ((rx_bits & INFERENCE_BIT_DEACTIVATE) != 0) ) {
                              state = STATE_WAIT_FOR_OBJECT;
                              intent |= rx_bits;
                              rtos_printf("Found object wait for action\n");
                        } else if ( ((rx_bits & INFERENCE_BIT_GREEN) != 0)
                                 || ((rx_bits & INFERENCE_BIT_RED) != 0) ) {
                              state = STATE_WAIT_FOR_ACTION;
                              intent |= rx_bits;
                              rtos_printf("Found action wait for object\n");
                        }
                        break;
                    case STATE_WAIT_FOR_ACTION:
                        if ( ((rx_bits & INFERENCE_BIT_ACTIVATE) != 0)
                          || ((rx_bits & INFERENCE_BIT_DEACTIVATE) != 0) ) {
                          intent |= rx_bits;
                        } else {
                            rtos_printf("Action with no object.  Reset intent state\n");
                            intent = 0;
                        }
                        state = STATE_WAIT_FOR_ANY;
                        break;
                    case STATE_WAIT_FOR_OBJECT:
                        if ( ((rx_bits & INFERENCE_BIT_GREEN) != 0)
                          || ((rx_bits & INFERENCE_BIT_RED) != 0) ) {
                          intent |= rx_bits;
                        } else {
                            rtos_printf("Object with no action.  Reset intent state\n");
                            intent = 0;
                        }
                        state = STATE_WAIT_FOR_ANY;
                        break;
                }

                /* Process intent */
                switch (intent) {
                    default:
                        break;
                    case (INFERENCE_BIT_GREEN | INFERENCE_BIT_ACTIVATE):
                        green_led_on();
                        rtos_printf("Intent is GREEN ON\n");
                        intent = 0;
                        break;
                    case (INFERENCE_BIT_GREEN | INFERENCE_BIT_DEACTIVATE):
                        green_led_off();
                        rtos_printf("Intent is GREEN OFF\n");
                        intent = 0;
                        break;
                    case (INFERENCE_BIT_RED | INFERENCE_BIT_ACTIVATE):
                        red_led_on();
                        rtos_printf("Intent is RED ON\n");
                        intent = 0;
                        break;
                    case (INFERENCE_BIT_RED | INFERENCE_BIT_DEACTIVATE):
                        red_led_off();
                        rtos_printf("Intent is RED OFF\n");
                        intent = 0;
                        break;
                }
            }
        }
    }
}


void inference_hmi_create(unsigned priority, void *args)
{
    xTaskCreate((TaskFunction_t) inference_hmi_task,
                "inf_hmi",
                RTOS_THREAD_STACK_SIZE(inference_hmi_task),
                args,
                priority,
                NULL);
}
