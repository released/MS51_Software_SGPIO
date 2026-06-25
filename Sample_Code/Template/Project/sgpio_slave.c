/*_____ I N C L U D E S ____________________________________________________*/
#include <stdio.h>
#include <string.h>
#include "numicro_8051.h"
#include "sgpio_slave.h"

/*_____ D E C L A R A T I O N S ____________________________________________*/
extern unsigned long get_systick(void);

typedef struct
{
    uint32_t frame_count;
    uint32_t dropped_frames;
    uint16_t bit_count;
    uint16_t act_mask;
    uint16_t locate_mask;
    uint16_t fail_mask;
    uint8_t raw[SGPIO_SLAVE_RX_MAX_BYTES];
    uint8_t raw_len;
    uint8_t sload_raw;
    uint8_t sload_raw_valid;
    uint8_t low_sync_count;
    uint8_t overflow;
    uint8_t valid;
} SGPIO_FRAME_T;

/*_____ D E F I N I T I O N S ______________________________________________*/
#define SGPIO_LOW_SYNC_MIN_BITS             (5U)
#define SGPIO_SLOAD_RAW_BITS                (4U)
#define SGPIO_DATA_BITS_PER_SLOT            (3U)
#define SGPIO_DECODE_SLOT_COUNT             (SGPIO_SLAVE_MAX_SLOTS)
#define SGPIO_SLOT_BIT_MAX                  (SGPIO_SLAVE_MAX_SLOTS * SGPIO_DATA_BITS_PER_SLOT)
#define SGPIO_FRAME_GAP_TIMEOUT_MS          (5UL)
#define SGPIO_FRAME_ARM_TIMEOUT_MS          (20UL)
#define SGPIO_FRAME_LOG_FIRST_N             (1UL)
#define SGPIO_FRAME_LOG_MIN_INTERVAL_MS     (1000UL)
#define SGPIO_FRAME_STABLE_REQUIRED         (2U)
#define SGPIO_UNSTABLE_LOG_MIN_INTERVAL_MS  (3000UL)

static volatile uint8_t g_sgpio_capture_active = 0U;
static volatile uint8_t g_sgpio_marker_seen = 0U;
static volatile uint8_t g_sgpio_frame_ready = 0U;
static volatile uint8_t g_sgpio_overflow = 0U;
static volatile uint8_t g_sgpio_low_sync_count = 0U;
static volatile uint8_t g_sgpio_low_sync_at_marker = 0U;
static volatile uint8_t g_sgpio_sload_raw = 0U;
static volatile uint8_t g_sgpio_sload_raw_valid = 0U;
static volatile uint16_t g_sgpio_slot_bit_count = 0U;
static volatile uint8_t g_sgpio_raw[SGPIO_SLAVE_RX_MAX_BYTES];
static volatile uint32_t g_sgpio_frame_count = 0UL;
static volatile uint32_t g_sgpio_dropped_frames = 0UL;
static volatile unsigned long g_sgpio_capture_start_tick = 0UL;
static volatile unsigned long g_sgpio_last_clock_tick = 0UL;

static SGPIO_FRAME_T g_sgpio_last_frame;
static SGPIO_FRAME_T g_sgpio_filter_candidate;
static unsigned long g_sgpio_last_log_tick = 0UL;
static unsigned long g_sgpio_last_unstable_log_tick = 0UL;
static uint8_t g_sgpio_filter_candidate_valid = 0U;
static uint8_t g_sgpio_filter_repeat_count = 0U;

/*_____ F U N C T I O N S __________________________________________________*/
static void sgpio_clear_raw(void)
{
    uint8_t i;

    for (i = 0U; i < SGPIO_SLAVE_RX_MAX_BYTES; i++)
    {
        g_sgpio_raw[i] = 0U;
    }
}

