/*
main.c

WildlifeThrive – Event-driven low-power node verification firmware
Target MCU: nRF52840
Typical board: Adafruit Feather nRF52840 Express
Framework: Zephyr / Nordic NCS

--------------------------------------------------------------------
Main purpose of this firmware:
To validate the core low-power architecture that future WildlifeThrive tags will use.
--------------------------------------------------------------------
*/

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/util.h>
#include <hal/nrf_saadc.h>
#include <hal/nrf_power.h>  
#include <limits.h>

#include "board_pins.h"

/* --- Build configuration switches --- */
#ifndef ULP_DEBUG_PULSES
#define ULP_DEBUG_PULSES 1
#endif

#ifndef ULP_ADC_CALIBRATE_ONCE
#define ULP_ADC_CALIBRATE_ONCE 1
#endif

#ifndef ULP_ISR_MARKER
#define ULP_ISR_MARKER ULP_ISR_MARKER_DEFAULT
#endif

/* --- Timing constants --- */
#define HOLDOFF_MS 3
#define SENSOR_SETTLE_MS 2
#define GAP_US 500

/* --- ADC configuration --- */
#define ADC_RESOLUTION 12
#define ADC_ACQ_US 10
#define ADC_GAIN_USED ADC_GAIN_1_6
#define ADC_REF_USED ADC_REF_INTERNAL
#define ADC_VREF_MV 600
#define MAX_PULSES 15

/* --- Device handles --- */
static const struct device *gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));

/* --- ADC buffers and sequence --- */
static int16_t adc_samples[2];

static const struct adc_channel_cfg adc_ch0_cfg = {
    .gain = ADC_GAIN_USED,
    .reference = ADC_REF_USED,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, ADC_ACQ_US),
    .channel_id = 0,
    .input_positive = SENSOR1_ADC_INPUT
};

static const struct adc_channel_cfg adc_ch1_cfg = {
    .gain = ADC_GAIN_USED,
    .reference = ADC_REF_USED,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, ADC_ACQ_US),
    .channel_id = 1,
    .input_positive = SENSOR2_ADC_INPUT
};

static struct adc_sequence adc_seq = {
    .channels = BIT(0) | BIT(1),
    .buffer = adc_samples,
    .buffer_size = sizeof(adc_samples),
    .resolution = ADC_RESOLUTION,
};

/* --- Synchronization objects --- */
static struct k_sem wake_sem;
static struct gpio_callback wake_cb;
static uint32_t last_isr_time;

static inline uint32_t now_us(void) {
    return k_cyc_to_us_near32(k_cycle_get_32());
}

static int32_t adc_raw_to_mv_safe(int16_t raw) {
    if (raw == INT16_MIN) return 0;
    int32_t mv = raw;
    adc_raw_to_millivolts(ADC_VREF_MV, ADC_GAIN_USED, ADC_RESOLUTION, &mv);
    if (mv < 0) mv = 0;
    if (mv > 3600) mv = 3600;
    return mv;
}

static int map_pulses_from_mv(int32_t mv) {
    if (mv <= 0) return 0;
    if (mv >= 3600) return MAX_PULSES;
    return (mv * MAX_PULSES + 1800) / 3600;
}

static void emit_pulses(int count, uint32_t high_us, uint32_t low_us) {
#if (ULP_DEBUG_PULSES == 1)
    for (int i = 0; i < count; i++) {
        gpio_pin_set(gpio0_dev, DEBUG_PULSE_NUM, 1);
        k_busy_wait(high_us);
        gpio_pin_set(gpio0_dev, DEBUG_PULSE_NUM, 0);
        k_busy_wait(low_us);
    }
#else
    ARG_UNUSED(count);
    ARG_UNUSED(high_us);
    ARG_UNUSED(low_us);
#endif
}

static void read_adc(int16_t *out0, int16_t *out2) {
    *out0 = INT16_MIN;
    *out2 = INT16_MIN;

    /* -------- Channel 0 -------- */
    int16_t sample0 = 0;
    struct adc_sequence seq0 = {0};
    seq0.channels = BIT(0);
    seq0.buffer = &sample0;
    seq0.buffer_size = sizeof(sample0);
    seq0.resolution = ADC_RESOLUTION;

    adc_read(adc_dev, &seq0); // throwaway read
    if (adc_read(adc_dev, &seq0) == 0) {
        *out0 = sample0;
        adc_samples[0] = sample0;
    }

    k_busy_wait(20); // allow SAADC to settle

    /* -------- Channel 1 -------- */
    int16_t sample1 = 0;
    struct adc_sequence seq1 = {0};
    seq1.channels = BIT(1);
    seq1.buffer = &sample1;
    seq1.buffer_size = sizeof(sample1);
    seq1.resolution = ADC_RESOLUTION;

    adc_read(adc_dev, &seq1); // throwaway read
    if (adc_read(adc_dev, &seq1) == 0) {
        *out2 = sample1;
        adc_samples[1] = sample1;
    }
}

