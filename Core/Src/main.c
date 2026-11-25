#include "main.h"

//1 MHz counter
static void timer_init(void) {
    RCC->APB1ENR |= RCC_APB1ENR_TIM2ENB;
    TIM2->PSC = 16 - 1;
    TIM2->ARR = 0xFFFFFFFF;
    TIM2->EGR = TIM_EGR_UGB;
    TIM2->CR1 = TIM_CR1_CENB;
}

// 10 kHz PWM on TIM3_CH1 (AF2) 
static void pwm_init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBENB;
    RCC->APB1ENR |= RCC_APB1ENR_TIM3ENB;

    // PB4 -> AF2 (TIM3_CH1)
    GPIOB->MODER &= ~(3u << 8);
    GPIOB->MODER |= (2u << 8);
    GPIOB->AFR[0] &= ~(0xFu << 16);
    GPIOB->AFR[0]  |= (2u << 16);
    GPIOB->OSPEEDR |= (3u << 8);

    TIM3->PSC  = 0; // 16 MHz
    TIM3->ARR  = PWM_PERIOD; // 10 kHz frequency
    TIM3->CCR1 = 0;  
    TIM3->CCMR1 |= TIM_CCMR1_OC1M_PWM1B | TIM_CCMR1_OC1PEB; 
    TIM3->CCER  |= TIM_CCER_CC1EB;      
    TIM3->CR1   |= TIM_CR1_ARPEB | TIM_CR1_CENB;
}

static void pwm_set_duty(uint16_t duty) {
    if (duty > PWM_PERIOD) duty = PWM_PERIOD;
    TIM3->CCR1 = duty;
}


static void button_init(void) {
    // Set button to input and pull-up
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCENB;
    GPIOC->MODER &= ~(3u << 26);  
    GPIOC->PUPDR  = (GPIOC->PUPDR & ~(3u << 26)) | (1u << 26); 
}

__attribute__((unused)) static int button_is_pressed(void) {
    static uint8_t count = 0;
    static uint8_t state = 0;
    uint8_t raw = !(GPIOC->IDR & (1u << 13));
    if (raw != state) {
        // Debounce
        if (++count >= 2) {
            state = raw;
            count = 0;
        }
    } else {
        count = 0;
    }
    return state;
}

// PID controller, target 25mA
static float Kp = 0.006f;
static float Ki = 0.03f;
static float pid_integral = 0.0f;

static uint16_t pid_update(int32_t measured_ua, int32_t target_ua, float dt) {
    float error = (float)(target_ua - measured_ua);

    // Proportional
    float P = Kp * error;

    // Integral with anti-windup clamping to [0, PWM_PERIOD]
    pid_integral += error * dt;
    float max_integral = (float)PWM_PERIOD / Ki;
    if (pid_integral > max_integral) pid_integral = max_integral;
    else if (pid_integral < 0.0f)    pid_integral = 0.0f;
    float I = Ki * pid_integral;

    float output = P + I;
    if (output < 0.0f) output = 0.0f;
    if (output > (float)PWM_PERIOD) output = (float)PWM_PERIOD;

    return (uint16_t)output;
}

// Printing over USART for debugging
static void uart_init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAENB;
    RCC->APB1ENR |= RCC_APB1ENR_USART2ENB;

    GPIOA->MODER &= ~(3u << 4);
    GPIOA->MODER  |= (2u << 4);
    GPIOA->AFR[0] &= ~(0xFu << 8);
    GPIOA->AFR[0] |= (7u << 8);
    USART2->BRR = 16000000 / 115200;
    USART2->CR1 = USART_CR1_UEB | USART_CR1_TEB;
}
static void uart_print(const char *s) {
    while (*s) { while (!(USART2->SR & USART_SR_TXEB)); USART2->DR = *s++; }
}



int main(void) {
    char msg[120];
    uart_init();
    uart_print("UART ready\r\n");
    timer_init();
    spi_init();
    uart_print("SPI1 Master ready\r\n");
    I2C1_Init();
    int i2c_status = INA219_Init();
    if (i2c_status == 0) {
        uart_print("INA219 ready\r\n");
    } else {
        snprintf(msg, sizeof msg, "INA219 INIT FAILED (error %d)\r\n", i2c_status);
        uart_print(msg);
    }
    pwm_init();
    button_init();
    uart_print("PWM and Button ready\r\n");

    uint32_t next_pid = 0, next_tx = 0;
    uint16_t seq = 0;
    int32_t latest_current_ua = 0;
    uint16_t current_pwm = 0;
    int i2c_read_err = 0;

    for (;;) {
        uint32_t now = TIM2->CNT;  

        // Continuous Closed-Loop PID
        if ((int32_t)(now - next_pid) >= 0) {
            next_pid = now + PID_INTERVAL_US;
            i2c_read_err = INA219_ReadCurrent_uA(&latest_current_ua);

            // Regulate towards target
            current_pwm = pid_update(latest_current_ua, TARGET_CURRENT_UA, 0.02f);
            pwm_set_duty(current_pwm);
        }

        // SPI Transmission 
        if ((int32_t)(now - next_tx) >= 0) {
            next_tx = now + 100000;
            payload_spi t = {
                .sync      = SYNC_DATA,
                .seq        = seq++,
                .timestamp  = now,
                .current_ua = latest_current_ua,
                .checksum   = 0,
                .reserved   = 0
            };
            t.checksum = calc_checksum((const uint8_t *)&t, 12);

            spi_send_telemetry(&t);

            // Printing transmitted payload for checking
            if (i2c_read_err != 0) {
                snprintf(msg, sizeof msg, "[SPI TX] seq=%u time=%lu us I_meas=0 uA [I2C ERR %d] PWM=%u\r\n",
                         t.seq, (unsigned long)t.timestamp, i2c_read_err, current_pwm);
            } else {
                int32_t ma_whole = t.current_ua / 1000;
                int32_t ma_frac = (t.current_ua % 1000) / 100;
                if (ma_frac < 0) ma_frac = -ma_frac;
                snprintf(msg, sizeof msg, "[SPI TX] seq=%u time=%lu us I_meas=%ld uA (%ld.%ld mA, target=%ld uA) PWM=%u\r\n",
                         t.seq, (unsigned long)t.timestamp, (long)t.current_ua, (long)ma_whole, (long)ma_frac,
                         (long)TARGET_CURRENT_UA, current_pwm);
            }
            uart_print(msg);
        }
    }
}
