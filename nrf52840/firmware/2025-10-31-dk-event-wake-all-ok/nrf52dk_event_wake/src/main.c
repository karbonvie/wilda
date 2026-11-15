/*
PORTED CODE: EVENT_WAKE MINI PROJECT (nRF52840 DK)
--- PURPOSE ---
Implements event-driven wake on P0.06 with 3ms storm protection (HOLDOFF_MS),
keeping the low-power SAADC sequence, debug pulses, and LED heartbeat.
RTC remains stopped for idle floor isolation.

--- USAGE NOTES ---
- Connect AD2 WaveGen (W1) 0→3 V square wave to P0.06 (Header P20, pin 2).
- Probe P0.28 (Header P17, pin 2) for composite debug pulses.
- Probe P0.17 (Header P17, pin 1) for debug marker pulse (confirming ISR runs).
- Use PPK2 to measure current floor during k_sem_take(K_FOREVER) state (target < 30 uA).

--- CRITICAL ADAPTATION ---
Pin definitions for wake-in, sensor power, and debug are now HARDCODED
using pin numbers to bypass Devicetree overlay syntax corruption issues.
*/

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/sys/util.h>
#include <limits.h>
#include <hal/nrf_saadc.h>

/* ==== LED (heartbeat) - Uses standard Devicetree alias 'led0' ==== */
#define LED0_NODE DT_ALIAS(led0)
BUILD_ASSERT(DT_NODE_HAS_STATUS(LED0_NODE, okay), "led0 alias not defined");
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ==== RTC selection (left stopped for low-power test) ==== */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(rtc2), okay)
  #define RTC_NODE DT_NODELABEL(rtc2)
#elif DT_NODE_HAS_STATUS(DT_NODELABEL(rtc1), okay)
  #define RTC_NODE DT_NODELABEL(rtc1)
#elif DT_NODE_HAS_STATUS(DT_NODELABEL(rtc0), okay)
  #define RTC_NODE DT_NODELABEL(rtc0)
#else
  #error "No RTC instance available"
#endif
static const struct device *const rtc = DEVICE_DT_GET(RTC_NODE);

/* ==== HARDCODED CUSTOM PIN DEFINITIONS (BYPASSING CORRUPTED OVERLAY) ==== */
#define WAKE_PIN_NUM     6    // P0.06: External stimulus input
#define SENSOR_PWR_NUM  26    // P0.26: Sensor power output
#define DEBUG_PULSE_NUM 28    // P0.28: Composite debug pulse output
#define DBG_MARKER_NUM  17    // P0.17: ISR execution marker output
#define HOLDOFF_MS   3  /* suppress floods within this window (3ms) */

// Get the GPIO controller device pointer once (P0 is needed by all custom pins)
const struct device *gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

// Custom GPIO structs using hardcoded pin numbers
static const struct gpio_dt_spec sensor_pwr = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    .pin  = SENSOR_PWR_NUM,
};
static const struct gpio_dt_spec dbg_marker = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    .pin  = DBG_MARKER_NUM,
};
static const struct gpio_dt_spec dbg_pulse = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    .pin  = DEBUG_PULSE_NUM,
};
static const struct gpio_dt_spec wake_in = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    .pin  = WAKE_PIN_NUM,
};


/* ==== Configure unused pins as inputs with pulldown to avoid leakage ==== */
static void init_unused_pins(void)
{
    const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
    if (!device_is_ready(gpio0) || !device_is_ready(gpio1)) return;

    /* Port 1 pins */
    gpio_pin_configure(gpio1, 10, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio1, 15, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio1,  0, GPIO_INPUT | GPIO_PULL_DOWN);

    /* Port 0 pins: EXCLUDE used pins, buttons, and primary LEDs (P0.13, P0.14, P0.15, P0.16) */
    gpio_pin_configure(gpio0, 31, GPIO_INPUT | GPIO_PULL_DOWN); /* AIN7 */
    gpio_pin_configure(gpio0, 20, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio0, 22, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_configure(gpio0,  9, GPIO_INPUT | GPIO_PULL_DOWN); /* NFC1 */
    gpio_pin_configure(gpio0, 10, GPIO_INPUT | GPIO_PULL_DOWN); /* NFC2 */
}


/* ==== SAADC config (sequential single-channel reads) - NO CHANGE ==== */
#define ADC_RESOLUTION  12
#define ADC_ACQ_US      10
static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
static int16_t sample_ch0 __aligned(4);
static int16_t sample_ch5 __aligned(4);

/* Channel 0: AIN0 (P0.02) */
#define AIN0_CH_ID  0
static const struct adc_channel_cfg adc_ch0_cfg = {
    .gain             = ADC_GAIN_1_6,
    .reference        = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, ADC_ACQ_US),
    .channel_id       = AIN0_CH_ID,
#if defined(CONFIG_ADC_CONFIGURABLE_INPUTS)
    .input_positive   = SAADC_CH_PSELP_PSELP_AnalogInput0
#endif
};
static struct adc_sequence adc_seq_ch0 = {
    .channels    = BIT(AIN0_CH_ID),
    .buffer      = &sample_ch0,
    .buffer_size = sizeof(sample_ch0),
    .resolution  = ADC_RESOLUTION,
};