static void sgpio_reset_capture_state(void)
{
    sgpio_clear_raw();
    g_sgpio_capture_active = 0U;
    g_sgpio_marker_seen = 0U;
    g_sgpio_overflow = 0U;
    g_sgpio_low_sync_count = 0U;
    g_sgpio_low_sync_at_marker = 0U;
    g_sgpio_sload_raw = 0U;
    g_sgpio_sload_raw_valid = 0U;
    g_sgpio_slot_bit_count = 0U;
    g_sgpio_capture_start_tick = 0UL;
    g_sgpio_last_clock_tick = 0UL;
}

static void sgpio_store_sdata_bit(uint8_t bit_value)
{
    uint16_t bit_index;
    uint8_t byte_index;
    uint8_t bit_mask;

    bit_index = g_sgpio_slot_bit_count;
    if (bit_index >= SGPIO_SLOT_BIT_MAX)
    {
        /* Ignore frame padding after the 16 standard SGPIO slots. */
        return;
    }

    byte_index = (uint8_t)(bit_index >> 3U);
    bit_mask = (uint8_t)(1U << (bit_index & 0x7U));

    if (byte_index < SGPIO_SLAVE_RX_MAX_BYTES)
    {
        if (bit_value != 0U)
        {
            g_sgpio_raw[byte_index] = (uint8_t)(g_sgpio_raw[byte_index] | bit_mask);
        }
    }
    else
    {
        g_sgpio_overflow = 1U;
    }

    g_sgpio_slot_bit_count++;
}

static uint8_t sgpio_get_raw_bit(const uint8_t *raw, uint16_t bit_index)
{
    uint8_t byte_index;
    uint8_t bit_mask;
    uint8_t value;

    byte_index = (uint8_t)(bit_index >> 3U);
    bit_mask = (uint8_t)(1U << (bit_index & 0x7U));

    if (byte_index >= SGPIO_SLAVE_RX_MAX_BYTES)
    {
        value = 0U;
    }
    else if ((raw[byte_index] & bit_mask) != 0U)
    {
        value = 1U;
    }
    else
    {
        value = 0U;
    }

    return value;
}

static uint8_t sgpio_is_valid_captured_bit_count(uint16_t bit_count)
{
    uint8_t valid;

    /*
     * Pico emits SGPIO pairs in 16-pair PIO words. After the restart marker,
     * a valid MS51 RX-only capture is 26 bits for slot counts 1..8, 42 bits
     * for slot counts 9..14, or capped at 48 bits for slot counts 15..16.
     */
    if ((bit_count == 26U) || (bit_count == 42U) || (bit_count == SGPIO_SLOT_BIT_MAX))
    {
        valid = 1U;
    }
    else
    {
        valid = 0U;
    }

    return valid;
}

static void sgpio_finalize_capture(void)
{
    if ((g_sgpio_marker_seen != 0U) && (g_sgpio_slot_bit_count > 0U) && (g_sgpio_frame_ready == 0U))
    {
        g_sgpio_frame_count++;
        g_sgpio_frame_ready = 1U;
    }
    else if (g_sgpio_frame_ready != 0U)
    {
        g_sgpio_dropped_frames++;
    }

    g_sgpio_capture_active = 0U;
    g_sgpio_marker_seen = 0U;
}

static void sgpio_copy_frame_from_isr(SGPIO_FRAME_T *frame)
{
    uint8_t i;

    memset(frame, 0, sizeof(*frame));
    frame->frame_count = g_sgpio_frame_count;
    frame->dropped_frames = g_sgpio_dropped_frames;
    frame->bit_count = g_sgpio_slot_bit_count;
    frame->overflow = g_sgpio_overflow;
    frame->sload_raw = (uint8_t)(g_sgpio_sload_raw & 0x0FU);
    frame->sload_raw_valid = g_sgpio_sload_raw_valid;
    frame->low_sync_count = g_sgpio_low_sync_at_marker;
    frame->valid = 0U;

    if ((frame->low_sync_count >= SGPIO_LOW_SYNC_MIN_BITS) &&
        (sgpio_is_valid_captured_bit_count(frame->bit_count) != 0U))
    {
        frame->valid = 1U;
    }

    frame->raw_len = (uint8_t)((frame->bit_count + 7U) >> 3U);
    if (frame->raw_len > SGPIO_SLAVE_RX_MAX_BYTES)
    {
        frame->raw_len = SGPIO_SLAVE_RX_MAX_BYTES;
    }

    for (i = 0U; i < SGPIO_SLAVE_RX_MAX_BYTES; i++)
    {
        frame->raw[i] = g_sgpio_raw[i];
    }

    g_sgpio_frame_ready = 0U;
}

