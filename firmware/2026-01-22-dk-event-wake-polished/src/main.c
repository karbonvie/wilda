/*
EVENT_WAKE TEST (nRF52840 DK / Zephyr / NCS 3.2.x)

Purpose:
- Wake on GPIO P0.06 (edge-to-active) with 3 ms storm holdoff.
- On wake: power sensor rail, sample 2 SAADC channels, emit 2 pulse trains on P0.28.
- Optional ISR marker pulse on P0.17.
- Designed so debug features can be compiled out for power measurements.

Analog inputs:
- AIN0 = P0.02 (SAADC AIN0)
- AIN2 = P0.04 (SAADC AIN2)
- Each wake produces 2 trains on P0.28:
    train 1 = AIN0, train 2 = AIN2
- Pulse count is proportional to instantaneous voltage (0..15 pulses for 0..0.6 V ramp).

SAADC mapping note:
- With CONFIG_ADC_CONFIGURABLE_INPUTS=y, .input_positive expects the AIN index (0..7)
  for the nRF SAADC driver. Using shifted SAADC register-field macros can mis-map inputs.

Low-power measurement note:
- Debug pulse trains, LED toggles, and k_busy_wait() dominate active energy and can
  inflate power numbers. Disable them when measuring real energy per cycle.

API compatibility reminder (VS Code / NCS 3.2.x / Zephyr stable):
- This code uses Zephyr public APIs for GPIO/ADC/kernel intended to stay stable.
- Do not use non-existent/obsolete ADC APIs such as adc_calibrate(); SAADC calibration,
  when needed, is requested via adc_sequence.calibrate and performed by adc_read().


--- BUILD + FLASH REFERENCE (TWO MODES) ---

1) After a long break (when forgetting how VS Code, Zephyr, and NCS interact)
- Environment is auto‑loaded via ~/.bashrc and ~/.profile
  (ZEPHYR_BASE and west should already be available)
- Open VS Code normally (desktop launcher or "code .")
- In the nRF Connect sidebar:
    Add Build Configuration (only once per project)
    Select board: nrf52840dk/nrf52840
    Use Build or Flash buttons as needed
- Manual pristine build (only when changing board, toolchain, or major Kconfig):
    west build -b nrf52840dk/nrf52840 --pristine
    west flash

2) Routine daily development ("today")
- Open VS Code, environment already correct
- Use nRF Connect sidebar:
    Build
    Flash
- Manual fallback:
    west build -b nrf52840dk/nrf52840
    (or west build -b nrf52840dk/nrf52840 -p ) -p for pristine
    west flash

This workflow avoids repeated sourcing, avoids environment drift, and keeps
VS Code + Zephyr + NCS stable across reboots.  
*/

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/util.h>
#include <limits.h>

/* ------------------------ Build-time switches ------------------------ */
/* Set to 0 when measuring power (debug output dominates active energy). */
#ifndef ULP_DEBUG_PULSES
#define ULP_DEBUG_PULSES 1
#endif

#ifndef ULP_LED
#define ULP_LED 1
#endif

#ifndef ULP_ISR_MARKER
#define ULP_ISR_MARKER 1
#endif

/* Perform one SAADC calibration at boot using adc_sequence.calibrate. */
#ifndef ULP_ADC_CALIBRATE_ONCE
#define ULP_ADC_CALIBRATE_ONCE 1
#endif

/* ------------------------ DK LED (heartbeat) ------------------------ */
#define LED0_NODE DT_ALIAS(led0)
BUILD_ASSERT(DT_NODE_HAS_STATUS(LED0_NODE, okay), "led0 alias not defined");
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ------------------------ Hard pin definitions ------------------------ */
#define WAKE_PIN_NUM      6   /* P0.06: external digital wake */
#define SENSOR_PWR_NUM   26   /* P0.26: sensor power rail enable */
#define DEBUG_PULSE_NUM  28   /* P0.28: pulse train output */
#define DBG_MARKER_NUM   17   /* P0.17: optional ISR marker pulse */
#define HOLDOFF_MS        3   /* suppress interrupt floods in this window */

/* GPIO0 device */
static const struct device *const gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

/* Runtime GPIO specs (no DT flags; all active-high). */
static struct gpio_dt_spec sensor_pwr;
static struct gpio_dt_spec dbg_pulse;
static struct gpio_dt_spec dbg_marker;
static struct gpio_dt_spec wake_in;