/* Channel 5: AIN5 (P0.29) */
#define AIN5_CH_ID  5
static const struct adc_channel_cfg adc_ch5_cfg = {
    .gain             = ADC_GAIN_1_6,
    .reference        = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, ADC_ACQ_US),
    .channel_id       = AIN5_CH_ID,
#if defined(CONFIG_ADC_CONFIGURABLE_INPUTS)
    .input_positive   = SAADC_CH_PSELP_PSELP_AnalogInput5
#endif
};
static struct adc_sequence adc_seq_ch5 = {
    .channels    = BIT(AIN5_CH_ID),
    .buffer      = &sample_ch5,
    .buffer_size = sizeof(sample_ch5),
    .resolution  = ADC_RESOLUTION,
};

/* ==== Ring buffer (two-channel reading + diagnostics) ==== */
#define BUF_LEN 32
struct reading2 {
    uint32_t ticks;
    int16_t  ch0;
    int16_t  ch5;
    uint8_t  cause;
    uint16_t latency_us;
    uint16_t suppressed;
};
static struct reading2 ring_buf[BUF_LEN];
static size_t buf_head = 0;

/* ==== Wake semaphore ==== */
static struct k_sem wake_sem;

/* ==== ADC debug and helpers (NO CHANGE) ==== */
static int map_pulses(int16_t adc_val)
{
    if (adc_val < 0) return 0;
    int pulses = (adc_val + 128) / 256;
    if (pulses > 15) pulses = 15;
    return pulses;
}

static void emit_pulses(int count, uint32_t high_us, uint32_t low_us)
{
    if (count <= 0) return;
    for (int i = 0; i < count; i++) {
        gpio_pin_set_dt(&dbg_pulse, 1);
        k_busy_wait(high_us);
        gpio_pin_set_dt(&dbg_pulse, 0);
        k_busy_wait(low_us);
    }
}

static int16_t read_ain0_once(void)
{
    if (adc_channel_setup(adc_dev, &adc_ch0_cfg) != 0) return INT16_MIN;
    int rc = adc_read(adc_dev, &adc_seq_ch0);
    nrf_saadc_disable(NRF_SAADC);
    if (rc != 0) return INT16_MIN;
    return sample_ch0;
}

static int16_t read_ain5_once(void)
{
    if (adc_channel_setup(adc_dev, &adc_ch5_cfg) != 0) return INT16_MIN;
    int rc = adc_read(adc_dev, &adc_seq_ch5);
    nrf_saadc_disable(NRF_SAADC);
    if (rc != 0) return INT16_MIN;
    return sample_ch5;
}

/* ==== External stimulus wake: GPIO input and ISR with storm protection (P0.06) ==== */
static uint32_t last_isr_us = 0;
static uint16_t suppressed_edges = 0;

static inline uint32_t now_us(void)
{
    return k_cyc_to_us_near32(k_cycle_get_32());
}

static struct gpio_callback wake_cb;

