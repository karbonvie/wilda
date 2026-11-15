/*
main.c - pwrlow Tier4 next big step: page batching + checkpointed resume metadata

What this version changes:
- keep the proven low-level bitbang QSPI transport that already worked on Feather
- keep the hardened 32-byte record format (magic + type + seq + CRC)
- stop writing flash on every wake event; buffer one full 256-byte page in RAM first
- stop scanning the whole historical log on every boot; store resume metadata in a reserved flash sector

Why this is the right next step now:
- flash transport is already proven by the previous hybrid build
- the biggest remaining power hit was boot-time full scan after long runs
- the second biggest hit was direct write on every event
- this version attacks both without redesigning the whole logger again

Expected behavior:
- normal wake path still does: wake -> ADC -> pulse output -> log sample
- samples are buffered in RAM; nothing is written to flash until 8 records are collected
- every successful page flush emits 3 short extra debug pulses
- every failed page flush or failed checkpoint emits 10 short extra debug pulses
- maintenance mode dumps only flushed records; up to 7 newest records can be lost across reset by design
- boot should be much faster after long runs because it resumes from metadata instead of rescanning all old pages

About erase:
- YES, still do one full flash erase once when switching to this format/layout from older firmware
- enable flash_chip_erase() once, flash firmware, let normal mode boot once, then comment it back out and reflash
- do NOT leave erase enabled during real runs

Good tests to do with this firmware:
- your usual 500 ms wake cycle, then dump; verify records continue and # resume_mode says checkpoint
- faster wake cycle, e.g. 100 ms; verify page flushes happen more often and no 10-pulse errors appear
- slower wake cycle, e.g. 2 s or more; verify logging still works, knowing last partial page is RAM-only until filled
- leave it running long enough to create many pages, then power-cycle and check boot current/time is much lower than before
- vary AIN voltages and waveforms; many zeros are still normal if you sample at one instant and the signal is near zero then

If it fails, what to look for first:
- 10 extra pulses after a page should flush -> flash transport/checkpoint problem, not ADC problem
- # resume_mode,fallback_scan after a normal previous run -> checkpoint metadata was not written or not valid
- # records stays low while pulses continue -> likely buffering not flushing because not enough events yet
- # records non-zero but newest few expected events missing after reset -> normal with page batching; they were still only in RAM

Other files:
- no conceptual changes needed in prj.conf, board_pins.h or overlay for this step
- keep the same working maintenance-mode USB setup you already used successfully
*/


#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_power.h>
#include <hal/nrf_qspi.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <limits.h>

#include "board_pins.h"

#define SENSOR_SETTLE_MS              2U
#define GAP_US                        500U
#define ADC_RESOLUTION                12U
#define ADC_ACQ_US                    10U
#define MAX_PULSES                    15

#define FLASH_SIZE_BYTES              (1024U * 1024U)   /* 8 Mbit = 1 MiB external flash */
#define FLASH_PAGE_SIZE               256U              /* page program granularity */
#define FLASH_SECTOR_SIZE             4096U             /* 4 KiB sector erase granularity */
#define FLASH_TIMEOUT_WEL_MS          4U
#define FLASH_TIMEOUT_PROG_MS         40U
#define FLASH_TIMEOUT_ERASE_MS        10000U
#define FLASH_SR_WIP                  BIT(0)
#define FLASH_SR_WEL                  BIT(1)

#define META_SECTOR_BASE              (FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)   /* reserve last 4 KiB for resume metadata */
#define DATA_FLASH_BYTES              META_SECTOR_BASE                          /* data log occupies everything below metadata */

#define LOG_MAGIC                     0xA55AU
#define LOG_TAIL                      0x5AA5U
#define LOG_VERSION                   1U
#define LOG_TYPE_DATA                 1U
#define LOG_TYPE_TIME_ANCHOR          2U

#define META_MAGIC                    0xC33CU
#define META_TAIL                     0x3CC3U
#define META_VERSION                  1U

#define RTC_SOURCE_NONE               0U
#define RTC_SOURCE_EXT_RTC            1U   /* future RV-3028-C7 or similar */
#define RTC_SOURCE_LORAWAN            2U   /* future nearby LoRaWAN / TTN source */
#define RTC_SOURCE_USB_SERVICE        3U   /* reserved for future service-set UTC */

#define LOG_FLAG_TIME_BOOT            BIT(0)
#define LOG_FLAG_TIME_PERIODIC        BIT(1)
#define TIME_ANCHOR_PERIOD_MS         (60U * 60U * 1000U)

typedef struct __packed {
    uint16_t magic;
    uint8_t  type;
    uint8_t  version;
    uint32_t seq;
    uint32_t uptime_ms;
    uint32_t utc_s;
    int16_t  sensor1_mv;
    int16_t  sensor2_mv;
    uint8_t  rtc_source;
    uint8_t  rtc_valid;
    uint16_t flags;
    uint32_t reserved0;
    uint16_t crc16;
    uint16_t tail;
} log_record_t;