static void sgpio_decode_frame(SGPIO_FRAME_T *frame)
{
    uint8_t slot;
    uint16_t bit_index;

    frame->act_mask = 0U;
    frame->locate_mask = 0U;
    frame->fail_mask = 0U;

    for (slot = 0U; slot < SGPIO_DECODE_SLOT_COUNT; slot++)
    {
        bit_index = (uint16_t)(slot * SGPIO_DATA_BITS_PER_SLOT);
        if ((uint16_t)(bit_index + 2U) >= frame->bit_count)
        {
            break;
        }

        if (sgpio_get_raw_bit(frame->raw, bit_index) != 0U)
        {
            frame->act_mask |= (uint16_t)(1UL << slot);
        }
        if (sgpio_get_raw_bit(frame->raw, (uint16_t)(bit_index + 1U)) != 0U)
        {
            frame->locate_mask |= (uint16_t)(1UL << slot);
        }
        if (sgpio_get_raw_bit(frame->raw, (uint16_t)(bit_index + 2U)) != 0U)
        {
            frame->fail_mask |= (uint16_t)(1UL << slot);
        }
    }
}

static void sgpio_print_slot_mask(const char *label, uint16_t mask)
{
    uint8_t slot;
    uint8_t printed;

    printf("%s", label);
    printed = 0U;
    for (slot = 0U; slot < SGPIO_DECODE_SLOT_COUNT; slot++)
    {
        if ((mask & (uint16_t)(1UL << slot)) != 0U)
        {
            if (printed != 0U)
            {
                printf(",");
            }
            printf("%u", (unsigned int)slot);
            printed = 1U;
        }
    }

    if (printed == 0U)
    {
        printf("-");
    }
}

static void sgpio_print_slot_decode(const SGPIO_FRAME_T *frame)
{
    printf("  slots: ");
    sgpio_print_slot_mask("ACT=", frame->act_mask);
    printf(" ");
    sgpio_print_slot_mask("LOCATE=", frame->locate_mask);
    printf(" ");
    sgpio_print_slot_mask("FAIL=", frame->fail_mask);
    printf("\r\n");
}

static void sgpio_print_raw_bytes(const SGPIO_FRAME_T *frame)
{
    uint8_t i;

    printf("  raw:");
    for (i = 0U; i < frame->raw_len; i++)
    {
        printf(" %02X", (unsigned int)frame->raw[i]);
    }
    if (frame->raw_len == 0U)
    {
        printf(" -");
    }
    printf("\r\n");
}

static void sgpio_print_slot_bits(const SGPIO_FRAME_T *frame)
{
    uint8_t slot;
    uint8_t act_bit;
    uint8_t locate_bit;
    uint8_t fail_bit;
    uint8_t printed;
    uint16_t bit_index;

    printed = 0U;
    printf("  bits S0..S7:");
    for (slot = 0U; slot < SGPIO_DECODE_SLOT_COUNT; slot++)
    {
        bit_index = (uint16_t)(slot * SGPIO_DATA_BITS_PER_SLOT);
        if ((uint16_t)(bit_index + 2U) >= frame->bit_count)
        {
            break;
        }

        act_bit = sgpio_get_raw_bit(frame->raw, bit_index);
        locate_bit = sgpio_get_raw_bit(frame->raw, (uint16_t)(bit_index + 1U));
        fail_bit = sgpio_get_raw_bit(frame->raw, (uint16_t)(bit_index + 2U));
        if (slot == 8U)
        {
            printf("\r\n  bits S8..S15:");
        }
        printf(" S%u=%u%u%u",
               (unsigned int)slot,
               (unsigned int)act_bit,
               (unsigned int)locate_bit,
               (unsigned int)fail_bit);
        printed = 1U;
    }
    if (printed == 0U)
    {
        printf(" -");
    }
    printf("\r\n");
}

