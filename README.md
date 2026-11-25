# Precision Closed-Loop Current Controller & IoT Telemetry System

A dual-MCU embedded system featuring an **STM32F446RE (ARM Cortex-M4)** running a real-time digital PI current control loop, an **ESP32** acting as a high-speed SPI-slave telemetry bridge, and an interactive **real-time web dashboard** with instant mobile push alerts.

---

## System Overview

```
+-------------------------------------------------------------------------------+
|                                  HARDWARE LAYER                               |
|                                                                               |
|   +-------------------+        I2C1 (400 kHz)        +--------------------+   |
|   |  INA219 Sensor    | <--------------------------> |   STM32F446RE      |   |
|   | (Current / Shunt) |                              | (ARM Cortex-M4)    |   |
|   +-------------------+                              |                    |   |
|                                                      |  - Digital PI Loop |   |
|   +-------------------+       TIM3_CH1 PWM (10 kHz)  |  - 1 MHz TIM2 Time |   |
|   | Power Stage /     | <--------------------------- |  - SPI1 Master     |   |
|   | Load Driver       |                              +---------+----------+   |
+---|-------------------+----------------------------------------|--------------+
                                                                 |
                                              SPI (500 kHz, Mode 0)
                                              16-Byte Aligned DMA Frame
                                                                 |
+----------------------------------------------------------------v--------------+
|   +-----------------------------------------------------------------------+   |
|   |                              ESP32 GATEWAY                            |   |
|   |  - Hardware SPI Slave with DMA Buffering                              |   |
|   |  - Sync-word validation (0xA55A) + 16-bit additive checksum          |   |
|   |  - Serial / Wi-Fi telemetry forwarder                                 |   |
|   +------------------------------------+----------------------------------+   |
+----------------------------------------|--------------------------------------+
                                         |
                       Serial Stream / HTTP / WebSockets
                                         |
+----------------------------------------v--------------------------------------+
|                           REAL-TIME WEB DASHBOARD                             |
|                                                                               |
|   +-------------------------------+       +-------------------------------+   |
|   |       Live Chart View         |       |      Mobile Push Alerts       |   |
|   | - 50 Hz real-time current plot|       | - Low / High threshold bands  |   |
|   | - Dynamic target / band tweak |       | - Instant ntfy.sh integration |   |
|   +-------------------------------+       +-------------------------------+   |
+-------------------------------------------------------------------------------+
```

---

## Key Features

- **Bare-Metal Register-Level STM32 Drivers**:
  - Direct CMSIS register manipulation for **I2C1**, **SPI1**, **TIM2**, **TIM3 (PWM)**, and **USART2**.
  - No bloated abstraction layers in the critical timing path.
- **Closed-Loop Digital PI Current Controller**:
  - Regulates current to a steady setpoint (default: `25.0 mA`).
  - Sampling rate: **50 Hz** ($20\,\text{ms}$ loop).
  - Integral anti-windup clamping to prevent overshoot and hunting on resistive loads.
- **High-Integrity Inter-Chip SPI Telemetry**:
  - Fixed **16-byte frame** optimized for ESP32 hardware DMA (32-bit word aligned).
  - Synchronization byte(`0xA55A`), sequence numbering, and checksum verification.
- **Zero-Dependency Web Dashboard**:
  - Real-time current plotting using lightweight browser canvas/SVG.
  - Server-Sent Events (SSE) backend powered by Python.
  - Replay and demo modes available for testing without physical hardware connected.
- **Push Notifications via ntfy**:
  - Zero-cost, account-free push alerts sent directly to smartphones if current drifts outside safety thresholds.

---

## Hardware Pinout & Interconnect

### STM32F446RE $\leftrightarrow$ ESP32 (SPI1 Telemetry)
| STM32 Pin | Nucleo Header | ESP32 Pin | Function | Notes |
|:---:|:---:|:---:|:---:|:---|
| **PA4** | A2 | **GPIO 5** | SPI CS / NSS | Active Low Chip Select |
| **PA5** | D13 | **GPIO 18** | SPI SCK | 500 kHz Clock, Mode 0 |
| **PA7** | D11 | **GPIO 23** | SPI MOSI | Master Out $\rightarrow$ Slave In |
| **PA6** | D12 | **GPIO 19** | SPI MISO | Slave Out $\rightarrow$ Master In |
| **GND** | GND | **GND** | Ground | Common reference |

