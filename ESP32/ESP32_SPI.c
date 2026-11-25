/*
 * ESP32 SPI Telemetry Receiver (16-Byte Aligned DMA) -- Arduino IDE
 *
 * Pairs with STM32F446RE SPI1 Master (500 kHz, Mode 0)
 *
 * Wiring (Direct 3.3V, NO transceivers):
 *   STM32 PA4 (A2)  <------> ESP32 GPIO 5  (CS)
 *   STM32 PA5 (D13) <------> ESP32 GPIO 18 (SCK)
 *   STM32 PA7 (D11) <------> ESP32 GPIO 23 (MOSI)
 *   STM32 PA6 (D12) <------> ESP32 GPIO 19 (MISO)
 *   STM32 GND       <------> ESP32 GND
 */

#include <Arduino.h>
#include "driver/spi_slave.h"
#include "esp_timer.h"

// ---------------- Pins (VSPI default) ----------------
#define GPIO_CS     5
#define GPIO_SCLK   18
#define GPIO_MOSI   23
#define GPIO_MISO   19

#define TELEMETRY_MAGIC 0xA55A
#define STATS_PERIOD_MS 5000
#define VERBOSE_RX      1

// ---------------- 16-byte Aligned Telemetry Frame ----------------
typedef struct __attribute__((packed)) {
  uint16_t magic;      // 0xA55A
  uint16_t seq;        // sequence counter
  uint32_t timestamp;  // microsecond timestamp from STM32 TIM2
  int32_t  current_ua; // current in microamperes
  uint16_t checksum;   // 16-bit additive sum of preceding 12 bytes
  uint16_t reserved;   // 0x0000 alignment padding for 32-bit DMA
} telemetry_t;
static_assert(sizeof(telemetry_t) == 16, "telemetry_t must be exactly 16 bytes");

typedef struct {
  int64_t     rx_us;
  telemetry_t telem;
} rx_item_t;

typedef struct {
  int64_t  rx_us;
  uint32_t node_ts_us;
  int32_t  current_ua;
  float    current_ma;
  uint16_t seq;
} sample_t;

// ---------------- State ----------------
static QueueHandle_t g_rxQueue;
static portMUX_TYPE  g_mux        = portMUX_INITIALIZER_UNLOCKED;
static sample_t      g_latest;
static bool          g_haveLatest = false;

static volatile uint32_t g_rxCount    = 0;
static volatile uint32_t g_queueDrops = 0;
static volatile uint32_t g_badFrames  = 0;
static uint32_t g_validCount = 0;
static uint32_t g_lostCount  = 0;

static bool     g_havePrev    = false;
static uint16_t g_expectedSeq = 0;
static int64_t  g_prevRxUs    = 0;
static uint32_t g_prevNodeTs  = 0;

// 12-byte payload checksum calculator
static uint16_t calc_checksum(const uint8_t *data, size_t len) {
  uint16_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += data[i];
  return sum;
}

bool getLatestSample(sample_t *out) {
  bool ok;
  portENTER_CRITICAL(&g_mux);
  ok = g_haveLatest;
  if (ok && out) *out = g_latest;
  portEXIT_CRITICAL(&g_mux);
  return ok;
}

// ---------------- SPI Slave RX Task (DMA) ----------------
static void spiRxTask(void *) {
  // DMA buffer must be 32-bit word aligned
  WORD_ALIGNED_ATTR uint8_t rx_buf[32];
  spi_slave_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length    = sizeof(telemetry_t) * 8; // Exactly 16 bytes = 128 bits
  t.rx_buffer = rx_buf;

  for (;;) {
    memset(rx_buf, 0, sizeof(rx_buf));
    esp_err_t ret = spi_slave_transmit(VSPI_HOST, &t, portMAX_DELAY);
    if (ret == ESP_OK) {
      int64_t ts = esp_timer_get_time();
      g_rxCount++;

      // Verify that full 128 bits were clocked
      if (t.trans_len >= sizeof(telemetry_t) * 8) {
        rx_item_t item;
        item.rx_us = ts;
        memcpy(&item.telem, rx_buf, sizeof(telemetry_t));

        if (xQueueSend(g_rxQueue, &item, 0) != pdTRUE) {
          g_queueDrops++;
        }
      } else {
        g_badFrames++;
      }
    }
  }
}