/* ------------------------ Optional: reduce leakage on unused pins ------------------------ */
/* WARNING: Only configure pins you are sure are not used/connected in your setup. */
static void init_unused_pins(void)
{
    const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
    if (!device_is_ready(gpio0) || !device_is_ready(gpio1)) {
        return;
    }

    /* Port 1 (typical floating candidates on DK headers) */
    gpio_pin_configure(gpio1, 10, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio1, 15, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio1,  0, GPIO_INPUT | GPIO_PULL_DOWN);

    /* Port 0: avoid buttons, LEDs, and any pins you actively use */
    gpio_pin_configure(gpio0, 31, GPIO_INPUT | GPIO_PULL_DOWN); /* AIN7 */
    gpio_pin_configure(gpio0, 20, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio0, 22, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio0,  9, GPIO_INPUT | GPIO_PULL_DOWN); /* NFC1 */
    gpio_pin_configure(gpio0, 10, GPIO_INPUT | GPIO_PULL_DOWN); /* NFC2 */
}

/* ------------------------ SAADC configuration ------------------------ */
#define ADC_RESOLUTION   12
#define ADC_ACQ_US       10
#define ADC_GAIN_USED    ADC_GAIN_1_6
#define ADC_REF_USED     ADC_REF_INTERNAL
#define ADC_VREF_MV      600   /* nRF internal reference in mV */
#define MAX_PULSES       15

static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));

/* Buffer for 2 channels read in one adc_read() call */
static int16_t adc_samples[2] __aligned(4);

/* Channel IDs equal AIN indexes (nRF SAADC driver uses channel_id as SAADC channel). */
#define CH_AIN0_ID 0
#define CH_AIN2_ID 2

static const struct adc_channel_cfg adc_ch0_cfg = {
    .gain             = ADC_GAIN_USED,
    .reference        = ADC_REF_USED,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, ADC_ACQ_US),
    .channel_id       = CH_AIN0_ID,
#if defined(CONFIG_ADC_CONFIGURABLE_INPUTS)
    .input_positive   = 0,  /* AIN0 */
#endif
};

static const struct adc_channel_cfg adc_ch2_cfg = {
    .gain             = ADC_GAIN_USED,
    .reference        = ADC_REF_USED,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, ADC_ACQ_US),
    .channel_id       = CH_AIN2_ID,
#if defined(CONFIG_ADC_CONFIGURABLE_INPUTS)
    .input_positive   = 2,  /* AIN2 */
#endif
};

/* Read both channels at once. Samples are stored starting from the lowest channel id. */
static struct adc_sequence adc_seq = {
    .channels    = BIT(CH_AIN0_ID) | BIT(CH_AIN2_ID),
    .buffer      = adc_samples,
    .buffer_size = sizeof(adc_samples),
    .resolution  = ADC_RESOLUTION,
};

/* Convert raw sample -> mV (clamped to 0..ADC_VREF_MV for 0..0.6 V ramps). */
static int32_t adc_raw_to_mv_safe(int16_t raw)
{
    if (raw == INT16_MIN) {
        return 0;
    }

    int32_t mv = raw;

    /* Zephyr helper expects reference voltage in mV (e.g., 600), not an enum. */
    adc_raw_to_millivolts(ADC_VREF_MV, ADC_GAIN_USED, ADC_RESOLUTION, &mv);

    if (mv < 0) mv = 0;
    if (mv > ADC_VREF_MV) mv = ADC_VREF_MV;

    return mv;
}

static int map_pulses_from_mv(int32_t mv)
{
    /* 0..600 mV -> 0..MAX_PULSES (rounded) */
    if (mv <= 0) return 0;
    if (mv >= ADC_VREF_MV) return MAX_PULSES;
    return (mv * MAX_PULSES + (ADC_VREF_MV / 2)) / ADC_VREF_MV;
}

/* Pulse generator (main context only). Busy-waits keep CPU running. */
static void emit_pulses(int count, uint32_t high_us, uint32_t low_us)
{
#if (ULP_DEBUG_PULSES == 1)
    if (count <= 0) return;

    for (int i = 0; i < count; i++) {
        gpio_pin_set_dt(&dbg_pulse, 1);
        k_busy_wait(high_us);
        gpio_pin_set_dt(&dbg_pulse, 0);
        k_busy_wait(low_us);
    }
#else
    ARG_UNUSED(count);
    ARG_UNUSED(high_us);
    ARG_UNUSED(low_us);
#endif
}

/* Single ADC read returning raw samples; INT16_MIN used for error signaling. */
static void read_ain0_ain2_once(int16_t *out0, int16_t *out2)
{
    *out0 = INT16_MIN;
    *out2 = INT16_MIN;

    adc_samples[0] = 0;
    adc_samples[1] = 0;

    int rc = adc_read(adc_dev, &adc_seq);
    if (rc != 0) {
        return;
    }

    /* Sample order follows ascending channel IDs (0, then 2). */
    *out0 = adc_samples[0];
    *out2 = adc_samples[1];
}