/*
 * SGPIO SDATAOUT raw bytes are logged LSB-first in capture order.
 * For slot N:
 *   bit (N * 3) + 0 = ACT
 *   bit (N * 3) + 1 = LOCATE
 *   bit (N * 3) + 2 = FAIL
 *
 * Example:
 *   raw 38 8E C3 00
 *   byte 0x38 is read as bits 0,0,0,1,1,1,0,0
 *   triples are printed as:
 *   bits S0..S7: S0=000 S1=111 S2=000 S3=111 S4=000 S5=111 S6=000 S7=011
 *   bits S8..S15: S8=...
 * In each triple, the digit order is ACT, LOCATE, FAIL.
 */
static void sgpio_print_frame(const SGPIO_FRAME_T *frame)
{
    printf("[SGPIO RX] frame #%lu\r\n",
           (unsigned long)frame->frame_count);
    printf("  capture: bits=%u bytes=%u valid=%u overflow=%u dropped=%lu low_sync=%u\r\n",
           (unsigned int)frame->bit_count,
           (unsigned int)frame->raw_len,
           (unsigned int)frame->valid,
           (unsigned int)frame->overflow,
           (unsigned long)frame->dropped_frames,
           (unsigned int)frame->low_sync_count);
    printf("  sload: L0..L3=0x%X valid=%u\r\n",
           (unsigned int)frame->sload_raw,
           (unsigned int)frame->sload_raw_valid);
    printf("  masks: ACT=0x%04X LOCATE=0x%04X FAIL=0x%04X\r\n",
           (unsigned int)frame->act_mask,
           (unsigned int)frame->locate_mask,
           (unsigned int)frame->fail_mask);
    sgpio_print_raw_bytes(frame);
    sgpio_print_slot_bits(frame);
    sgpio_print_slot_decode(frame);
    printf("\r\n");
}

static void sgpio_app_slot(uint8_t slot, uint8_t act, uint8_t locate, uint8_t fail)
{
    switch (slot)
    {
    case 0U:
        /*
         * Slot 0 application hook.
         * Replace the comments below with product behavior, such as setting
         * GPIO/LED/device state. Prefer setting outputs to the decoded state
         * instead of toggling on every repeated SGPIO frame unless repeated
         * toggle behavior is intentionally required.
         */
        if (act != 0U)
        {
            /* Slot 0 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 0 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 0 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 1U:
        if (act != 0U)
        {
            /* Slot 1 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 1 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 1 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 2U:
        if (act != 0U)
        {
            /* Slot 2 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 2 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 2 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 3U:
        if (act != 0U)
        {
            /* Slot 3 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 3 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 3 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 4U:
        if (act != 0U)
        {
            /* Slot 4 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 4 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 4 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 5U:
        if (act != 0U)
        {
            /* Slot 5 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 5 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 5 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 6U:
        if (act != 0U)
        {
            /* Slot 6 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 6 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 6 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 7U:
        if (act != 0U)
        {
            /* Slot 7 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 7 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 7 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 8U:
        if (act != 0U)
        {
            /* Slot 8 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 8 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 8 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 9U:
        if (act != 0U)
        {
            /* Slot 9 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 9 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 9 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 10U:
        if (act != 0U)
        {
            /* Slot 10 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 10 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 10 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 11U:
        if (act != 0U)
        {
            /* Slot 11 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 11 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 11 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 12U:
        if (act != 0U)
        {
            /* Slot 12 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 12 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 12 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 13U:
        if (act != 0U)
        {
            /* Slot 13 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 13 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 13 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 14U:
        if (act != 0U)
        {
            /* Slot 14 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 14 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 14 FAIL asserted: insert expected behavior here. */
        }
        break;

    case 15U:
        if (act != 0U)
        {
            /* Slot 15 ACT asserted: insert expected behavior here. */
        }
        if (locate != 0U)
        {
            /* Slot 15 LOCATE asserted: insert expected behavior here. */
        }
        if (fail != 0U)
        {
            /* Slot 15 FAIL asserted: insert expected behavior here. */
        }
        break;

    default:
        break;
    }
}

