#include "i2c.h"

// PB6 = SCL, PB7 = SDA (I2C1 and AF4)
void I2C1_Init(void){
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBENB;
	RCC->APB1ENR |= RCC_APB1ENR_I2C1ENB;

	// Reset I2C1
	I2C1->CR1 |= I2C1_CR1_SWRST;
	for (volatile int i = 0; i < 2000; i++);
	I2C1->CR1 &= ~I2C1_CR1_SWRST;

	// GPIO Configurations
	GPIOB->MODER &= ~((3u << 12) | (3u << 14));
	GPIOB->MODER |= ((2u << 12) | (2u << 14));

	GPIOB->OTYPER |= (1u << 6) | (1u << 7);

	GPIOB->PUPDR &= ~((3u << 12) | (3u << 14));
	GPIOB->PUPDR  |= ((1u << 12) | (1u << 14));

	GPIOB->OSPEEDR |= (3u << 12) | (3u << 14);

	GPIOB->AFR[0] &= ~((0xFu << 24) | (0xFu << 28));
	GPIOB->AFR[0] |= ((4u << 24) | (4u << 28));

	// I2C1 configuration
	I2C1->CR2   = 16;       // 16 MHz APB1
	I2C1->CCR   = 80;       // 100 kHz standard mode
	I2C1->TRISE = 17;       // 1000ns / 62.5ns + 1 = 17
	I2C1->CR1   = I2C1_CR1_ACK | I2C1_CR1_PE; // Enable ACK and I2C
}


int INA219_Init(void){
	uint32_t reg_data;

	// Wait for bus to get ready
	reg_data = I2C_TIMEOUT;
	while ((I2C1->SR2 & I2C1_SR2_BUSY) && --reg_data);
	if (!reg_data) {
		I2C1_Init();
	}

	// Generate START condition
	I2C1->CR1 |= I2C1_CR1_START;
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_SB) && --reg_data);
	// SB timeout
	if (!reg_data) return -1;

	// Send address with write bit
	I2C1->DR = (INA219_I2C_ADDR << 1);
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_ADDR) && --reg_data) {
		if (I2C1->SR1 & I2C1_SR1_AF) {
			I2C1->SR1 &= ~I2C1_SR1_AF;
			// Send STOP bit
			I2C1->CR1 |= I2C1_CR1_STOP;
			// NACK: Sensor not responding
			return -2;
		}
	}
	if (!reg_data) {
		I2C1->CR1 |= I2C1_CR1_STOP;
		// ADDR timeout
		return -3;
	}
	(void)I2C1->SR1;
	(void)I2C1->SR2;

	// Point to Configuration Register
	I2C1->DR = INA219_REG_CONFIG;
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_TXE) && --reg_data);
	if (!reg_data) {
		I2C1->CR1 |= I2C1_CR1_STOP;
		return -4;
	}

	// Send Config MSB (32V range, +-320mV, 12-bit continuous: 0x399F)
	uint8_t config_msb = 0x39;
	uint8_t config_lsb = 0x9F;
	I2C1->DR = config_msb;
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_TXE) && --reg_data);
	if (!reg_data) {
		I2C1->CR1 |= I2C1_CR1_STOP;
		return -5;
	}

	// Send Config LSB
	I2C1->DR = config_lsb;
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_BTF) && --reg_data);
	I2C1->CR1 |= I2C1_CR1_STOP;
	if (!reg_data) 
		return -6;

	return 0;
}


int INA219_ReadShunt(int16_t *value){
	uint8_t msb = 0, lsb = 0;
	uint32_t reg_data;

	// Point to Shunt Voltage Register
	I2C1->CR1 |= I2C1_CR1_START;
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_SB) && --reg_data);
	if (!reg_data) 
		return -1;

	I2C1->DR = (INA219_I2C_ADDR << 1);
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_ADDR) && --reg_data) {
		if (I2C1->SR1 & I2C1_SR1_AF) {
			I2C1->SR1 &= ~I2C1_SR1_AF;
			I2C1->CR1 |= I2C1_CR1_STOP;
			return -2;
		}
	}
	if (!reg_data) { 
		I2C1->CR1 |= I2C1_CR1_STOP; 
		return -3; 
	}
	(void)I2C1->SR1; 
	(void)I2C1->SR2;

	I2C1->DR = INA219_REG_SHUNT;
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_BTF) && --reg_data);
	if (!reg_data) { 
		I2C1->CR1 |= I2C1_CR1_STOP; 
		return -4; 
	}

	// Repeated START for 2-byte read sequence
	I2C1->CR1 |= I2C1_CR1_START;
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_SB) && --reg_data);
	if (!reg_data) return -5;

	I2C1->DR = (INA219_I2C_ADDR << 1) | 1; // Read address
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_ADDR) && --reg_data) {
		if (I2C1->SR1 & I2C1_SR1_AF) {
			I2C1->SR1 &= ~I2C1_SR1_AF;
			I2C1->CR1 |= I2C1_CR1_STOP;
			return -6;
		}
	}
	if (!reg_data) { 
		I2C1->CR1 |= I2C1_CR1_STOP; 
		return -7; 
	}

	// Set POS=1 and Clear ACK=0 BEFORE clearing ADDR
	I2C1->CR1 |= I2C1_CR1_POS;
	I2C1->CR1 &= ~I2C1_CR1_ACK;
	// Clear ADDR
	(void)I2C1->SR1; 
	(void)I2C1->SR2; 

	// Wait for BTF=1
	reg_data = I2C_TIMEOUT;
	while (!(I2C1->SR1 & I2C1_SR1_BTF) && --reg_data);
	if (!reg_data) {
		I2C1->CR1 |= I2C1_CR1_STOP;
		I2C1->CR1 &= ~I2C1_CR1_POS;
		return -8;
	}

	// Generate STOP condition
	I2C1->CR1 |= I2C1_CR1_STOP;

	// Read MSB then LSB
	msb = I2C1->DR;
	lsb = I2C1->DR;

	// Reset POS
	I2C1->CR1 &= ~I2C1_CR1_POS;
	I2C1->CR1 |= I2C1_CR1_ACK;

	*value = (int16_t)((msb << 8) | lsb);
	return 0;
}

int16_t latest_raw_shunt = 0;

int INA219_ReadCurrent_mA(int16_t *current_ma){
	int16_t raw_shunt = 0;
	int res = INA219_ReadShunt(&raw_shunt);
	if (res != 0) {
		*current_ma = 0;
		return res;
	}
	latest_raw_shunt = raw_shunt;
	// 0.1 ohm shunt resistor: 1 LSB = 10 uV -> 0.1 mA (raw / 10)
	*current_ma = (raw_shunt / 10);
	return 0;
}

int INA219_ReadCurrent_uA(int32_t *current_ua){
	int16_t raw_shunt = 0;
	int res = INA219_ReadShunt(&raw_shunt);
	if (res != 0) {
		*current_ua = 0;
		return res;
	}
	latest_raw_shunt = raw_shunt;
	// 1 LSB = 10 uV across 0.1 ohm shunt = 100 uA
	*current_ua = (int32_t)raw_shunt * 100;
	return 0;
}