### STM32F446RE $\leftrightarrow$ INA219 (I2C1 Sensor)
| STM32 Pin | Nucleo Header | INA219 Pin | Function | Notes |
|:---:|:---:|:---:|:---:|:---|
| **PB8** / **PB6** | D15 / D10 | **SCL** | I2C Clock | Pull-up to 3.3V |
| **PB9** / **PB7** | D14 / D2 | **SDA** | I2C Data | Pull-up to 3.3V |
| **3V3** | 3V3 | **VCC** | Power | 3.3V supply |
| **GND** | GND | **GND** | Ground | Common ground |

### PWM Output
| STM32 Pin | Nucleo Header | Function | Description |
|:---:|:---:|:---:|:---|
| **PB4** | D5 | **TIM3_CH1** | 10 kHz PWM Output ($ARR = 1599$, $0 - 100\%$ duty) |

---

## SPI Frame Protocol Specification

The STM32 transmits a packed, 16-byte fixed-size binary frame to the ESP32. This structure satisfies the ESP32 DMA 4-byte buffer alignment requirement and eliminates frame slip:

```c
typedef struct __attribute__((packed)) {
    uint16_t sync;        // Synchronization Word (0xA55A)
    uint16_t seq;         // Packet Sequence Number
    uint32_t timestamp;   // Microsecond timestamp
    int32_t  current_ua;  // Measured current in microamperes (µA)
    uint16_t checksum;    // 16-bit additive sum of bytes
    uint16_t reserved;    // Alignment padding (0x0000)
} payload_spi;
```

---

## Directory Structure

```
├── Core/
│   ├── Inc/
│   │   ├── i2c.h               # I2C1 bare-metal bit definitions & INA219 declarations
│   │   ├── main.h              # Peripheral clock & timer bit definitions
│   │   └── spi.h               # SPI1 definitions, payload_spi struct, constants
│   ├── Src/
│   │   ├── i2c.c               # Bare-metal I2C driver & INA219 sensor interface
│   │   ├── main.c              # Main control loop, PI algorithm, PWM & UART
│   │   ├── spi.c               # SPI1 master transmission & checksum computation
│   │   └── system_stm32f4xx.c  # CMSIS clock configurations
│   └── Startup/                # Cortex-M4 assembly startup files
├── Drivers/                    # ST HAL & ARM CMSIS library files
├── ESP32/
│   ├── ESP32_SPI.c             # ESP32 SPI slave receiver with DMA & queue management
│   └── dashboard/              # Live dashboard server & frontend
│       ├── server.py           # SSE stream & REST API HTTP server
│       ├── sources.py          # Serial data reader & CSV replay engine
│       ├── alerts.py           # Threshold monitor & ntfy push dispatcher
│       └── static/
│           └── index.html      # Real-time web UI
├── .gitignore                  # Git exclusions for build outputs and artifacts
└── README.md                   # Project documentation
```

---

## Getting Started

### 1. STM32 Firmware (STM32CubeIDE)
1. Open **STM32CubeIDE** and choose `File -> Open Projects from File System...`.
2. Select the repository root folder.
3. Build the project (`Ctrl + B`).
4. Connect your **Nucleo-F446RE** via USB and click **Run / Debug**.

### 2. ESP32 SPI Receiver (Arduino IDE / ESP-IDF)
1. Open `ESP32/ESP32_SPI.c` (or use within an Arduino sketch).
2. Wire the SPI pins between the STM32 and ESP32 as outlined in the pinout table.
3. Flash the firmware to your ESP32 board.
4. Open the Serial Monitor at **115200 baud** to verify incoming SPI packets:
   ```
   [SPI RX] seq=124 node_t=2480000 us I=25040 uA (25.04 mA) ...
   ```

### 3. Real-Time Web Dashboard
1. Navigate to the dashboard directory:
   ```bash
   cd ESP32/dashboard
   ```
2. Install dependencies:
   ```bash
   pip install pyserial
   ```
3. Run live mode (connected to ESP32 serial port):
   ```bash
   python server.py --port 8000
   ```
   *(Or run in demo simulation mode without hardware: `./run_demo.sh`)*
4. Open your browser and navigate to:
   ```
   http://localhost:8000
   ```

---

## Configuration & Tuning

- **Target Current**: Change `TARGET_CURRENT_UA` in [Core/Inc/spi.h](Core/Inc/spi.h) (default is `25000` for 25 mA).
- **PI Gains**: Adjusted in [Core/Src/main.c](Core/Src/main.c):
  - $K_p = 0.006$
  - $K_i = 0.03$
- **PWM Frequency**: Configured in `TIM3->ARR = 1599` ($10\,\text{kHz}$ at $16\,\text{MHz}$ bus clock).
