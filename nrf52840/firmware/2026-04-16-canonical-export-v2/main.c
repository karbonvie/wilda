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

/*
main.c - pwrlow Stage 1: canonical tag-event export on top of the proven sparse-event logger

What changed in this Stage 1 build:
- keeps the already-proven sparse-event storage model: immediate event writes + metadata checkpoints
- keeps the already-proven low-level QSPI transport and maintenance dump path
- freezes a canonical tag-event contract for later gateway / STM32WL / TTN work
- does NOT yet add BLE, LoRaWAN, RTC hardware, or battery ADC measurement

Why this version exists:
- the logger is already working well enough; the next system-level need is a stable event contract
- downstream pieces (gateway, TTN decoder, backend row mapping) should depend on one clean export format
- this version changes semantics and export only, not the proven flash transport

Canonical maintenance export columns:
- v, seq, boot_id, tag_id, site_id, mono_ms, utc_s, utc_valid, event_type, sensor1_mv, sensor2_mv, batt_mv, fw_id

Field mapping used in this firmware:
- v            -> TAG_EVENT_EXPORT_VERSION (contract version, external-facing)
- seq          -> per-record sequence number
- boot_id      -> stored in rec.reserved0, increments once per boot and is persisted through metadata
- tag_id       -> compile-time constant for now
- site_id      -> compile-time constant for now
- mono_ms      -> uptime_ms recorded at event time
- utc_s        -> 0 until RTC or gateway time is added later
- utc_valid    -> 0 until RTC or gateway time is added later
- event_type   -> rec.flags for data records, special exported code for time-anchor records
- batt_mv      -> 0 placeholder until real battery measurement path exists
- fw_id        -> compile-time constant for easy traceability

Expected behavior:
- normal mode still wakes, samples, pulses, writes events, and checkpoints periodically
- maintenance mode dumps canonical CSV rows plus a few diagnostic footer lines
- boot_id_current in footer shows the ID that will be written into new records in the current boot

Erase procedure:
- do one full flash erase once when switching from older layouts / semantics to this one
- uncomment flash_chip_erase(), flash firmware, boot once in normal mode, comment it back out, rebuild, flash again
- do NOT leave erase enabled during normal use

What to troubleshoot first if results look wrong:
- if dump works but fields look shifted or nonsensical, most likely the record/meta layout changed without a one-time erase
- if dump shows rows but boot_id never changes across reboots, metadata write/load path is the first thing to inspect
- if storage/dump regresses at all, stop touching export semantics and go back to the last known-good logger build
*/

#define SENSOR_SETTLE_MS              2U
#define GAP_US                        500U
#define ADC_RESOLUTION                12U
#define ADC_ACQ_US                    10U
#define MAX_PULSES                    15

#define DEBUG_PULSE_MARKERS           0U   /* 0 = best power, 1 = 3/10 pulse write markers */
#define CHECKPOINT_EVERY_EVENTS       16U  /* lower power than every-event checkpoint, tiny boot recovery scan */

#define TAG_EVENT_EXPORT_VERSION      1U   /* contract version for gateway/backend integration */
#define TAG_ID                        1U   /* compile-time placeholder until real registry exists */
#define SITE_ID                       1U   /* compile-time placeholder until deployment mapping exists */
#define FW_ID                         1U   /* simple numeric firmware ID, enough for traceability today */

#define EVENT_TYPE_WAKE_SAMPLE        1U   /* ordinary accepted wake/sample event */
#define EVENT_TYPE_TIME_BOOT          100U /* exported maintenance label for boot anchor records */
#define EVENT_TYPE_TIME_PERIODIC      101U /* exported maintenance label for periodic anchor records */

#define FLASH_SIZE_BYTES              (1024U * 1024U)
#define FLASH_PAGE_SIZE               256U
#define FLASH_SECTOR_SIZE             4096U
#define FLASH_TIMEOUT_WEL_MS          4U
#define FLASH_TIMEOUT_PROG_MS         40U
#define FLASH_TIMEOUT_ERASE_MS        10000U
#define FLASH_SR_WIP                  BIT(0)
#define FLASH_SR_WEL                  BIT(1)

#define META_SECTOR_BASE              (FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define DATA_FLASH_BYTES              META_SECTOR_BASE

#define LOG_MAGIC                     0xA55AU
#define LOG_TAIL                      0x5AA5U
#define LOG_VERSION                   1U
#define LOG_TYPE_DATA                 1U
#define LOG_TYPE_TIME_ANCHOR          2U

#define META_MAGIC                    0xC33CU
#define META_TAIL                     0x3CC3U
#define META_VERSION                  1U

