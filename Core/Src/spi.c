#include "spi.h"

// 1MHz SPI clock
void spi_init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAENB;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1ENB;

    // PA4 is CS, set as Output, High speed
    GPIOA->MODER &= ~(3u << 8);
    GPIOA->MODER |= (1u << 8);
    GPIOA->OSPEEDR |= (3u << 8);
    // Initially kept high
    GPIOA->BSRR = (1u << 4);

    // PA5 (SCK), PA6 (MISO), PA7 (MOSI); Alternate Function AF5
    GPIOA->MODER &= ~(3u << 10);
    GPIOA->MODER |= ((3u << 12) | (3u << 14) | (2u << 10) | (2u << 12) | (2u << 14));
    GPIOA->OSPEEDR |= ((3u << 10) | (3u << 12) | (3u << 14));
    GPIOA->AFR[0] &= ~(0xFu << 20);
    GPIOA->AFR[0]  = ((0xFu << 24) | (0xFu << 28) | (5u << 20) | (5u << 24) | (5u << 28));

    // SPI1 Master configurations
    // fPCLK/32 (500 kHz), CPOL=0, CPHA=0, 8-bit, MSB first, Software NSS
    SPI1->CR1 = SPI1_CR1_MSTR | SPI1_CR1_SSM | SPI1_CR1_SSI | SPI1_CR1_BR_DIV32;

    // Enable SPI1
    SPI1->CR1 |= SPI1_CR1_SPE;
}

uint8_t spi_transfer(uint8_t tx) {
    while (!(SPI1->SR & SPI1_SR_TXE));
    *((volatile uint8_t *)&SPI1->DR) = tx;
    while (!(SPI1->SR & SPI1_SR_RXNE));
    return *((volatile uint8_t *)&SPI1->DR);
}

uint16_t calc_checksum(const uint8_t *data, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

void spi_send_telemetry(const payload_spi *t) {
    // Assert CS LOW
    GPIOA->BSRR = (1u << 20);
    // Setup delay for slave DMA
    for (volatile int i = 0; i < 200; i++);

    const uint8_t *p = (const uint8_t *)t;
    for (size_t i = 0; i < sizeof(payload_spi); i++) {
        spi_transfer(p[i]);
    }

    while (SPI1->SR & SPI1_SR_BSY);
    // Hold delay
    for (volatile int i = 0; i < 200; i++);
    // Deassert CS HIGH
    GPIOA->BSRR = (1u << 4);
}