typedef struct __packed {
    uint16_t magic;
    uint8_t  version;
    uint8_t  reserved_a;
    uint32_t generation;
    uint32_t write_ptr;
    uint32_t next_seq;
    uint32_t anchor_utc_s;
    uint32_t anchor_uptime_ms;
    uint8_t  rtc_source;
    uint8_t  rtc_valid;
    uint16_t reserved_b;
    uint16_t crc16;
    uint16_t tail;
} meta_record_t;

BUILD_ASSERT(sizeof(log_record_t) == 32, "log_record_t must stay 32 bytes");
BUILD_ASSERT(sizeof(meta_record_t) == 32, "meta_record_t must stay 32 bytes");
BUILD_ASSERT((FLASH_PAGE_SIZE % sizeof(log_record_t)) == 0, "record size must divide page size");
BUILD_ASSERT((FLASH_SECTOR_SIZE % sizeof(meta_record_t)) == 0, "meta record size must divide metadata sector");

#define PAGE_BUFFER_RECORDS           (FLASH_PAGE_SIZE / sizeof(log_record_t))
#define META_SLOTS                    (FLASH_SECTOR_SIZE / sizeof(meta_record_t))

struct rtc_anchor_ctx {
    bool valid;
    uint32_t unix_s;
    uint32_t uptime_ms;
    uint32_t last_anchor_ms;
    uint8_t source;
    bool boot_anchor_pending;
};

static const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *adc   = DEVICE_DT_GET(DT_NODELABEL(adc));

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

static struct k_sem wake_sem;
static struct gpio_callback wake_cb;

static struct rtc_anchor_ctx rtc_ctx;
static log_record_t page_buffer[PAGE_BUFFER_RECORDS];
static uint8_t page_buffer_count;
static uint32_t flash_write_ptr;
static uint32_t next_seq;
static bool log_storage_full;
static uint32_t meta_generation;
static uint16_t meta_next_slot;
static bool resume_used_fallback_scan;

static inline int32_t adc_to_mv_fast(int16_t raw)
{
    if (raw <= 0) {
        return 0;
    }
    return ((int32_t)raw * 3600) / 4096;   /* 0.6 V ref, gain 1/6, 12-bit */
}

static inline int map_pulses(int32_t mv)
{
    if (mv <= 0) {
        return 0;
    }
    return (int)((mv * MAX_PULSES) / 3600);
}

static inline bool time_anchor_due(uint32_t now_ms)
{
    if (!rtc_ctx.valid) {
        return false;
    }
    return (uint32_t)(now_ms - rtc_ctx.last_anchor_ms) >= TIME_ANCHOR_PERIOD_MS;
}

static uint32_t rtc_estimate_unix_from_uptime(uint32_t uptime_ms)
{
    if (!rtc_ctx.valid) {
        return 0U;
    }
    return rtc_ctx.unix_s + ((uptime_ms - rtc_ctx.uptime_ms) / 1000U);
}

static void rtc_seed_known_time(uint32_t unix_s, uint8_t source)
{
    rtc_ctx.valid = true;
    rtc_ctx.unix_s = unix_s;
    rtc_ctx.uptime_ms = k_uptime_get_32();
    rtc_ctx.last_anchor_ms = 0U;              /* force one boot anchor when RTC becomes real later */
    rtc_ctx.source = source;
    rtc_ctx.boot_anchor_pending = true;
}

