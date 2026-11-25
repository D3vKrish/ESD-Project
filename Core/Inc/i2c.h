#include "stm32f4xx.h"
#define INA219_I2C_ADDR 0x40
#define INA219_REG_CONFIG 0x00
#define INA219_REG_SHUNT 0x01

#define RCC_AHB1ENR_GPIOBENB	(1U << 1)   
#define RCC_APB1ENR_I2C1ENB     (1U << 21)

#define I2C1_CR1_PE 	(1U << 0)
#define I2C1_CR1_START	(1U << 8)
#define I2C1_CR1_STOP	(1U << 9)
#define I2C1_CR1_ACK	(1U << 10)
#define I2C1_CR1_POS	(1U << 11)
#define I2C1_CR1_SWRST	(1U << 15)
#define I2C1_SR1_SB		(1U << 0)
#define I2C1_SR1_ADDR	(1U << 1)
#define I2C1_SR1_BTF	(1U << 2)
#define I2C1_SR1_TXE	(1U << 7)
#define I2C1_SR1_AF		(1U << 10) 

#define I2C1_SR2_MSL	(1U << 0)
#define I2C1_SR2_BUSY   (1U << 1)
#define I2C1_SR2_TRA    (1U << 2)

#define I2C_TIMEOUT  100000

// Functions
extern int16_t latest_raw_shunt;
void I2C1_Init(void);
int INA219_Init(void);
int INA219_ReadShunt(int16_t *value);
int INA219_ReadCurrent_mA(int16_t *current_ma);
int INA219_ReadCurrent_uA(int32_t *current_ua);
