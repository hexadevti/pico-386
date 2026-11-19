// nousb_rp2350.cpp -- a do-nothing USB `Serial` for the RP2350 build without a USB stack
//
// The build uses arduino-pico's usbstack=nousb (2026-11-17), a test for a user's Pico 2 (official,
// in a PicoCalc v2.0 with keyboard BIOS 1.7) that hit a HardFault at every start on v0.2.1 and
// v0.3.0. The first faults returned from TinyUSB's tu_fifo_read to byte-pattern addresses
// (0x08080808, 0x10101010), but TinyUSB was not the cause: without USB it still faulted at 300 MHz,
// at random places, and at the stock 150 MHz both builds reached the menu (2026-11-19). His chip
// does not run 300 MHz at the boot-time 1.10 V. Nothing here needs USB: the log and the @ commands
// also go through UART0, which the mainboard's CH340 puts on its USB-C port.
//
// With NO_USB the core leaves SerialUSB.cpp out but SerialUSB.h still declares `Serial`, and both
// our code (hal_log, rp2350CheckSerialCmd) and SdFat's PioSdio debug paths use it. These stubs keep
// all of them linking, and turn every USB write into nothing and every read into "no data".

#if defined(BOARD_PICOCALC_RP2350) && defined(NO_USB)

#include <Arduino.h>

SerialUSB::SerialUSB() {}
void SerialUSB::begin(unsigned long baud) { (void)baud; }
void SerialUSB::end() {}
int SerialUSB::peek() { return -1; }
int SerialUSB::read() { return -1; }
int SerialUSB::available() { return 0; }
int SerialUSB::availableForWrite() { return 0; }
void SerialUSB::flush() {}
size_t SerialUSB::write(uint8_t c) { (void)c; return 1; }
size_t SerialUSB::write(const uint8_t *p, size_t len) { (void)p; return len; }
SerialUSB::operator bool() { return false; }
bool SerialUSB::dtr() { return false; }
bool SerialUSB::rts() { return false; }
void SerialUSB::ignoreFlowControl(bool ignore) { (void)ignore; }

SerialUSB Serial;

#endif