static bool rtc_try_seed_on_boot(uint32_t *unix_s, uint8_t *source)
{
    ARG_UNUSED(unix_s);
    ARG_UNUSED(source);
    return false;                             /* foundation hook only; external RTC not wired yet */
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8U; bit++) {
            if (crc & 0x8000U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static void record_finalize(log_record_t *rec)
{
    rec->crc16 = crc16_ccitt((const uint8_t *)rec, offsetof(log_record_t, crc16));
    rec->tail = LOG_TAIL;
}

static bool record_is_erased(const log_record_t *rec)
{
    const uint8_t *p = (const uint8_t *)rec;

    for (size_t i = 0; i < sizeof(*rec); i++) {
        if (p[i] != 0xFFU) {
            return false;
        }
    }

    return true;
}

static bool record_is_valid(const log_record_t *rec)
{
    if (rec->magic != LOG_MAGIC) {
        return false;
    }
    if (rec->tail != LOG_TAIL) {
        return false;
    }
    if (rec->version != LOG_VERSION) {
        return false;
    }
    if ((rec->type != LOG_TYPE_DATA) && (rec->type != LOG_TYPE_TIME_ANCHOR)) {
        return false;
    }
    return rec->crc16 == crc16_ccitt((const uint8_t *)rec, offsetof(log_record_t, crc16));
}

static void record_make_data(log_record_t *rec, uint32_t seq, uint32_t uptime_ms, int16_t s1_mv, int16_t s2_mv)
{
    memset(rec, 0, sizeof(*rec));

    rec->magic = LOG_MAGIC;
    rec->type = LOG_TYPE_DATA;
    rec->version = LOG_VERSION;
    rec->seq = seq;
    rec->uptime_ms = uptime_ms;
    rec->utc_s = 0U;                          /* reconstructed later from time-anchor records */
    rec->sensor1_mv = s1_mv;
    rec->sensor2_mv = s2_mv;
    rec->rtc_source = rtc_ctx.source;
    rec->rtc_valid = rtc_ctx.valid ? 1U : 0U;
    rec->flags = 0U;
    rec->reserved0 = 0U;

    record_finalize(rec);
}

static void record_make_time_anchor(log_record_t *rec, uint32_t seq, uint16_t flags)
{
    memset(rec, 0, sizeof(*rec));

    rec->magic = LOG_MAGIC;
    rec->type = LOG_TYPE_TIME_ANCHOR;
    rec->version = LOG_VERSION;
    rec->seq = seq;
    rec->uptime_ms = rtc_ctx.uptime_ms;
    rec->utc_s = rtc_ctx.unix_s;
    rec->sensor1_mv = INT16_MAX;
    rec->sensor2_mv = INT16_MAX;
    rec->rtc_source = rtc_ctx.source;
    rec->rtc_valid = rtc_ctx.valid ? 1U : 0U;
    rec->flags = flags;
    rec->reserved0 = 0U;

    record_finalize(rec);
}

static void meta_finalize(meta_record_t *meta)
{
    meta->crc16 = crc16_ccitt((const uint8_t *)meta, offsetof(meta_record_t, crc16));
    meta->tail = META_TAIL;
}

static bool meta_is_erased(const meta_record_t *meta)
{
    const uint8_t *p = (const uint8_t *)meta;

    for (size_t i = 0; i < sizeof(*meta); i++) {
        if (p[i] != 0xFFU) {
            return false;
        }
    }

    return true;
}

static bool meta_is_valid(const meta_record_t *meta)
{
    if (meta->magic != META_MAGIC) {
        return false;
    }
    if (meta->tail != META_TAIL) {
        return false;
    }
    if (meta->version != META_VERSION) {
        return false;
    }
    if (meta->write_ptr > DATA_FLASH_BYTES) {
        return false;
    }
    if ((meta->write_ptr % FLASH_PAGE_SIZE) != 0U) {
        return false;
    }
    return meta->crc16 == crc16_ccitt((const uint8_t *)meta, offsetof(meta_record_t, crc16));
}

static int read_adc(int16_t *v0, int16_t *v1)
{
    int ret;
    int16_t sample = 0;

    struct adc_sequence seq = {
        .buffer = &sample,
        .buffer_size = sizeof(sample),
        .resolution = ADC_RESOLUTION
    };

    seq.channels = BIT(0);
    ret = adc_read(adc, &seq);
    if (ret < 0) {
        return ret;
    }
    ret = adc_read(adc, &seq);                /* throw away first read after mux/select change */
    if (ret < 0) {
        return ret;
    }
    *v0 = sample;

    k_busy_wait(20);

    seq.channels = BIT(1);
    ret = adc_read(adc, &seq);
    if (ret < 0) {
        return ret;
    }
    ret = adc_read(adc, &seq);
    if (ret < 0) {
        return ret;
    }
    *v1 = sample;

    return 0;
}

static void emit_pulses(int count)
{
    for (int i = 0; i < count; i++) {
        gpio_pin_set(gpio0, DEBUG_PULSE_NUM, 1);
        k_busy_wait(200);
        gpio_pin_set(gpio0, DEBUG_PULSE_NUM, 0);
        k_busy_wait(200);
    }
}

static void wake_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
    k_sem_give(&wake_sem);                    /* keep ISR tiny; do real work in thread context */
}

static int qspi_claim_pins(void)
{
    int ret;

    if (NRF_QSPI->ENABLE != 0U) {             /* old proven trick: forcibly release hardware QSPI owner */
        NRF_QSPI->TASKS_DEACTIVATE = 1U;
        k_busy_wait(10);
        NRF_QSPI->ENABLE = 0U;
        k_busy_wait(10);
    }

    ret = gpio_pin_configure(gpio0, QSPI_CS_PIN, GPIO_OUTPUT_ACTIVE);    /* CS idle high */
    if (ret < 0) return ret;

    ret = gpio_pin_configure(gpio0, QSPI_SCK_PIN, GPIO_OUTPUT_INACTIVE); /* SCK idle low */
    if (ret < 0) return ret;

    ret = gpio_pin_configure(gpio0, QSPI_IO0_PIN, GPIO_OUTPUT_INACTIVE); /* MOSI */
    if (ret < 0) return ret;

    ret = gpio_pin_configure(gpio0, QSPI_IO1_PIN, GPIO_INPUT);           /* MISO, no pull */
    if (ret < 0) return ret;

    ret = gpio_pin_configure(gpio0, QSPI_IO2_PIN, GPIO_OUTPUT_ACTIVE);   /* WP# high */
    if (ret < 0) return ret;

    ret = gpio_pin_configure(gpio0, QSPI_IO3_PIN, GPIO_OUTPUT_ACTIVE);   /* HOLD# high */
    if (ret < 0) return ret;

    k_busy_wait(20);
    return 0;
}

static int qspi_park_low_power(void)
{
    int ret;

    ret = gpio_pin_configure(gpio0, QSPI_CS_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) return ret;

    ret = gpio_pin_configure(gpio0, QSPI_SCK_PIN, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) return ret;

    ret = gpio_pin_configure(gpio0, QSPI_IO0_PIN, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) return ret;

    ret = gpio_pin_configure(gpio0, QSPI_IO1_PIN, GPIO_INPUT);
    if (ret < 0) return ret;

    ret = gpio_pin_configure(gpio0, QSPI_IO2_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) return ret;

    ret = gpio_pin_configure(gpio0, QSPI_IO3_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) return ret;

    return ret;
}

static void flash_send_byte(uint8_t value)
{
    for (int i = 7; i >= 0; i--) {
        gpio_pin_set(gpio0, QSPI_IO0_PIN, (value >> i) & 1U);
        gpio_pin_set(gpio0, QSPI_SCK_PIN, 1);
        k_busy_wait(1);
        gpio_pin_set(gpio0, QSPI_SCK_PIN, 0);
    }
}

static uint8_t flash_read_byte(void)
{
    uint8_t value = 0U;

    for (int i = 7; i >= 0; i--) {
        gpio_pin_set(gpio0, QSPI_SCK_PIN, 1);
        k_busy_wait(1);
        if (gpio_pin_get(gpio0, QSPI_IO1_PIN) > 0) {
            value |= BIT(i);
        }
        gpio_pin_set(gpio0, QSPI_SCK_PIN, 0);
        k_busy_wait(1);
    }

    return value;
}

static void flash_send_cmd(uint8_t cmd)
{
    gpio_pin_set(gpio0, QSPI_CS_PIN, 0);
    flash_send_byte(cmd);
    gpio_pin_set(gpio0, QSPI_CS_PIN, 1);
}

static void flash_wake(void)
{
    flash_send_cmd(0xABU);                    /* release deep power down */
    k_busy_wait(50);
}

static void flash_sleep(void)
{
    flash_send_cmd(0xB9U);                    /* deep power down again after each operation group */
    k_busy_wait(20);
}

static uint8_t flash_read_status(void)
{
    uint8_t sr;

    gpio_pin_set(gpio0, QSPI_CS_PIN, 0);
    flash_send_byte(0x05U);
    sr = flash_read_byte();
    gpio_pin_set(gpio0, QSPI_CS_PIN, 1);

    return sr;
}

static int flash_wait_mask_set(uint8_t mask, uint32_t timeout_ms)
{
    uint32_t start = k_uptime_get_32();

    while ((uint32_t)(k_uptime_get_32() - start) < timeout_ms) {
        if ((flash_read_status() & mask) == mask) {
            return 0;
        }
        k_busy_wait(50);
    }

    return -ETIMEDOUT;
}

static int flash_wait_mask_clear(uint8_t mask, uint32_t timeout_ms)
{
    uint32_t start = k_uptime_get_32();

    while ((uint32_t)(k_uptime_get_32() - start) < timeout_ms) {
        if ((flash_read_status() & mask) == 0U) {
            return 0;
        }
        k_busy_wait(50);
    }

    return -ETIMEDOUT;
}

static int flash_read_bytes_awake(uint32_t addr, uint8_t *buf, uint16_t len)
{
    gpio_pin_set(gpio0, QSPI_CS_PIN, 0);

    flash_send_byte(0x03U);
    flash_send_byte((uint8_t)(addr >> 16));
    flash_send_byte((uint8_t)(addr >> 8));
    flash_send_byte((uint8_t)addr);

    for (uint16_t i = 0; i < len; i++) {
        buf[i] = flash_read_byte();
    }

    gpio_pin_set(gpio0, QSPI_CS_PIN, 1);
    return 0;
}

static int flash_program_bytes(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    int ret;
    uint8_t verify[FLASH_PAGE_SIZE];

    if (len == 0U) {
        return 0;
    }

    if (((addr % FLASH_PAGE_SIZE) + len) > FLASH_PAGE_SIZE) {
        return -EINVAL;                       /* never cross page boundary */
    }

    ret = qspi_claim_pins();
    if (ret < 0) {
        return ret;
    }

    flash_wake();

    ret = flash_wait_mask_clear(FLASH_SR_WIP, FLASH_TIMEOUT_PROG_MS);
    if (ret < 0) {
        flash_sleep();
        qspi_park_low_power();
        return ret;
    }

    flash_send_cmd(0x06U);                    /* WREN */

    ret = flash_wait_mask_set(FLASH_SR_WEL, FLASH_TIMEOUT_WEL_MS);
    if (ret < 0) {
        flash_sleep();
        qspi_park_low_power();
        return ret;
    }

    gpio_pin_set(gpio0, QSPI_CS_PIN, 0);
    flash_send_byte(0x02U);                   /* page program */
    flash_send_byte((uint8_t)(addr >> 16));
    flash_send_byte((uint8_t)(addr >> 8));
    flash_send_byte((uint8_t)addr);

    for (uint16_t i = 0; i < len; i++) {
        flash_send_byte(buf[i]);
    }

    gpio_pin_set(gpio0, QSPI_CS_PIN, 1);

    ret = flash_wait_mask_clear(FLASH_SR_WIP, FLASH_TIMEOUT_PROG_MS);
    if (ret < 0) {
        flash_sleep();
        qspi_park_low_power();
        return ret;
    }

    flash_read_bytes_awake(addr, verify, len); /* verify write immediately; still worth it in this stage */
    flash_sleep();
    qspi_park_low_power();

    if (memcmp(verify, buf, len) != 0) {
        return -EIO;
    }

    return 0;
}

static int flash_erase_sector(uint32_t addr)
{
    int ret;

    ret = qspi_claim_pins();
    if (ret < 0) {
        return ret;
    }

    flash_wake();

    ret = flash_wait_mask_clear(FLASH_SR_WIP, FLASH_TIMEOUT_ERASE_MS);
    if (ret < 0) {
        flash_sleep();
        qspi_park_low_power();
        return ret;
    }

    flash_send_cmd(0x06U);

    ret = flash_wait_mask_set(FLASH_SR_WEL, FLASH_TIMEOUT_WEL_MS);
    if (ret < 0) {
        flash_sleep();
        qspi_park_low_power();
        return ret;
    }

    gpio_pin_set(gpio0, QSPI_CS_PIN, 0);
    flash_send_byte(0x20U);                   /* 4 KiB sector erase */
    flash_send_byte((uint8_t)(addr >> 16));
    flash_send_byte((uint8_t)(addr >> 8));
    flash_send_byte((uint8_t)addr);
    gpio_pin_set(gpio0, QSPI_CS_PIN, 1);

    ret = flash_wait_mask_clear(FLASH_SR_WIP, FLASH_TIMEOUT_ERASE_MS);
    flash_sleep();
    qspi_park_low_power();
    return ret;
}

static int flash_chip_erase(void)
{
    int ret;
    uint8_t probe[16];

    ret = qspi_claim_pins();
    if (ret < 0) {
        return ret;
    }

    flash_wake();
    flash_send_cmd(0x06U);

    ret = flash_wait_mask_set(FLASH_SR_WEL, FLASH_TIMEOUT_WEL_MS);
    if (ret < 0) {
        flash_sleep();
        qspi_park_low_power();
        return ret;
    }

    gpio_pin_set(gpio0, QSPI_CS_PIN, 0);
    flash_send_byte(0xC7U);                   /* whole-chip erase */
    gpio_pin_set(gpio0, QSPI_CS_PIN, 1);

    ret = flash_wait_mask_clear(FLASH_SR_WIP, FLASH_TIMEOUT_ERASE_MS);
    if (ret < 0) {
        flash_sleep();
        qspi_park_low_power();
        return ret;
    }

    flash_read_bytes_awake(0U, probe, sizeof(probe));
    flash_sleep();
    qspi_park_low_power();

    for (size_t i = 0; i < sizeof(probe); i++) {
        if (probe[i] != 0xFFU) {
            return -EIO;
        }
    }

    return 0;
}

static int meta_load_latest(void)
{
    int ret;
    meta_record_t meta;
    bool found = false;
    uint32_t best_generation = 0U;
    uint16_t best_slot = 0U;

    ret = qspi_claim_pins();
    if (ret < 0) {
        return ret;
    }

    flash_wake();

    for (uint16_t slot = 0U; slot < META_SLOTS; slot++) {
        uint32_t addr = META_SECTOR_BASE + ((uint32_t)slot * sizeof(meta_record_t));
        flash_read_bytes_awake(addr, (uint8_t *)&meta, sizeof(meta));

        if (meta_is_erased(&meta)) {
            continue;
        }

        if (!meta_is_valid(&meta)) {
            continue;
        }

        if ((!found) || (meta.generation >= best_generation)) {
            best_generation = meta.generation;
            best_slot = slot;
            found = true;
        }
    }

    flash_sleep();
    qspi_park_low_power();

    if (!found) {
        meta_generation = 0U;
        meta_next_slot = 0U;
        return -ENOENT;
    }

    ret = qspi_claim_pins();
    if (ret < 0) {
        return ret;
    }

    flash_wake();
    flash_read_bytes_awake(META_SECTOR_BASE + ((uint32_t)best_slot * sizeof(meta_record_t)),
                           (uint8_t *)&meta, sizeof(meta));
    flash_sleep();
    qspi_park_low_power();

    if (!meta_is_valid(&meta)) {
        return -EIO;
    }

    flash_write_ptr = meta.write_ptr;
    next_seq = meta.next_seq;
    rtc_ctx.valid = (meta.rtc_valid != 0U);
    rtc_ctx.unix_s = meta.anchor_utc_s;
    rtc_ctx.uptime_ms = meta.anchor_uptime_ms;
    rtc_ctx.last_anchor_ms = meta.anchor_uptime_ms;
    rtc_ctx.source = meta.rtc_source;
    meta_generation = meta.generation;
    meta_next_slot = best_slot + 1U;
    resume_used_fallback_scan = false;

    if (flash_write_ptr >= DATA_FLASH_BYTES) {
        log_storage_full = true;
    }

    return 0;
}

static int meta_store_checkpoint(void)
{
    meta_record_t meta;
    int ret;
    uint32_t addr;

    if (meta_next_slot >= META_SLOTS) {
        ret = flash_erase_sector(META_SECTOR_BASE);     /* simple circular policy for now */
        if (ret < 0) {
            return ret;
        }
        meta_next_slot = 0U;
    }

    memset(&meta, 0, sizeof(meta));
    meta.magic = META_MAGIC;
    meta.version = META_VERSION;
    meta.generation = ++meta_generation;
    meta.write_ptr = flash_write_ptr;
    meta.next_seq = next_seq;
    meta.anchor_utc_s = rtc_ctx.valid ? rtc_ctx.unix_s : 0U;
    meta.anchor_uptime_ms = rtc_ctx.valid ? rtc_ctx.uptime_ms : 0U;
    meta.rtc_source = rtc_ctx.source;
    meta.rtc_valid = rtc_ctx.valid ? 1U : 0U;
    meta.reserved_b = 0U;
    meta_finalize(&meta);

    addr = META_SECTOR_BASE + ((uint32_t)meta_next_slot * sizeof(meta_record_t));
    ret = flash_program_bytes(addr, (const uint8_t *)&meta, sizeof(meta));
    if (ret < 0) {
        return ret;
    }

    meta_next_slot++;
    return 0;
}

static int scan_log_and_find_end_fallback(void)
{
    int ret;
    log_record_t rec;

    flash_write_ptr = 0U;
    next_seq = 0U;
    log_storage_full = false;
    rtc_ctx.valid = false;
    rtc_ctx.unix_s = 0U;
    rtc_ctx.uptime_ms = 0U;
    rtc_ctx.last_anchor_ms = 0U;
    rtc_ctx.source = RTC_SOURCE_NONE;
    rtc_ctx.boot_anchor_pending = false;

    ret = qspi_claim_pins();
    if (ret < 0) {
        return ret;
    }

    flash_wake();

    while ((flash_write_ptr + sizeof(log_record_t)) <= DATA_FLASH_BYTES) {
        flash_read_bytes_awake(flash_write_ptr, (uint8_t *)&rec, sizeof(rec));

        if (record_is_erased(&rec)) {
            break;
        }
        if (!record_is_valid(&rec)) {
            break;
        }

        next_seq = rec.seq + 1U;

        if ((rec.type == LOG_TYPE_TIME_ANCHOR) && (rec.rtc_valid != 0U)) {
            rtc_ctx.valid = true;
            rtc_ctx.unix_s = rec.utc_s;
            rtc_ctx.uptime_ms = rec.uptime_ms;
            rtc_ctx.last_anchor_ms = rec.uptime_ms;
            rtc_ctx.source = rec.rtc_source;
        }

        flash_write_ptr += sizeof(log_record_t);
    }

    flash_sleep();
    qspi_park_low_power();

    meta_generation = 0U;
    meta_next_slot = 0U;
    resume_used_fallback_scan = true;

    if (flash_write_ptr >= DATA_FLASH_BYTES) {
        log_storage_full = true;
    }

    return 0;
}

static int flush_page_buffer(void)
{
    int ret;
    uint8_t page[FLASH_PAGE_SIZE];

    if (page_buffer_count == 0U) {
        return 0;
    }

    if (page_buffer_count != PAGE_BUFFER_RECORDS) {
        return -EAGAIN;                        /* this build flushes only full pages on purpose */
    }

    if ((flash_write_ptr + FLASH_PAGE_SIZE) > DATA_FLASH_BYTES) {
        log_storage_full = true;
        return -ENOSPC;
    }

    memcpy(page, page_buffer, sizeof(page));

    ret = flash_program_bytes(flash_write_ptr, page, sizeof(page));
    if (ret < 0) {
        return ret;
    }

    flash_write_ptr += FLASH_PAGE_SIZE;
    page_buffer_count = 0U;

    ret = meta_store_checkpoint();
    if (ret < 0) {
        return ret;                            /* data page is already there; next boot can still recover by fallback scan */
    }

    return 0;
}

static int append_record_buffered(const log_record_t *rec)
{
    int ret;

    if (log_storage_full) {
        return -ENOSPC;
    }

    page_buffer[page_buffer_count] = *rec;
    page_buffer_count++;

    if (page_buffer_count >= PAGE_BUFFER_RECORDS) {
        ret = flush_page_buffer();
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int append_time_anchor(uint16_t flags, bool force_flush)
{
    log_record_t rec;
    int ret;

    ARG_UNUSED(force_flush);                  /* kept in signature so future RTC work can choose a different policy */

    if (!rtc_ctx.valid) {
        return 0;
    }

    rtc_ctx.uptime_ms = k_uptime_get_32();
    rtc_ctx.unix_s = rtc_estimate_unix_from_uptime(rtc_ctx.uptime_ms);

    record_make_time_anchor(&rec, next_seq, flags);
    next_seq = rec.seq + 1U;

    ret = append_record_buffered(&rec);       /* anchor records follow the same page batching policy for now */
    if (ret == 0U) {
        rtc_ctx.last_anchor_ms = rec.uptime_ms;
        rtc_ctx.boot_anchor_pending = false;
    }

    return ret;
}

static void usb_write_str(const struct device *cdc, const char *s)
{
    while (*s != '\0') {
        uart_poll_out(cdc, *s++);
    }
}

static void usb_write_u32(const struct device *cdc, uint32_t v)
{
    char buf[11];
    int i = 0;

    if (v == 0U) {
        uart_poll_out(cdc, '0');
        return;
    }

    while (v > 0U) {
        buf[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }

    for (int j = i - 1; j >= 0; j--) {
        uart_poll_out(cdc, buf[j]);
    }
}

static void usb_write_hex_u32(const struct device *cdc, uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    usb_write_str(cdc, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_poll_out(cdc, hex[(v >> shift) & 0x0FU]);
    }
}

static void usb_write_s32(const struct device *cdc, int32_t v)
{
    if (v < 0) {
        uart_poll_out(cdc, '-');
        usb_write_u32(cdc, (uint32_t)(-v));
    } else {
        usb_write_u32(cdc, (uint32_t)v);
    }
}

static void usb_write_csv_record(const struct device *cdc,
                                 const log_record_t *rec,
                                 uint32_t *anchor_unix_s,
                                 uint32_t *anchor_uptime_ms)
{
    uint32_t est_unix = 0U;

    if (rec->type == LOG_TYPE_TIME_ANCHOR) {
        *anchor_unix_s = rec->utc_s;
        *anchor_uptime_ms = rec->uptime_ms;
        est_unix = rec->utc_s;
    } else if (*anchor_unix_s != 0U) {
        est_unix = *anchor_unix_s + ((rec->uptime_ms - *anchor_uptime_ms) / 1000U);
    }

    usb_write_u32(cdc, rec->seq);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, rec->type);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, rec->uptime_ms);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, est_unix);
    usb_write_str(cdc, ",");
    usb_write_s32(cdc, rec->sensor1_mv);
    usb_write_str(cdc, ",");
    usb_write_s32(cdc, rec->sensor2_mv);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, rec->rtc_source);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, rec->rtc_valid);
    usb_write_str(cdc, "\r\n");
}