static void wake_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);

    // Pulse the Debug Marker pin P0.17 for instant scope visibility
    gpio_pin_set_dt(&dbg_marker, 1);
    k_busy_wait(100);
    gpio_pin_set_dt(&dbg_marker, 0);

    // --- WAKE LOGIC ---
    const uint32_t t_us = now_us();
    const uint32_t dt = t_us - last_isr_us;

    if (dt < (HOLDOFF_MS * 1000U)) { // Storm protection (suppress edges within 3ms)
        suppressed_edges++;
        return;
    }

    // Accept this wake; record time and signal main loop.
    last_isr_us = t_us;
    k_sem_give(&wake_sem); // *** CRITICAL: Unblocks the main thread from deep sleep ***
}

/* ==== Main ==== */
#define PULSE_MS   1
#define SETTLE_MS  2
#define GAP_US   500

int main(void)
{
    k_sem_init(&wake_sem, 0, 1);

    /* Check all critical devices are ready before proceeding */
    if (!device_is_ready(led.port) || !device_is_ready(rtc) ||
        !device_is_ready(adc_dev) || !device_is_ready(gpio0_dev)) {
        // Check main components: LED, RTC (for counter), ADC, and GPIO Port 0
        return 0;
    }

    /* Configure custom hardcoded Outputs (P0.26, P0.28, P0.17) */
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);                 // P0.13 (LED1)
    gpio_pin_configure_dt(&sensor_pwr, GPIO_OUTPUT_INACTIVE);          // P0.26 (PWR)
    gpio_pin_configure_dt(&dbg_pulse, GPIO_OUTPUT_INACTIVE);           // P0.28 (DBG)
    gpio_pin_configure_dt(&dbg_marker, GPIO_OUTPUT_INACTIVE);          // P0.17 (MARKER)

    /* Unused pins low-leakage */
    init_unused_pins();

    /* RTC is intentionally NOT started for idle current isolation test. */

    /* Configure wake input (P0.06): pull-down + interrupt on rising edge */
    gpio_pin_configure_dt(&wake_in, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_init_callback(&wake_cb, wake_isr, BIT(wake_in.pin));
    gpio_add_callback(wake_in.port, &wake_cb);

    // Configure interrupt on the hardcoded pin (P0.06)
    gpio_pin_interrupt_configure(wake_in.port, wake_in.pin, GPIO_INT_EDGE_TO_ACTIVE);

    /* Initialize diagnostics */
    last_isr_us = 0;
    suppressed_edges = 0;

    for (;;) {
        /* Wait for external stimulus (GPIO edge accepted by ISR) */
        /*
         * This is the core of the low-power logic. It suspends the main thread
         * and puts the CPU into the lowest possible System ON sleep state (deep sleep)
         * until the 'wake_sem' is given by the interrupt (wake_isr).
         * This is where the low current floor is expected.
         */
        k_sem_take(&wake_sem, K_FOREVER);

        /* Power sensor(s) ON and allow settle */
        gpio_pin_set_dt(&sensor_pwr, 1);
        k_sleep(K_MSEC(SETTLE_MS));

        /* Take two independent single-channel SAADC samples */
        int16_t val0 = read_ain0_once();
        int16_t val5 = read_ain5_once();

        /* Measure approximate latency */
        uint32_t t_now_us = now_us();
        uint16_t latency = (t_now_us >= last_isr_us) ? (uint16_t)(t_now_us - last_isr_us) : 0;

        /* Store in ring buffer with diagnostics */
        uint32_t now_ticks = 0;
        (void)counter_get_value(rtc, &now_ticks);
        ring_buf[buf_head].ticks      = now_ticks;
        ring_buf[buf_head].ch0        = val0;
        ring_buf[buf_head].ch5        = val5;
        ring_buf[buf_head].cause      = 1;
        ring_buf[buf_head].latency_us = latency;
        ring_buf[buf_head].suppressed = suppressed_edges;
        buf_head = (buf_head + 1) % BUF_LEN;

        /* Composite debug pulses: pulse width shows ADC value */
        emit_pulses(map_pulses(val0), 200, 200);
        k_busy_wait(GAP_US);
        emit_pulses(map_pulses(val5), 200, 200);

        /* Power sensor(s) OFF */
        gpio_pin_set_dt(&sensor_pwr, 0);

        /* LED heartbeat pulse (very short) */
        gpio_pin_set_dt(&led, 1);
        k_busy_wait(PULSE_MS * 1000U);
        gpio_pin_set_dt(&led, 0);

        /* Loop back to wait for next external stimulus */
    }

    return 0;
}