static void wake_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    uint32_t t = now_us();
    if ((t - last_isr_time) < (HOLDOFF_MS * 1000U)) return;
    last_isr_time = t;

#if (ULP_ISR_MARKER == 1)
    gpio_pin_set(gpio0_dev, DBG_MARKER_NUM, 1);
    k_busy_wait(10);
    gpio_pin_set(gpio0_dev, DBG_MARKER_NUM, 0);
#endif

    k_sem_give(&wake_sem);
}

/*
--------------------------------------------------------------------
Hardware Leakage Prevention (Crucial for ultra-low power)
--------------------------------------------------------------------
When peripherals are disabled in the overlay, Zephyr abandons their pins.
If those pins are connected to physical components (like the Feather's 
onboard flash or serial chip), they float. A floating CMOS pin oscillates,
drawing massive "shoot-through" current (the 800uA spikes you see).
This function forces them into stable states.
*/
static void park_leak_prone_pins(void) {
#if defined(CONFIG_BOARD_ADAFRUIT_FEATHER_NRF52840)
    /* 1. Park the UART RX Pin. 
    Without the Zephyr serial driver, this pin floats. We pull it UP
    so it stays stable at 3.3V and draws zero current.
    */
    gpio_pin_configure(gpio0_dev, UART_RX_PIN, GPIO_INPUT | GPIO_PULL_UP);

    /*
    2. Secure the QSPI External Flash Chip.
    Since we disabled CONFIG_FLASH, Zephyr won't talk to the flash chip.
    If the Chip Select (CS) pin floats low, the flash chip wakes up.
    We force CS HIGH to keep the flash chip in Standby mode (~15uA).
    */
    gpio_pin_configure(gpio0_dev, QSPI_CS_PIN, GPIO_OUTPUT_ACTIVE); // Active High

    /*
    3. Park the remaining QSPI data/clock lines.
    Pulling them down prevents them from floating and triggering the 
    nRF52840's internal input buffers.
    */
    gpio_pin_configure(gpio0_dev, QSPI_SCK_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio0_dev, QSPI_IO0_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio0_dev, QSPI_IO1_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio0_dev, QSPI_IO2_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio0_dev, QSPI_IO3_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
#endif
}

int main(void) {
    if (!device_is_ready(gpio0_dev)) return 0;
    if (!device_is_ready(adc_dev)) return 0;

    /* Turn on the internal DCDC converter for 40% CPU power savings */
    nrf_power_dcdcen_set(NRF_POWER, true); // <--- ADD THIS LINE

    /* Initialize synchronization and park the floating pins immediately */
    k_sem_init(&wake_sem, 0, 1);
    park_leak_prone_pins();

    /* Configure logic pins */
    gpio_pin_configure(gpio0_dev, SENSOR_PWR_NUM, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio0_dev, DEBUG_PULSE_NUM, GPIO_OUTPUT_INACTIVE);

#if (ULP_ISR_MARKER == 1)
    gpio_pin_configure(gpio0_dev, DBG_MARKER_NUM, GPIO_OUTPUT_INACTIVE);
#endif

#if BOARD_USE_UNUSED_PIN_INIT
    init_unused_pins();
#endif

    /* Configure Wake Pin and Interrupt */
    gpio_pin_configure(gpio0_dev, WAKE_PIN_NUM, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_init_callback(&wake_cb, wake_isr, BIT(WAKE_PIN_NUM));
    gpio_add_callback(gpio0_dev, &wake_cb);
    gpio_pin_interrupt_configure(gpio0_dev, WAKE_PIN_NUM, GPIO_INT_EDGE_TO_ACTIVE);

    /* Setup ADC Channels */
    adc_channel_setup(adc_dev, &adc_ch0_cfg);
    adc_channel_setup(adc_dev, &adc_ch1_cfg);

    /* Main Sleep/Wake Loop */
    while (1) {
        // Drop into System ON sleep (WFE). 
        // With DCDC on, UART off, and pins parked, this should pull < 20uA.
        k_sem_take(&wake_sem, K_FOREVER);

        gpio_pin_set(gpio0_dev, SENSOR_PWR_NUM, 1);
        k_sleep(K_MSEC(SENSOR_SETTLE_MS));

        int16_t raw0, raw1;
        read_adc(&raw0, &raw1);

        int32_t mv0 = adc_raw_to_mv_safe(raw0);
        int32_t mv1 = adc_raw_to_mv_safe(raw1);

        emit_pulses(map_pulses_from_mv(mv0), 200, 200);
        k_busy_wait(GAP_US);
        emit_pulses(map_pulses_from_mv(mv1), 200, 200);

        gpio_pin_set(gpio0_dev, SENSOR_PWR_NUM, 0);
    }
}