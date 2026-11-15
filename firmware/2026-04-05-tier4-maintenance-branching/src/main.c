/*
main.c - WildlifeThrive STABLE + Flash Dump (carefully merged)

Design intent:
- keep the EXACT stability of your last working version
- DO NOT reintroduce HFCLK or SAADC forced shutdowns (they caused instability)
- add flash dump ONLY inside maintenance mode
- zero impact on normal mode timing / wake / pulses

Key rule applied here:
maintenance path must be electrically and logically isolated from runtime path

*/

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>

#include <hal/nrf_power.h>

#include "board_pins.h"

/* -------- CONFIG -------- */
#define SENSOR_SETTLE_MS 2
#define GAP_US           500
#define ADC_RESOLUTION   12
#define ADC_ACQ_US       10
#define MAX_PULSES       15

#define FLASH_SIZE_BYTES (1024 * 1024)   /* external flash size */
#define EVENT_SIZE       8               /* packed event size */

/* -------- EVENT STRUCTURE -------- */
/* must match what was written */
typedef struct __packed {
    uint32_t timestamp_ms;
    int16_t sensor1_mv;
    int16_t sensor2_mv;
} event_t;

/* -------- DEVICES -------- */
static const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *adc   = DEVICE_DT_GET(DT_NODELABEL(adc));

/* -------- ADC CONFIG -------- */
static const struct adc_channel_cfg ch0 = {
    .gain = ADC_GAIN_1_6,
    .reference = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, ADC_ACQ_US),
    .channel_id = 0,
    .input_positive = SENSOR1_ADC_INPUT
};

static const struct adc_channel_cfg ch1 = {
    .gain = ADC_GAIN_1_6,
    .reference = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, ADC_ACQ_US),
    .channel_id = 1,
    .input_positive = SENSOR2_ADC_INPUT
};

/* -------- SYNC -------- */
static struct k_sem wake_sem;
static struct gpio_callback wake_cb;

/* -------- UTILS -------- */
static inline int32_t adc_to_mv_fast(int16_t raw) {
    if (raw <= 0) return 0;
    return (raw * 3600) / 4096;
}

static inline int map_pulses(int32_t mv) {
    if (mv <= 0) return 0;
    return (mv * MAX_PULSES) / 3600;
}

/* -------- ADC -------- */
static void read_adc(int16_t *v0, int16_t *v1) {
    int16_t sample;

    struct adc_sequence seq = {
        .buffer = &sample,
        .buffer_size = sizeof(sample),
        .resolution = ADC_RESOLUTION
    };

    seq.channels = BIT(0);
    adc_read(adc, &seq);
    adc_read(adc, &seq);
    *v0 = sample;

    k_busy_wait(20);

    seq.channels = BIT(1);
    adc_read(adc, &seq);
    adc_read(adc, &seq);
    *v1 = sample;
}

/* -------- PULSES -------- */
static void emit_pulses(int count) {
    for (int i = 0; i < count; i++) {
        gpio_pin_set(gpio0, DEBUG_PULSE_NUM, 1);
        k_busy_wait(200);
        gpio_pin_set(gpio0, DEBUG_PULSE_NUM, 0);
        k_busy_wait(200);
    }
}

/* -------- ISR -------- */
static void wake_isr(const struct device *port,
                     struct gpio_callback *cb,
                     uint32_t pins)
{
    k_sem_give(&wake_sem);   /* keep simple → no timing side effects */
}

/* =========================================================
   BITBANG FLASH (ONLY USED IN MAINTENANCE)
   ========================================================= */

