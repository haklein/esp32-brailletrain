---
name: hardware_setup
description: ESP32-S3 to BrailleWave hardware wiring and serial connection details
type: project
---

ESP32-S3 DevKitC-1 connects to HT BrailleWave via MAX232 level shifter.

- GPIO 7 (TX) -> MAX232 breakout RX pin
- GPIO 6 (RX) <- MAX232 breakout TX pin
- MAX232 DB9 -> **crossover/null-modem cable** -> BrailleWave RJ-12 serial
- UART0 via CH340 (/dev/ttyACM0) for debug serial at 115200 baud
- Upload port: /dev/ttyACM0

**Why:** Both MAX232 breakout and BrailleWave have female DB9. A gender changer only swaps gender (straight-through), doesn't cross pins 2↔3. Need a proper crossover/null-modem cable between them.

**How to apply:** Always use null-modem/crossover cable between MAX232 and BrailleWave. Pin assignment TX=7 RX=6 confirmed via loopback test.