#define RTC_SOURCE_NONE               0U
#define RTC_SOURCE_EXT_RTC            1U
#define RTC_SOURCE_LORAWAN            2U
#define RTC_SOURCE_USB_SERVICE        3U

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
    uint8_t  rtc_source;
    uint32_t generation;
    uint32_t write_ptr;
    uint32_t next_seq;
    uint32_t boot_id;        /* persisted once per boot so exported rows can distinguish sessions */
    uint32_t anchor_utc_s;
    uint32_t anchor_uptime_ms;
    uint16_t crc16;
    uint16_t tail;
} meta_record_t;

BUILD_ASSERT(sizeof(log_record_t) == 32, "log_record_t must stay 32 bytes");
BUILD_ASSERT(sizeof(meta_record_t) == 32, "meta_record_t must stay 32 bytes");
BUILD_ASSERT((FLASH_PAGE_SIZE % sizeof(log_record_t)) == 0, "record size must divide page size");
BUILD_ASSERT((FLASH_SECTOR_SIZE % sizeof(meta_record_t)) == 0, "meta record size must divide metadata sector");
BUILD_ASSERT((CHECKPOINT_EVERY_EVENTS >= 1U), "checkpoint interval must be >= 1");

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
static uint32_t flash_write_ptr;
static uint32_t next_seq;
static bool log_storage_full;
static uint32_t meta_generation;
static uint16_t meta_next_slot;
static bool resume_used_fallback_scan;
static uint32_t events_since_checkpoint;
static uint32_t boot_id;                        /* increments once per boot, persisted via metadata */

