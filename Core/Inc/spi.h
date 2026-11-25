#include "stm32f4xx.h"

#define RCC_APB2ENR_SPI1ENB      (1U << 12)
#define RCC_AHB1ENR_GPIOAENB     (1U << 0)

#define SPI1_CR1_CPHA        (1U << 0)
#define SPI1_CR1_CPOL        (1U << 1)
#define SPI1_CR1_MSTR        (1U << 2)
#define SPI1_CR1_SPE         (1U << 6)
#define SPI1_CR1_LSBFIRST    (1U << 7)
#define SPI1_CR1_SSI         (1U << 8)
#define SPI1_CR1_SSM         (1U << 9)
#define SPI1_CR1_BR_DIV32    (4U << 3)
#define SPI1_SR_RXNE         (1U << 0)
#define SPI1_SR_TXE          (1U << 1)
#define SPI1_SR_BSY          (1U << 7)

// Target current for PID control
#define TARGET_CURRENT_UA   25000
#define PWM_PERIOD          1599  // 10 kHz
#define PID_INTERVAL_US     20000
#define SYNC_DATA     0xA55A

// SPI payload configuration
typedef struct __attribute__((packed)) {
    uint16_t sync;
    uint16_t seq;
    uint32_t timestamp;
    int32_t  current_ua;
    uint16_t checksum;
    uint16_t reserved;
} payload_spi;


// Functions
void spi_init(void);
uint8_t spi_transfer(uint8_t tx);
uint16_t calc_checksum(const uint8_t *data, size_t len);
void spi_send_telemetry(const payload_spi *t);