static void sgpio_app_frame(const SGPIO_FRAME_T *frame)
{
    uint8_t slot;
    uint8_t act;
    uint8_t locate;
    uint8_t fail;
    uint16_t bit_index;
    uint16_t slot_mask;

    if ((frame->valid == 0U) || (frame->overflow != 0U))
    {
        return;
    }

    for (slot = 0U; slot < SGPIO_DECODE_SLOT_COUNT; slot++)
    {
        bit_index = (uint16_t)(slot * SGPIO_DATA_BITS_PER_SLOT);
        if ((uint16_t)(bit_index + 2U) >= frame->bit_count)
        {
            break;
        }

        slot_mask = (uint16_t)(1UL << slot);
        act = (uint8_t)(((frame->act_mask & slot_mask) != 0U) ? 1U : 0U);
        locate = (uint8_t)(((frame->locate_mask & slot_mask) != 0U) ? 1U : 0U);
        fail = (uint8_t)(((frame->fail_mask & slot_mask) != 0U) ? 1U : 0U);

        sgpio_app_slot(slot, act, locate, fail);
    }
}

static uint8_t sgpio_frame_same(const SGPIO_FRAME_T *a, const SGPIO_FRAME_T *b)
{
    uint8_t i;
    uint8_t same;

    same = 1U;

    if ((a->valid != b->valid) ||
        (a->overflow != b->overflow) ||
        (a->bit_count != b->bit_count) ||
        (a->raw_len != b->raw_len) ||
        (a->sload_raw != b->sload_raw) ||
        (a->sload_raw_valid != b->sload_raw_valid) ||
        (a->low_sync_count != b->low_sync_count) ||
        (a->act_mask != b->act_mask) ||
        (a->locate_mask != b->locate_mask) ||
        (a->fail_mask != b->fail_mask))
    {
        same = 0U;
    }

    if (same != 0U)
    {
        for (i = 0U; i < SGPIO_SLAVE_RX_MAX_BYTES; i++)
        {
            if (a->raw[i] != b->raw[i])
            {
                same = 0U;
                break;
            }
        }
    }

    return same;
}

static void sgpio_set_filter_candidate(const SGPIO_FRAME_T *frame)
{
    memcpy(&g_sgpio_filter_candidate, frame, sizeof(g_sgpio_filter_candidate));
    g_sgpio_filter_candidate_valid = 1U;
    g_sgpio_filter_repeat_count = 1U;
}

static void sgpio_print_unstable_frame(const SGPIO_FRAME_T *frame, unsigned long now)
{
    if ((unsigned long)(now - g_sgpio_last_unstable_log_tick) < SGPIO_UNSTABLE_LOG_MIN_INTERVAL_MS)
    {
        return;
    }

    printf("[SGPIO RX] unstable frame ignored\r\n");
    printf("  capture: bits=%u bytes=%u valid=%u overflow=%u low_sync=%u\r\n",
           (unsigned int)frame->bit_count,
           (unsigned int)frame->raw_len,
           (unsigned int)frame->valid,
           (unsigned int)frame->overflow,
           (unsigned int)frame->low_sync_count);
    printf("  raw0=0x%02X candidate_repeat=%u\r\n\r\n",
           (unsigned int)frame->raw[0],
           (unsigned int)g_sgpio_filter_repeat_count);
    g_sgpio_last_unstable_log_tick = now;
}