// ---------------- Frame Handling ----------------
static void handleFrame(const rx_item_t &item) {
  const telemetry_t &t = item.telem;

  // 1. Verify Magic Sync Word
  if (t.magic != TELEMETRY_MAGIC) {
    g_badFrames++;
    Serial.printf("[SPI ERR] Bad magic: 0x%04X (expected 0x%04X)\n", t.magic, TELEMETRY_MAGIC);
    return;
  }

  // 2. Verify Checksum over the 12 data bytes
  uint16_t expected_cs = calc_checksum((const uint8_t *)&t, 12);
  if (t.checksum != expected_cs) {
    g_badFrames++;
    Serial.printf("[SPI ERR] Checksum mismatch (calc=0x%04X, frame=0x%04X)\n", expected_cs, t.checksum);
    return;
  }

  g_validCount++;

  // 3. Sequence Tracking & Frame Loss Detection
  if (g_havePrev) {
    uint16_t gap = (uint16_t)(t.seq - g_expectedSeq);
    if (t.seq == 0 && g_expectedSeq != 0) {
      Serial.println("[NODE RESTART] Sequence counter reset to 0");
    } else if (gap != 0) {
      g_lostCount += gap;
      Serial.printf("[WARNING] LOST %u frame(s)! (expected seq=%u, got seq=%u)\n", gap, g_expectedSeq, t.seq);
    }
  }
  g_expectedSeq = t.seq + 1;

  // 4. Timing Spacing
  long dtRx   = g_havePrev ? (long)(item.rx_us - g_prevRxUs) : 0;
  long dtNode = g_havePrev ? (long)(int32_t)(t.timestamp - g_prevNodeTs) : 0;
  g_prevRxUs   = item.rx_us;
  g_prevNodeTs = t.timestamp;
  g_havePrev   = true;

  float ma = t.current_ua / 1000.0f;

  // Publish sample thread-safely
  portENTER_CRITICAL(&g_mux);
  g_latest.rx_us      = item.rx_us;
  g_latest.node_ts_us = t.timestamp;
  g_latest.current_ua = t.current_ua;
  g_latest.current_ma = ma;
  g_latest.seq        = t.seq;
  g_haveLatest        = true;
  portEXIT_CRITICAL(&g_mux);

#if VERBOSE_RX
  Serial.printf("[SPI RX] seq=%u  node_t=%lu us  I=%ld uA (%.1f mA)  dt_rx=%ld us  dt_node=%ld us\n",
                t.seq, (unsigned long)t.timestamp, (long)t.current_ua, ma, dtRx, dtNode);
#endif
}

static void printStats() {
  Serial.printf("--- [STATS] rx=%lu  valid=%lu  lost=%lu  bad=%lu  qdrop=%lu ---\n",
                (unsigned long)g_rxCount, (unsigned long)g_validCount,
                (unsigned long)g_lostCount, (unsigned long)g_badFrames,
                (unsigned long)g_queueDrops);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32 SPI Telemetry Receiver Starting (16-Byte Aligned)...");

  // Enable internal pull-up on CS to prevent crosstalk false triggers
  gpio_set_pull_mode((gpio_num_t)GPIO_CS, GPIO_PULLUP_ONLY);

  g_rxQueue = xQueueCreate(32, sizeof(rx_item_t));

  // Configure SPI Slave Bus
  spi_bus_config_t buscfg = {
    .mosi_io_num     = GPIO_MOSI,
    .miso_io_num     = GPIO_MISO,
    .sclk_io_num     = GPIO_SCLK,
    .quadwp_io_num   = -1,
    .quadhd_io_num   = -1,
    .max_transfer_sz = 32
  };

  spi_slave_interface_config_t slvcfg = {
    .spics_io_num    = GPIO_CS,
    .flags           = 0,
    .queue_size      = 3,
    .mode            = 0, // Mode 0 (CPOL=0, CPHA=0)
    .post_setup_cb   = NULL,
    .post_trans_cb   = NULL
  };

  esp_err_t err = spi_slave_initialize(VSPI_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    Serial.printf("SPI Slave init FAILED (0x%X)\n", err);
  } else {
    Serial.println("SPI Slave ready (CS=5, SCK=18, MOSI=23, MISO=19)");
  }

  // Launch dedicated RX task on Core 1
  xTaskCreatePinnedToCore(spiRxTask, "spi_rx", 4096, nullptr, 5, nullptr, 1);
}

void loop() {
  static uint32_t lastStats = 0;

  rx_item_t item;
  while (xQueueReceive(g_rxQueue, &item, 0) == pdTRUE) {
    handleFrame(item);
  }

  uint32_t now = millis();
  if (now - lastStats >= STATS_PERIOD_MS) {
    lastStats = now;
    printStats();
  }

  vTaskDelay(1);
}