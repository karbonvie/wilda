/*
board_pins.h - Physical Mapping for WildlifeThrive Tier 2
*/

#pragma once

#include <hal/nrf_saadc.h>

#if defined(CONFIG_BOARD_ADAFRUIT_FEATHER_NRF52840)

/* GPIO Mapping */
#define WAKE_PIN_NUM       6      /* P0.06, Feather pin: D11 */
#define SENSOR_PWR_NUM    26      /* P0.26, Feather pin: D9 */
#define DEBUG_PULSE_NUM    7      /* P0.07, Feather pin: D6 (Moved for safety) */
#define DBG_MARKER_NUM     0

/* ADC Physical Inputs (SAADC AIN Index) */
#define SENSOR1_ADC_INPUT 2       /* Feather A0 -> P0.04 -> AIN2 */
#define SENSOR2_ADC_INPUT 3       /* Feather A1 -> P0.05 -> AIN3 */

/* Power-Leak Prevention Pins (Feather Specific) */
#define UART_RX_PIN        8      /* P0.08 */
#define QSPI_SCK_PIN      19      /* P0.19 */
#define QSPI_CS_PIN       20      /* P0.20 */
#define QSPI_IO0_PIN      17      /* P0.17 */
#define QSPI_IO1_PIN      22      /* P0.22 */
#define QSPI_IO2_PIN      23      /* P0.23 */
#define QSPI_IO3_PIN      21      /* P0.21 */

#define BOARD_USE_UNUSED_PIN_INIT 0
#define ULP_ISR_MARKER_DEFAULT    0

#elif defined(CONFIG_BOARD_NRF52840DK_NRF52840)
/* nRF52840 DK Mapping (Not priority, for reference only) */
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