/* ------------------------ Wake ISR ------------------------ */
static struct k_sem wake_sem;
static struct gpio_callback wake_cb;

static uint32_t last_isr_us;
static uint16_t suppressed_edges;

static inline uint32_t now_us(void)
{
    return k_cyc_to_us_near32(k_cycle_get_32());
}

static void wake_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    const uint32_t t = now_us();
    if ((t - last_isr_us) < (HOLDOFF_MS * 1000U)) {
        suppressed_edges++;
        return;
    }

    last_isr_us = t;

#if (ULP_ISR_MARKER == 1)
    /* Very short marker pulse to confirm ISR execution. */
    gpio_pin_set_dt(&dbg_marker, 1);
    k_busy_wait(10);
    gpio_pin_set_dt(&dbg_marker, 0);
#endif

    k_sem_give(&wake_sem);
}

/* ------------------------ Main ------------------------ */
#define SENSOR_SETTLE_MS  2
#define GAP_US          500
#define LED_PULSE_MS      1

int main(void)
{
    k_sem_init(&wake_sem, 0, 1);

    /* Init runtime GPIO specs */
    sensor_pwr.port = gpio0_dev; sensor_pwr.pin = SENSOR_PWR_NUM; sensor_pwr.dt_flags = 0;
    dbg_pulse.port  = gpio0_dev; dbg_pulse.pin  = DEBUG_PULSE_NUM; dbg_pulse.dt_flags  = 0;
    dbg_marker.port = gpio0_dev; dbg_marker.pin = DBG_MARKER_NUM;  dbg_marker.dt_flags = 0;
    wake_in.port    = gpio0_dev; wake_in.pin    = WAKE_PIN_NUM;    wake_in.dt_flags    = 0;

    if (!device_is_ready(gpio0_dev) || !device_is_ready(adc_dev)) {
        return 0;
    }

    /* LED port might not be ready if DT is altered; guard it. */
    const bool led_ok = device_is_ready(led.port);

    /* GPIO outputs */
    if (led_ok) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    }
    gpio_pin_configure(sensor_pwr.port, sensor_pwr.pin, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(dbg_pulse.port,  dbg_pulse.pin,  GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(dbg_marker.port, dbg_marker.pin, GPIO_OUTPUT_INACTIVE);

    /* Optional leakage reduction (adjust to your wiring). */
    init_unused_pins();

    /* Wake input
     * Note: internal pull-down causes static current if wake source holds the line high.
     * If your production wake source provides a defined idle state, consider GPIO_INPUT only.
     */
    gpio_pin_configure(wake_in.port, wake_in.pin, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_init_callback(&wake_cb, wake_isr, BIT(wake_in.pin));
    (void)gpio_add_callback(wake_in.port, &wake_cb);
    (void)gpio_pin_interrupt_configure(wake_in.port, wake_in.pin, GPIO_INT_EDGE_TO_ACTIVE);

    /* SAADC channel setup once (avoid per-wake overhead). */
    (void)adc_channel_setup(adc_dev, &adc_ch0_cfg);
    (void)adc_channel_setup(adc_dev, &adc_ch2_cfg);

#if (ULP_ADC_CALIBRATE_ONCE == 1)
    /* One-time calibration using Zephyr-supported mechanism (adc_sequence.calibrate). */
    adc_seq.calibrate = true;
    (void)adc_read(adc_dev, &adc_seq);   /* discard results */
    adc_seq.calibrate = false;
#endif

    last_isr_us = 0;
    suppressed_edges = 0;

    for (;;) {
        /* Sleep until external wake */
        k_sem_take(&wake_sem, K_FOREVER);

        /* Power sensor rail only for the active window */
        gpio_pin_set(sensor_pwr.port, sensor_pwr.pin, 1);
        k_sleep(K_MSEC(SENSOR_SETTLE_MS));

        /* Sample both analog channels */
        int16_t raw0, raw2;
        read_ain0_ain2_once(&raw0, &raw2);

        /* Convert and emit pulse trains (debug-only) */
        const int32_t mv0 = adc_raw_to_mv_safe(raw0);
        const int32_t mv2 = adc_raw_to_mv_safe(raw2);

        emit_pulses(map_pulses_from_mv(mv0), 200, 200);
        k_busy_wait(GAP_US);
        emit_pulses(map_pulses_from_mv(mv2), 200, 200);

        /* Power down sensor rail immediately */
        gpio_pin_set(sensor_pwr.port, sensor_pwr.pin, 0);

#if (ULP_LED == 1)
        if (led_ok) {
            gpio_pin_set_dt(&led, 1);
            k_busy_wait(LED_PULSE_MS * 1000U);
            gpio_pin_set_dt(&led, 0);
        }
#endif
    }
}