static uint8_t sgpio_accept_stable_frame(const SGPIO_FRAME_T *frame, unsigned long now)
{
    uint8_t accepted;

    accepted = 0U;

    if ((frame->valid == 0U) || (frame->overflow != 0U))
    {
        sgpio_set_filter_candidate(frame);
        sgpio_print_unstable_frame(frame, now);
        return 0U;
    }

    if ((g_sgpio_last_frame.valid != 0U) && (sgpio_frame_same(frame, &g_sgpio_last_frame) != 0U))
    {
        accepted = 1U;
    }
    else if ((g_sgpio_filter_candidate_valid != 0U) &&
             (sgpio_frame_same(frame, &g_sgpio_filter_candidate) != 0U))
    {
        if (g_sgpio_filter_repeat_count < 0xFFU)
        {
            g_sgpio_filter_repeat_count++;
        }
        if (g_sgpio_filter_repeat_count >= SGPIO_FRAME_STABLE_REQUIRED)
        {
            accepted = 1U;
        }
    }
    else
    {
        sgpio_set_filter_candidate(frame);
    }

    if (accepted == 0U)
    {
        return 0U;
    }

    g_sgpio_filter_candidate_valid = 0U;
    g_sgpio_filter_repeat_count = 0U;
    return 1U;
}

void SGPIO_OnClockRisingSampledIrq(uint8_t sload_value, uint8_t sdata_value)
{
    unsigned long now;

    now = get_systick();

    if ((g_sgpio_capture_active != 0U) &&
        (g_sgpio_marker_seen != 0U) &&
        ((unsigned long)(now - g_sgpio_last_clock_tick) >= SGPIO_FRAME_GAP_TIMEOUT_MS))
    {
        sgpio_finalize_capture();
    }

    g_sgpio_last_clock_tick = now;

    if (g_sgpio_marker_seen == 0U)
    {
        if (sload_value == 0U)
        {
            if (g_sgpio_capture_active == 0U)
            {
                if (g_sgpio_frame_ready != 0U)
                {
                    g_sgpio_dropped_frames++;
                    g_sgpio_frame_ready = 0U;
                }

                sgpio_reset_capture_state();
                g_sgpio_capture_active = 1U;
                g_sgpio_capture_start_tick = now;
                g_sgpio_last_clock_tick = now;
            }

            if (g_sgpio_low_sync_count < 0xFFU)
            {
                g_sgpio_low_sync_count++;
            }
        }
        else if (g_sgpio_capture_active == 0U)
        {
            return;
        }
        else if (g_sgpio_low_sync_count >= SGPIO_LOW_SYNC_MIN_BITS)
        {
            g_sgpio_marker_seen = 1U;
            g_sgpio_low_sync_at_marker = g_sgpio_low_sync_count;
        }
        else
        {
            sgpio_reset_capture_state();
        }
        return;
    }

    if (g_sgpio_slot_bit_count < SGPIO_SLOAD_RAW_BITS)
    {
        if (sload_value != 0U)
        {
            g_sgpio_sload_raw = (uint8_t)(g_sgpio_sload_raw | (uint8_t)(1U << g_sgpio_slot_bit_count));
        }
        if (g_sgpio_slot_bit_count == (uint16_t)(SGPIO_SLOAD_RAW_BITS - 1U))
        {
            g_sgpio_sload_raw_valid = 1U;
        }
    }

    sgpio_store_sdata_bit(sdata_value);
}