static inline int32_t adc_to_mv_fast(int16_t raw)
{
    if (raw <= 0) {
        return 0;
    }
    return ((int32_t)raw * 3600) / 4096;
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

static inline void emit_debug_marker(int ok)
{
    if (DEBUG_PULSE_MARKERS == 0U) {
        return;
    }

    for (int i = 0; i < ok; i++) {
        gpio_pin_set(gpio0, DEBUG_PULSE_NUM, 1);
        k_busy_wait(200);
        gpio_pin_set(gpio0, DEBUG_PULSE_NUM, 0);
        k_busy_wait(200);
    }
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
    rtc_ctx.last_anchor_ms = 0U;
    rtc_ctx.source = source;
    rtc_ctx.boot_anchor_pending = true;
}

static bool rtc_try_seed_on_boot(uint32_t *unix_s, uint8_t *source)
{
    ARG_UNUSED(unix_s);
    ARG_UNUSED(source);
    return false;                             /* external RTC / LoRaWAN time can hook here later */
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
    rec->utc_s = 0U;
    rec->sensor1_mv = s1_mv;
    rec->sensor2_mv = s2_mv;
    rec->rtc_source = rtc_ctx.source;
    rec->rtc_valid = rtc_ctx.valid ? 1U : 0U;
    rec->flags = EVENT_TYPE_WAKE_SAMPLE;      /* data-record semantic type for gateway/backend */
    rec->reserved0 = boot_id;                 /* boot/session discriminator, survives export */

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
    rec->flags = flags;                       /* anchor subtype: boot or periodic */
    rec->reserved0 = boot_id;                /* same session discriminator as data records */

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
    if ((meta->write_ptr % sizeof(log_record_t)) != 0U) {
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
    ret = adc_read(adc, &seq);
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
    k_sem_give(&wake_sem);
}

static int qspi_claim_pins(void)
{
    int ret;

    if (NRF_QSPI->ENABLE != 0U) {
        NRF_QSPI->TASKS_DEACTIVATE = 1U;
        k_busy_wait(10);
        NRF_QSPI->ENABLE = 0U;
        k_busy_wait(10);
    }

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
    flash_send_cmd(0xABU);
    k_busy_wait(50);
}

static void flash_sleep(void)
{
    flash_send_cmd(0xB9U);
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
        return -EINVAL;
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

    flash_send_cmd(0x06U);

    ret = flash_wait_mask_set(FLASH_SR_WEL, FLASH_TIMEOUT_WEL_MS);
    if (ret < 0) {
        flash_sleep();
        qspi_park_low_power();
        return ret;
    }

    gpio_pin_set(gpio0, QSPI_CS_PIN, 0);
    flash_send_byte(0x02U);
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

    flash_read_bytes_awake(addr, verify, len);
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
    flash_send_byte(0x20U);
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
    flash_send_byte(0xC7U);
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
        events_since_checkpoint = 0U;
        boot_id = 1U;                        /* first boot after erase / no metadata */
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
    rtc_ctx.valid = ((meta.rtc_source != RTC_SOURCE_NONE) && (meta.anchor_utc_s != 0U));
    rtc_ctx.unix_s = meta.anchor_utc_s;
    rtc_ctx.uptime_ms = meta.anchor_uptime_ms;
    rtc_ctx.last_anchor_ms = meta.anchor_uptime_ms;
    rtc_ctx.source = meta.rtc_source;
    meta_generation = meta.generation;
    meta_next_slot = best_slot + 1U;
    events_since_checkpoint = 0U;
    boot_id = meta.boot_id + 1U;            /* new boot gets new ID, previous records keep old one */
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
        ret = flash_erase_sector(META_SECTOR_BASE);
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
    meta.boot_id = boot_id;
    meta.anchor_utc_s = rtc_ctx.valid ? rtc_ctx.unix_s : 0U;
    meta.anchor_uptime_ms = rtc_ctx.valid ? rtc_ctx.uptime_ms : 0U;
    meta.rtc_source = rtc_ctx.valid ? rtc_ctx.source : RTC_SOURCE_NONE;
    meta_finalize(&meta);

    addr = META_SECTOR_BASE + ((uint32_t)meta_next_slot * sizeof(meta_record_t));
    ret = flash_program_bytes(addr, (const uint8_t *)&meta, sizeof(meta));
    if (ret < 0) {
        return ret;
    }

    meta_next_slot++;
    events_since_checkpoint = 0U;
    return 0;
}

static int forward_scan_from_pointer(uint32_t start_ptr)
{
    int ret;
    log_record_t rec;
    uint32_t ptr = start_ptr;
    uint32_t found_since_checkpoint = 0U;

    if (ptr >= DATA_FLASH_BYTES) {
        log_storage_full = true;
        return -ENOSPC;
    }

    ret = qspi_claim_pins();
    if (ret < 0) {
        return ret;
    }

    flash_wake();

    while ((ptr + sizeof(log_record_t)) <= DATA_FLASH_BYTES) {
        flash_read_bytes_awake(ptr, (uint8_t *)&rec, sizeof(rec));

        if (record_is_erased(&rec)) {
            break;
        }
        if (!record_is_valid(&rec)) {
            break;
        }

        next_seq = rec.seq + 1U;
        ptr += sizeof(log_record_t);
        found_since_checkpoint++;

        if ((rec.type == LOG_TYPE_TIME_ANCHOR) && (rec.rtc_valid != 0U)) {
            rtc_ctx.valid = true;
            rtc_ctx.unix_s = rec.utc_s;
            rtc_ctx.uptime_ms = rec.uptime_ms;
            rtc_ctx.last_anchor_ms = rec.uptime_ms;
            rtc_ctx.source = rec.rtc_source;
        }

        if (found_since_checkpoint >= CHECKPOINT_EVERY_EVENTS) {
            break;                              /* bounded recovery scan by design */
        }
    }

    flash_sleep();
    qspi_park_low_power();

    flash_write_ptr = ptr;
    events_since_checkpoint = found_since_checkpoint;

    if (flash_write_ptr >= DATA_FLASH_BYTES) {
        log_storage_full = true;
    }

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
    meta_generation = 0U;
    meta_next_slot = 0U;
    events_since_checkpoint = 0U;
    resume_used_fallback_scan = true;

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

    if (flash_write_ptr >= DATA_FLASH_BYTES) {
        log_storage_full = true;
    }

    return 0;
}

static int append_record_direct(const log_record_t *rec)
{
    int ret;

    if (log_storage_full) {
        return -ENOSPC;
    }
    if ((flash_write_ptr + sizeof(log_record_t)) > DATA_FLASH_BYTES) {
        log_storage_full = true;
        return -ENOSPC;
    }

    ret = flash_program_bytes(flash_write_ptr, (const uint8_t *)rec, sizeof(*rec));
    if (ret < 0) {
        return ret;
    }

    flash_write_ptr += sizeof(*rec);
    next_seq = rec->seq + 1U;
    events_since_checkpoint++;

    if (events_since_checkpoint >= CHECKPOINT_EVERY_EVENTS) {
        ret = meta_store_checkpoint();        /* periodic checkpoint only, lower energy than every-event */
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

static int append_time_anchor(uint16_t flags)
{
    log_record_t rec;
    int ret;

    if (!rtc_ctx.valid) {
        return 0;
    }

    rtc_ctx.uptime_ms = k_uptime_get_32();
    rtc_ctx.unix_s = rtc_estimate_unix_from_uptime(rtc_ctx.uptime_ms);

    record_make_time_anchor(&rec, next_seq, flags);
    ret = append_record_direct(&rec);
    if (ret == 0) {
        rtc_ctx.last_anchor_ms = rec.uptime_ms;
        rtc_ctx.boot_anchor_pending = false;
        ret = meta_store_checkpoint();        /* anchors deserve immediate checkpoint so UTC context survives reboot */
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

static uint32_t export_event_type(const log_record_t *rec)
{
    if (rec->type == LOG_TYPE_DATA) {
        return (uint32_t)rec->flags;
    }

    if (rec->flags == LOG_FLAG_TIME_BOOT) {
        return EVENT_TYPE_TIME_BOOT;
    }

    return EVENT_TYPE_TIME_PERIODIC;
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

    usb_write_u32(cdc, TAG_EVENT_EXPORT_VERSION);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, rec->seq);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, rec->reserved0);       /* boot_id stored in reserved0 */
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, TAG_ID);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, SITE_ID);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, rec->uptime_ms);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, est_unix);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, rec->rtc_valid);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, export_event_type(rec));
    usb_write_str(cdc, ",");
    usb_write_s32(cdc, rec->sensor1_mv);
    usb_write_str(cdc, ",");
    usb_write_s32(cdc, rec->sensor2_mv);
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, 0U);                   /* batt_mv placeholder until battery ADC path is added */
    usb_write_str(cdc, ",");
    usb_write_u32(cdc, FW_ID);
    usb_write_str(cdc, "\r\n");          /* one canonical CSV row per record */
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

    usb_write_str(cdc, "v,seq,boot_id,tag_id,site_id,mono_ms,utc_s,utc_valid,event_type,sensor1_mv,sensor2_mv,batt_mv,fw_id\r\n");

    ret = meta_load_latest();
    if (ret < 0) {
        (void)scan_log_and_find_end_fallback();
    } else {
        (void)forward_scan_from_pointer(flash_write_ptr); /* recover only small tail beyond latest checkpoint */
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

    usb_write_str(cdc, "# events_since_checkpoint,");
    usb_write_u32(cdc, events_since_checkpoint);
    usb_write_str(cdc, "\r\n");

    usb_write_str(cdc, "# resume_mode,");
    usb_write_str(cdc, resume_used_fallback_scan ? "fallback_scan\r\n" : "checkpoint\r\n");

    usb_write_str(cdc, "# meta_generation,");
    usb_write_u32(cdc, meta_generation);
    usb_write_str(cdc, "\r\n");

    usb_write_str(cdc, "# meta_next_slot,");
    usb_write_u32(cdc, meta_next_slot);
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

    ret = gpio_pin_configure(gpio0, WAKE_PIN_NUM, GPIO_INPUT | GPIO_PULL_DOWN);
    if (ret < 0) {
        return 0;
    }

    k_msleep(50);

    if (gpio_pin_get(gpio0, WAKE_PIN_NUM) > 0) {
        run_maintenance();
    }

    NRF_USBD->ENABLE = 0;                     /* normal runtime stays USB-free for best power */
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

    rtc_ctx.valid = false;
    rtc_ctx.unix_s = 0U;
    rtc_ctx.uptime_ms = 0U;
    rtc_ctx.last_anchor_ms = 0U;
    rtc_ctx.source = RTC_SOURCE_NONE;
    rtc_ctx.boot_anchor_pending = false;

    flash_write_ptr = 0U;
    next_seq = 0U;
    log_storage_full = false;
    meta_generation = 0U;
    meta_next_slot = 0U;
    resume_used_fallback_scan = false;
    events_since_checkpoint = 0U;

    {
        uint32_t unix_s = 0U;
        uint8_t source = RTC_SOURCE_NONE;
        if (rtc_try_seed_on_boot(&unix_s, &source)) {
            rtc_seed_known_time(unix_s, source);
        }
    }

    /* Uncomment once for format/layout reset, boot once, then comment back out and reflash. */
    /* ret = flash_chip_erase(); if (ret < 0) { return 0; } */

    ret = meta_load_latest();
    if (ret < 0) {
        ret = scan_log_and_find_end_fallback();
        if (ret < 0) {
            return 0;
        }
        (void)meta_store_checkpoint();        /* create first checkpoint after fallback scan */
    } else {
        ret = forward_scan_from_pointer(flash_write_ptr); /* bounded recovery after checkpoint */
        if (ret < 0) {
            return 0;
        }
    }

    (void)meta_store_checkpoint();            /* persist the new boot_id even before first real event */

    if (rtc_ctx.boot_anchor_pending) {
        (void)append_time_anchor(LOG_FLAG_TIME_BOOT);
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

        emit_pulses(map_pulses(mv0));         /* keep existing visible behavior for lab debugging */
        k_busy_wait(GAP_US);
        emit_pulses(map_pulses(mv1));

        record_make_data(&rec, next_seq, now_ms, (int16_t)mv0, (int16_t)mv1);
        ret = append_record_direct(&rec);

        if (ret == 0) {
            emit_debug_marker(3);
        } else if (ret == -ENOSPC) {
            log_storage_full = true;
            emit_debug_marker(10);
        } else {
            emit_debug_marker(10);
        }

        if (time_anchor_due(now_ms)) {
            rtc_ctx.uptime_ms = now_ms;
            rtc_ctx.unix_s = rtc_estimate_unix_from_uptime(now_ms);
            (void)append_time_anchor(LOG_FLAG_TIME_PERIODIC);
        }
    }
}
