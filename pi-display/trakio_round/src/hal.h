// Board services: I2C bus, TCA9554 expander (power hold, amp, interrupts),
// battery voltage and the PWR button. Everything here runs on the loop task.
#pragma once
#include <Arduino.h>

void hal_init();                 // call first: latches power on battery

// TCA9554
void exio_write(uint8_t pin, bool level);
uint8_t exio_read_inputs();      // reading also clears the expander's #INT

// Battery
int  battery_millivolts();
int  battery_percent();          // smoothed, 0..100
bool battery_on_usb();           // best guess: VBUS keeps the cell above 4.15 V

// PWR key: call often; returns a press event since the last call
enum KeyEvent { KEY_NONE, KEY_SHORT, KEY_LONG };
KeyEvent pwr_key_poll();

void board_power_off();          // drops BAT_EN; on USB this does nothing
