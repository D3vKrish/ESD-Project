#include "i2c.h"
#include "spi.h"
#include <stdio.h>
#include <string.h>

#define RCC_AHB1ENR_GPIOAENB     (1U << 0)
#define RCC_AHB1ENR_GPIOBENB     (1U << 1)
#define RCC_AHB1ENR_GPIOCENB     (1U << 2)
#define RCC_APB1ENR_TIM2ENB      (1U << 0)
#define RCC_APB1ENR_TIM3ENB      (1U << 1)
#define RCC_APB1ENR_USART2ENB    (1U << 17)


#define TIM_CR1_CENB         (1U << 0)
#define TIM_CR1_ARPEB        (1U << 7)
#define TIM_EGR_UGB          (1U << 0)
#define TIM_CCMR1_OC1PEB     (1U << 3)
#define TIM_CCMR1_OC1M_PWM1B (3U << 5)
#define TIM_CCER_CC1EB       (1U << 0)

#define USART_CR1_UEB        (1U << 13)
#define USART_CR1_TEB        (1U << 3)
#define USART_SR_TXEB        (1U << 7)


// Functions
static void timer_init(void);
static void pwm_init(void);
static void pwm_set_duty(uint16_t duty);
static void button_init(void);
__attribute__((unused)) static int button_is_pressed(void);
static uint16_t pid_update(int32_t measured_ua, int32_t target_ua, float dt);
static void uart_init(void);
static void uart_print(const char *s);

