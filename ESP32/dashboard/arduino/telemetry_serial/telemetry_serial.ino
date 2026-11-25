// Serial output for the dashboard.
//
// The dashboard's Python side reads ONE LINE PER SAMPLE over USB serial:
//
//     <timestamp_ms>,<current_ma>,<seq>\n        e.g.  123456,487,1023
//
// Put print_telemetry() on whichever board is plugged into the PC:
//   * the CAN RECEIVER node: call it right after a telemetry frame is received
//   * or the SENSOR node itself (next to can_send) while testing without CAN
//
// Baud must match the --baud flag of dashboard/server.py (default 115200).
// Any other line you Serial.print (debug text) is ignored - just don't print a line
// that is exactly three comma-separated numbers.

#include <Arduino.h>

// Same layout as the struct sent over CAN (8 bytes -> fits one classic CAN frame).
// If your project already defines telemetry_t in a header, delete this and include it.
typedef struct {
  uint32_t timestamp;   // millis()
  int16_t  current_ma;  // INA219_ReadCurrent_mA()
  uint16_t seq;         // increments every frame
} telemetry_t;

void print_telemetry(const telemetry_t *t) {
  Serial.print(t->timestamp);
  Serial.print(',');
  Serial.print(t->current_ma);
  Serial.print(',');
  Serial.println(t->seq);
}

// ---------------------------------------------------------------------------
// Example A - sensor node, alongside your existing code:
//
//   int16_t current = INA219_ReadCurrent_mA();
//   telemetry_t t = { .timestamp = now, .current_ma = current, .seq = seq++ };
//   can_send(ID_TELEMETRY, &t, sizeof t);
//   print_telemetry(&t);                       // <-- add this line
//
// Example B - receiver node:
//
//   if (can_receive(&id, buf, &len) && id == ID_TELEMETRY && len == sizeof(telemetry_t)) {
//       telemetry_t t;
//       memcpy(&t, buf, sizeof t);
//       print_telemetry(&t);
//   }
//
// The stand-alone sketch below produces a fake signal so you can test the
// serial -> Python -> dashboard path with nothing but a bare Arduino.
// ---------------------------------------------------------------------------

static uint16_t seq = 0;

void setup() {
  Serial.begin(115200);
}

void loop() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 50) return;   // 20 Hz
  last = now;

  // Fake current: 500 mA +/- 350 mA slow sine + noise (replace with INA219_ReadCurrent_mA()).
  int16_t current = (int16_t)(500 + 350 * sin(now / 4000.0) + random(-15, 16));

  telemetry_t t = { now, current, seq++ };
  print_telemetry(&t);
}