void SGPIO_Process(void)
{
    SGPIO_FRAME_T local_frame;
    unsigned long now;

    now = get_systick();

    if (g_sgpio_capture_active != 0U)
    {
        if (((g_sgpio_marker_seen != 0U) && ((unsigned long)(now - g_sgpio_last_clock_tick) >= SGPIO_FRAME_GAP_TIMEOUT_MS)) ||
            ((unsigned long)(now - g_sgpio_capture_start_tick) >= SGPIO_FRAME_ARM_TIMEOUT_MS))
        {
            DISABLE_PIN_INTERRUPT;
            sgpio_finalize_capture();
            ENABLE_PIN_INTERRUPT;
        }
    }

    if (g_sgpio_frame_ready == 0U)
    {
        return;
    }

    DISABLE_PIN_INTERRUPT;
    sgpio_copy_frame_from_isr(&local_frame);
    sgpio_reset_capture_state();
    ENABLE_PIN_INTERRUPT;

    sgpio_decode_frame(&local_frame);
    if (sgpio_accept_stable_frame(&local_frame, now) == 0U)
    {
        return;
    }
    memcpy(&g_sgpio_last_frame, &local_frame, sizeof(g_sgpio_last_frame));

    /*
     * Run product behavior for every stable accepted frame. Keep this outside
     * the debug log rate limit so SGPIO host commands are not delayed by
     * printf throttling.
    */
    sgpio_app_frame(&local_frame);

    if ((local_frame.frame_count <= SGPIO_FRAME_LOG_FIRST_N) ||
        ((unsigned long)(now - g_sgpio_last_log_tick) >= SGPIO_FRAME_LOG_MIN_INTERVAL_MS))
    {
        sgpio_print_frame(&local_frame);
        g_sgpio_last_log_tick = now;
    }
}

void SGPIO_Init(void)
{
    /*
     * Keep SGPIO pins in GPIO input mode. The receiver uses the MS51 pin
     * interrupt path; SLOAD and SDATA OUT are sampled synchronously by SCLK.
     */
    SGPIO_SLAVE_SCLK_INPUT_MODE;
    SGPIO_SLAVE_SDOUT_INPUT_MODE;
    SGPIO_SLAVE_SLOAD_INPUT_MODE;

    sgpio_reset_capture_state();
    memset(&g_sgpio_last_frame, 0, sizeof(g_sgpio_last_frame));
    memset(&g_sgpio_filter_candidate, 0, sizeof(g_sgpio_filter_candidate));
    g_sgpio_filter_candidate_valid = 0U;
    g_sgpio_filter_repeat_count = 0U;
    g_sgpio_last_unstable_log_tick = 0UL;

    DISABLE_PIN_INTERRUPT;
    SGPIO_SLAVE_SELECT_INT_PORT;
    SGPIO_SLAVE_ENABLE_SCLK_TRIG;
    SGPIO_SLAVE_CLEAR_SCLK_INT_FLAG;
    SET_INT_PIT_LEVEL3;
    ENABLE_PIN_INTERRUPT;
    ENABLE_GLOBAL_INTERRUPT;

    printf("%s/SLOAD GPIO input sampled by SCLK\r\n", SGPIO_SLAVE_SLOAD_PIN_NAME);
    printf("%s/SDATAOUT GPIO input sampled by SCLK\r\n", SGPIO_SLAVE_SDOUT_PIN_NAME);
    printf("%s/SCLK shared GPIO ISR rising sampler\r\n", SGPIO_SLAVE_SCLK_PIN_NAME);
    printf("SGPIO GPIO ISR RX path, no SDATAIN TX\r\n");
}

void SGPIO_PinInterruptHandler(void)
{
    uint8_t sload_sample;
    uint8_t sdata_sample;

    /*
     * Shared pin interrupt rule:
     * the selected SCLK bit check must remain the first top-level branch in
     * this common pin interrupt handler. SGPIO depends on every SCLK rising
     * edge, so future unrelated pin handling must be added below this block
     * and must not delay the SCLK sampling path.
     */
    if (SGPIO_SLAVE_SCLK_INT_PENDING)
    {
        sload_sample = SGPIO_SLAVE_SLOAD_IO;
        sdata_sample = SGPIO_SLAVE_SDOUT_IO;
        SGPIO_SLAVE_CLEAR_SCLK_INT_FLAG;
        SGPIO_OnClockRisingSampledIrq(sload_sample, sdata_sample);
    }
}