static void run_maintenance(void)
{
    const struct device *cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
    log_record_t rec;
    uint32_t read_ptr = 0U;
    uint32_t anchor_unix_s = 0U;
    uint32_t anchor_uptime_ms = 0U;
    uint32_t count = 0U;
    uint32_t dtr = 0U;
    int ret;

    if (!device_is_ready(cdc)) {
        while (1) {
            k_sleep(K_SECONDS(1));
        }
    }

    if (usb_enable(NULL) != 0) {
        while (1) {
            k_sleep(K_SECONDS(1));
        }
    }

    for (int i = 0; i < 40; i++) {
        if ((uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr) == 0) && (dtr != 0U)) {
            break;
        }
        k_msleep(100);
    }

    usb_write_str(cdc, "seq,type,uptime_ms,utc_s,sensor1_mv,sensor2_mv,rtc_source,rtc_valid\r\n");

    ret = meta_load_latest();
    if (ret < 0) {
        (void)scan_log_and_find_end_fallback();
    }

    ret = qspi_claim_pins();
    if (ret < 0) {
        usb_write_str(cdc, "# qspi_claim_failed\r\n");
        while (1) {
            k_sleep(K_SECONDS(1));
        }
    }

    flash_wake();

    while ((read_ptr + sizeof(log_record_t)) <= flash_write_ptr) {
        flash_read_bytes_awake(read_ptr, (uint8_t *)&rec, sizeof(rec));

        if (record_is_erased(&rec)) {
            break;
        }
        if (!record_is_valid(&rec)) {
            break;
        }

        usb_write_csv_record(cdc, &rec, &anchor_unix_s, &anchor_uptime_ms);
        read_ptr += sizeof(log_record_t);
        count++;
    }

    flash_sleep();
    qspi_park_low_power();

    usb_write_str(cdc, "# records,");
    usb_write_u32(cdc, count);
    usb_write_str(cdc, "\r\n");

    usb_write_str(cdc, "# flash_write_ptr,");
    usb_write_hex_u32(cdc, flash_write_ptr);
    usb_write_str(cdc, "\r\n");

    usb_write_str(cdc, "# page_buffer_count,");
    usb_write_u32(cdc, page_buffer_count);
    usb_write_str(cdc, "\r\n");

    usb_write_str(cdc, "# resume_mode,");
    usb_write_str(cdc, resume_used_fallback_scan ? "fallback_scan\r\n" : "checkpoint\r\n");

    usb_write_str(cdc, "# meta_generation,");
    usb_write_u32(cdc, meta_generation);
    usb_write_str(cdc, "\r\n");

    while (1) {
        k_sleep(K_SECONDS(2));
    }
}