static void qspi_init_pins(void) {
    gpio_pin_configure(gpio0, QSPI_CS_PIN,  GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(gpio0, QSPI_SCK_PIN, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio0, QSPI_IO0_PIN, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio0, QSPI_IO1_PIN, GPIO_INPUT);
    gpio_pin_configure(gpio0, QSPI_IO2_PIN, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio0, QSPI_IO3_PIN, GPIO_OUTPUT_INACTIVE);
}

static void flash_cmd(uint8_t cmd) {
    gpio_pin_set(gpio0, QSPI_CS_PIN, 0);

    for (int i = 7; i >= 0; i--) {
        gpio_pin_set(gpio0, QSPI_IO0_PIN, (cmd >> i) & 1);
        gpio_pin_set(gpio0, QSPI_SCK_PIN, 1);
        k_busy_wait(1);
        gpio_pin_set(gpio0, QSPI_SCK_PIN, 0);
    }

    gpio_pin_set(gpio0, QSPI_CS_PIN, 1);
}

static void flash_wake(void) {
    flash_cmd(0xAB);
    k_busy_wait(50);
}

static void flash_sleep(void) {
    flash_cmd(0xB9);
}

static void flash_read(uint32_t addr, uint8_t *buf, uint16_t len) {

    gpio_pin_set(gpio0, QSPI_CS_PIN, 0);

    uint8_t hdr[4] = {
        0x03,
        (addr >> 16) & 0xFF,
        (addr >> 8) & 0xFF,
        addr & 0xFF
    };

    for (int j = 0; j < 4; j++) {
        for (int i = 7; i >= 0; i--) {
            gpio_pin_set(gpio0, QSPI_IO0_PIN, (hdr[j] >> i) & 1);
            gpio_pin_set(gpio0, QSPI_SCK_PIN, 1);
            k_busy_wait(1);
            gpio_pin_set(gpio0, QSPI_SCK_PIN, 0);
        }
    }

    for (int j = 0; j < len; j++) {
        uint8_t b = 0;
        for (int i = 7; i >= 0; i--) {
            gpio_pin_set(gpio0, QSPI_SCK_PIN, 1);
            if (gpio_pin_get(gpio0, QSPI_IO1_PIN)) b |= (1 << i);
            gpio_pin_set(gpio0, QSPI_SCK_PIN, 0);
        }
        buf[j] = b;
    }

    gpio_pin_set(gpio0, QSPI_CS_PIN, 1);
}

/* =========================================================
   USB HELPERS (SAFE, NO PRINTF)
   ========================================================= */

static void usb_write_str(const struct device *cdc, const char *s) {
    while (*s) uart_poll_out(cdc, *s++);
}

static void usb_write_u32(const struct device *cdc, uint32_t v) {

    char buf[12];
    int i = 0;

    if (v == 0) {
        uart_poll_out(cdc, '0');
        return;
    }

    while (v > 0) {
        buf[i++] = '0' + (v % 10);
        v /= 10;
    }

    for (int j = i - 1; j >= 0; j--) {
        uart_poll_out(cdc, buf[j]);
    }
}

/* =========================================================
   MAINTENANCE MODE
   ========================================================= */

static void run_maintenance(void) {

    const struct device *cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

    if (usb_enable(NULL) != 0) return;

    k_msleep(1000);   /* allow host to enumerate */

    usb_write_str(cdc, "\r\nMaintenance mode active\r\n");
    usb_write_str(cdc, "Dump start\r\n");

    qspi_init_pins();   /* reconfigure pins ONLY here */
    flash_wake();

    uint32_t addr = 0;
    event_t ev;

    while (addr < FLASH_SIZE_BYTES) {

        flash_read(addr, (uint8_t*)&ev, sizeof(event_t));

        if (ev.timestamp_ms == 0xFFFFFFFF) break;  /* end marker */

        usb_write_u32(cdc, ev.timestamp_ms);
        usb_write_str(cdc, ",");
        usb_write_u32(cdc, (uint32_t)ev.sensor1_mv);
        usb_write_str(cdc, ",");
        usb_write_u32(cdc, (uint32_t)ev.sensor2_mv);
        usb_write_str(cdc, "\r\n");

        addr += sizeof(event_t);
    }

    usb_write_str(cdc, "Dump end\r\n");

    flash_sleep();   /* return flash to deep sleep */

    while (1) {
        k_sleep(K_SECONDS(2));   /* idle forever */
    }
}

/* =========================================================
   MAIN (UNCHANGED STABLE CORE)
   ========================================================= */

int main(void)
{
    if (!device_is_ready(gpio0) || !device_is_ready(adc)) return 0;

    /* --- BOOT MODE --- */
    gpio_pin_configure(gpio0, WAKE_PIN_NUM, GPIO_INPUT | GPIO_PULL_DOWN);
    k_msleep(50);

    if (gpio_pin_get(gpio0, WAKE_PIN_NUM)) {
        run_maintenance();   /* completely separate execution path */
    }

    /* --- POWER --- */
    NRF_USBD->ENABLE = 0;
    NRF_POWER->TASKS_LOWPWR = 1;
    nrf_power_dcdcen_set(NRF_POWER, true);

    /* --- GPIO --- */
    gpio_pin_configure(gpio0, SENSOR_PWR_NUM, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio0, DEBUG_PULSE_NUM, GPIO_OUTPUT_INACTIVE);

    gpio_pin_configure(gpio0, QSPI_CS_PIN,  GPIO_OUTPUT_ACTIVE);
    gpio_pin_configure(gpio0, QSPI_SCK_PIN, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio0, QSPI_IO0_PIN, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio0, QSPI_IO1_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio0, QSPI_IO2_PIN, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio0, QSPI_IO3_PIN, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio0, UART_RX_PIN,  GPIO_INPUT | GPIO_PULL_UP);

    /* --- ADC --- */
    adc_channel_setup(adc, &ch0);
    adc_channel_setup(adc, &ch1);

    /* --- WAKE --- */
    k_sem_init(&wake_sem, 0, 1);

    gpio_init_callback(&wake_cb, wake_isr, BIT(WAKE_PIN_NUM));
    gpio_add_callback(gpio0, &wake_cb);

    gpio_pin_interrupt_configure(gpio0,
                                WAKE_PIN_NUM,
                                GPIO_INT_EDGE_TO_ACTIVE);

    /* ========================================================= */
    while (1) {

        k_sem_take(&wake_sem, K_FOREVER);

        gpio_pin_set(gpio0, SENSOR_PWR_NUM, 1);
        k_sleep(K_MSEC(SENSOR_SETTLE_MS));

        int16_t raw0, raw1;
        read_adc(&raw0, &raw1);

        gpio_pin_set(gpio0, SENSOR_PWR_NUM, 0);

        int32_t mv0 = adc_to_mv_fast(raw0);
        int32_t mv1 = adc_to_mv_fast(raw1);

        emit_pulses(map_pulses(mv0));
        k_busy_wait(GAP_US);
        emit_pulses(map_pulses(mv1));
    }
}