/*
board_pins.h

Purpose
- Central place for all hardware mapping.
- main.c contains logic only.

Important design rule
---------------------

This file defines ONLY physical hardware connections.

For ADC this means:
    - which SAADC INPUT is connected to the sensor

This file MUST NOT define:
    - SAADC channel slot numbers

Reason:
SAADC slots (0..7) are an internal ADC engine concept and should
be configured in main.c. They are independent of the physical
analog input pins.
*/

#pragma once

#include <hal/nrf_saadc.h>

/* ------------------------------------------------ */
/* Board detection                                  */
/* ------------------------------------------------ */

#if defined(CONFIG_BOARD_ADAFRUIT_FEATHER_NRF52840)

/* ================================================= */
/* Adafruit Feather nRF52840 Express                 */
/* ================================================= */

/* ------------------------------------------------ */
/* GPIO mapping                                     */
/* ------------------------------------------------ */

#define WAKE_PIN_NUM       6      /* P0.06 , Feather pin: D11 */
#define SENSOR_PWR_NUM    26      /* P0.26 , Feather pin: D9 */

/*
Debug pulse output used for oscilloscope visualization.
Moved off analog pins to avoid SAADC interaction.
*/
#define DEBUG_PULSE_NUM   15      /* P0.15, Feather pin: MI (MISO)*/
#define DBG_MARKER_NUM     0

/* ------------------------------------------------ */
/* ADC physical inputs                              */
/* ------------------------------------------------ */

/*
Sensor 1 analog input
Feather A0 → P0.04 → SAADC AIN2
*/
#define SENSOR1_ADC_INPUT 2 

/*
Sensor 2 analog input
Feather A1 → P0.05 → SAADC AIN3 
*/
#define SENSOR2_ADC_INPUT 3 

/* ------------------------------------------------ */
/* Power-Leak Prevention Pins (Feather Specific)    */
/* ------------------------------------------------ */
/* These pins are connected to hardware on the Feather (like the 
external flash chip and the USB-Serial chip). If we disable their 
drivers in Zephyr, we must park them manually. 
All of these are on Port 0 (P0.xx).
*/
#define UART_RX_PIN        8      /* P0.08 */
#define QSPI_SCK_PIN      19      /* P0.19 */
#define QSPI_CS_PIN       20      /* P0.20 */
#define QSPI_IO0_PIN      17      /* P0.17 */
#define QSPI_IO1_PIN      22      /* P0.22 */
#define QSPI_IO2_PIN      23      /* P0.23 */
#define QSPI_IO3_PIN      21      /* P0.21 */

/* ------------------------------------------------ */
/* Board behaviour switches                         */
/* ------------------------------------------------ */

#define BOARD_USE_UNUSED_PIN_INIT 0
#define ULP_ISR_MARKER_DEFAULT    0


#elif defined(CONFIG_BOARD_NRF52840DK_NRF52840)

#define WAKE_PIN_NUM       6
#define SENSOR_PWR_NUM    26
#define DEBUG_PULSE_NUM   15
#define DBG_MARKER_NUM    17

#define SENSOR1_ADC_INPUT NRF_SAADC_INPUT_AIN0
#define SENSOR2_ADC_INPUT NRF_SAADC_INPUT_AIN2

#define BOARD_USE_UNUSED_PIN_INIT 1
#define ULP_ISR_MARKER_DEFAULT    1

#else

#error "Unsupported board"

#endif