int main(void)
{
    int ret;

    if (!device_is_ready(gpio0) || !device_is_ready(adc)) {
        return 0;
    }

    ret = gpio_pin_configure(gpio0, WAKE_PIN_NUM, GPIO_INPUT | GPIO_PULL_DOWN);  /* WAKE high at boot enters maintenance mode */
    if (ret < 0) {
        return 0;
    }

    k_msleep(50);

    if (gpio_pin_get(gpio0, WAKE_PIN_NUM) > 0) {
        run_maintenance();
    }

    NRF_USBD->ENABLE = 0;                     /* keep USB electrically out of normal runtime path */
    NRF_POWER->TASKS_LOWPWR = 1;
    nrf_power_dcdcen_set(NRF_POWER, true);

    ret = qspi_park_low_power();
    if (ret < 0) {
        return 0;
    }

    ret = gpio_pin_configure(gpio0, SENSOR_PWR_NUM, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return 0;
    }

    ret = gpio_pin_configure(gpio0, DEBUG_PULSE_NUM, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return 0;
    }

    ret = gpio_pin_configure(gpio0, UART_RX_PIN, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0) {
        return 0;
    }

    ret = adc_channel_setup(adc, &ch0);
    if (ret < 0) {
        return 0;
    }

    ret = adc_channel_setup(adc, &ch1);
    if (ret < 0) {
        return 0;
    }

    k_sem_init(&wake_sem, 0, 1);
    gpio_init_callback(&wake_cb, wake_isr, BIT(WAKE_PIN_NUM));

    ret = gpio_add_callback(gpio0, &wake_cb);
    if (ret < 0) {
        return 0;
    }

    ret = gpio_pin_interrupt_configure(gpio0, WAKE_PIN_NUM, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        return 0;
    }

    {
        uint32_t unix_s = 0U;
        uint8_t source = RTC_SOURCE_NONE;
        if (rtc_try_seed_on_boot(&unix_s, &source)) {
            rtc_seed_known_time(unix_s, source);
        }
    }

    page_buffer_count = 0U;
    flash_write_ptr = 0U;
    next_seq = 0U;
    log_storage_full = false;
    meta_generation = 0U;
    meta_next_slot = 0U;
    resume_used_fallback_scan = false;

    /*
     * One-time erase for format/layout reset.
     * Use only once when switching from older firmware to this version.
     * Procedure:
     * 1) uncomment next line
     * 2) flash firmware
     * 3) let normal mode boot once and finish erase
     * 4) comment it back out
     * 5) rebuild + flash again for real tests
     */
    /* ret = flash_chip_erase(); if (ret < 0) { return 0; } */

    ret = meta_load_latest();
    if (ret < 0) {
        ret = scan_log_and_find_end_fallback();  /* rare fallback only if metadata is absent or invalid */
        if (ret < 0) {
            return 0;
        }
        (void)meta_store_checkpoint();           /* seed first checkpoint so next boot can resume fast */
    }

    if (rtc_ctx.boot_anchor_pending) {
        (void)append_time_anchor(LOG_FLAG_TIME_BOOT, false);
    }

    while (1) {
        int16_t raw0 = 0;
        int16_t raw1 = 0;
        int32_t mv0;
        int32_t mv1;
        uint32_t now_ms;
        log_record_t rec;

        ret = k_sem_take(&wake_sem, K_FOREVER);
        if (ret < 0) {
            continue;
        }

        ret = gpio_pin_set(gpio0, SENSOR_PWR_NUM, 1);
        if (ret < 0) {
            continue;
        }

        k_sleep(K_MSEC(SENSOR_SETTLE_MS));

        ret = read_adc(&raw0, &raw1);

        gpio_pin_set(gpio0, SENSOR_PWR_NUM, 0);
        if (ret < 0) {
            continue;
        }

        mv0 = adc_to_mv_fast(raw0);
        mv1 = adc_to_mv_fast(raw1);
        now_ms = k_uptime_get_32();

        emit_pulses(map_pulses(mv0));
        k_busy_wait(GAP_US);
        emit_pulses(map_pulses(mv1));

        {
            uint8_t before_count = page_buffer_count;

            record_make_data(&rec, next_seq, now_ms, (int16_t)mv0, (int16_t)mv1);
            next_seq = rec.seq + 1U;
            ret = append_record_buffered(&rec);

            if (ret == 0) {
                if ((before_count == (PAGE_BUFFER_RECORDS - 1U)) && (page_buffer_count == 0U)) {
                    emit_pulses(3);            /* one page flushed + checkpoint saved */
                }
            } else if (ret == -ENOSPC) {
                log_storage_full = true;
                emit_pulses(10);               /* storage full or page cannot fit */
            } else if (ret == -EAGAIN) {
                emit_pulses(10);               /* should not happen here; full page flushing is internal */
            } else {
                emit_pulses(10);               /* flush or checkpoint failed */
            }
        }

        if (time_anchor_due(now_ms)) {
            rtc_ctx.uptime_ms = now_ms;
            rtc_ctx.unix_s = rtc_estimate_unix_from_uptime(now_ms);
            (void)append_time_anchor(LOG_FLAG_TIME_PERIODIC, false);
        }
    }
}