/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#include <string.h>
#include "transmit_to_dac.h"
#include "usb_device_control.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/regs/dma.h"
#include "hardware/regs/pio.h"
#include "i2s_pio_interface.h"
#include "i2s_rate.h"
#include "idle_clock.h"
#include "audio_fade.h"
#include <stdatomic.h>
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "DMA fault handling requires lock-free atomic integers");
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "DMA fault handling requires lock-free atomic booleans");

static const PIO pio = pio0;
static const uint sm = 0;
static pio_sm_config sm_config;
static uint offset;

#define DMA_TX_SRAM_BANK_BYTES (64u * 1024u)
#define DMA_SILENCE_FRAMES (64u)
#define DMA_EMPTY_FADE_MS (5u)
#define DMA_EMPTY_FADE_MAX_FRAMES ((192000u * DMA_EMPTY_FADE_MS + 999u) / 1000u)
#define DMA_START_FADE_MS (5u)
#define DMA_STARTUP_SILENCE_US (7000000u)
#define DAC_ZERO_DETECT_FRAMES (1024u)
#define DAC_ZERO_SETTLE_MARGIN_US (1000u)

_Static_assert((AUDIO_FREQ_MAX * DMA_START_FADE_MS + 999u) / 1000u <= 65535u,
               "Fade frame count must fit in Q16 numerator");

static uint32_t __attribute__((section(".dma_sram_bank7")))
    dma_tx_buffers[DEPTH_DMA_TX_BUFFER][SIZE_DMA_TX_BUF];

_Static_assert(sizeof(dma_tx_buffers) <= DMA_TX_SRAM_BANK_BYTES,
               "DMA buffers exceed SRAM bank 7");

static int dma_ch;
static volatile bool dma_running;
static volatile bool dma_running_pcm;
static volatile bool dma_cleanly_stopped;
static atomic_uint dma_ahb_fault;
static atomic_bool dma_fault_quiescent;
static atomic_bool dma_startup_silence = true;
static atomic_bool dma_clock_ready = true;
static volatile bool dma_startup_clock_started;
static volatile uint32_t dma_startup_silence_since_us;
static bool dma_idle_clock_stopped;
static volatile bool dma_restart_in_progress;
static volatile bool dma_restart_clock_started;
static volatile uint32_t dma_restart_since_us;
static idle_clock_timer_t dma_idle_timer;
static bool dma_idle_silence_started;
static uint32_t dma_idle_silence_since_us;
static uint32_t dma_silence[(DMA_EMPTY_FADE_MAX_FRAMES + DMA_SILENCE_FRAMES) * 2u];
static volatile bool dma_was_silence = true;
static volatile bool dma_fade_out_in_flight;
static uint32_t dma_fade_in_total;
static uint32_t dma_fade_in_remaining;
static volatile int32_t dma_last_pcm_left;
static volatile int32_t dma_last_pcm_right;
DMA_TX_STRUCTURE dma_tx;

static void dma_tx_reset(void)
{
    dma_tx.wp = 0;
    dma_tx.rp = 0;
    dma_tx.using = 0;
    dma_tx.prev_write_length = 0;
    for (uint32_t i = 0; i < DEPTH_DMA_TX_BUFFER; i++)
    {
        dma_tx.data[i].tx_buf = dma_tx_buffers[i];
        dma_tx.data[i].tx_size = 0;
    }
    memset(dma_silence, 0, sizeof(dma_silence));
    dma_was_silence = true;
    dma_fade_out_in_flight = false;
    dma_fade_in_total = 0u;
    dma_fade_in_remaining = 0u;
    dma_last_pcm_left = 0;
    dma_last_pcm_right = 0;
}

volatile bool enable_output = false;

float i2s_actual_sample_rate(void)
{
    return i2s_rate_from_clkdiv(clock_get_hz(clk_sys), pio->sm[sm].clkdiv);
}

void __not_in_flash_func(dma_tx_start)(void);

static bool __not_in_flash_func(dma_tx_check_ahb_errors)(void)
{
    const uint32_t error = dma_hw->ch[dma_ch].ctrl_trig &
        (DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS);
    if (error != 0u)
    {
        dma_channel_set_irq0_enabled((uint)dma_ch, false);
        atomic_store_explicit(&dma_ahb_fault, error, memory_order_release);
    }
    return error != 0u;
}

bool dma_tx_fault_pending(void)
{
    return atomic_load_explicit(&dma_fault_quiescent, memory_order_acquire);
}

bool dma_tx_startup_silence_active(void)
{
    return atomic_load_explicit(&dma_startup_silence, memory_order_acquire);
}

bool dma_tx_clock_ready(void)
{
    return atomic_load_explicit(&dma_clock_ready, memory_order_acquire);
}

bool dma_tx_is_silent(void)
{
    return dma_was_silence && !dma_fade_out_in_flight &&
        !dma_running_pcm && dma_tx.using == 0;
}

static void __not_in_flash_func(dma_tx_schedule)(void)
{
    if (dma_running || atomic_load_explicit(&dma_ahb_fault, memory_order_acquire) != 0u)
        return;

    uint32_t *buffer;
    uint32_t words;
    if (dma_tx_startup_silence_active() || dma_restart_in_progress)
    {
        buffer = dma_silence;
        words = DMA_SILENCE_FRAMES * 2u;
        dma_running_pcm = false;
    }
    else if (dma_tx.using != 0)
    {
        const uint32_t rp = dma_tx.rp;
        buffer = dma_tx.data[rp].tx_buf;
        words = dma_tx.data[rp].tx_size;
        dma_tx.rp = (rp + 1u) % DEPTH_DMA_TX_BUFFER;
        dma_running_pcm = true;

        int32_t *samples = (int32_t *)buffer;
        const uint32_t frames = words / 2u;
        if (dma_was_silence)
        {
            dma_fade_in_total = (audio_state.freq * DMA_START_FADE_MS + 999u) / 1000u;
            if (dma_fade_in_total == 0u)
                dma_fade_in_total = 1u;
            dma_fade_in_remaining = dma_fade_in_total;
            dma_was_silence = false;
        }
        for (uint32_t frame = 0; frame < frames && dma_fade_in_remaining != 0u; frame++)
        {
            const uint32_t numerator = dma_fade_in_total - dma_fade_in_remaining + 1u;
            const uint32_t gain = audio_fade_gain_q16(numerator, dma_fade_in_total);
            samples[2u * frame] = audio_fade_apply_q16(samples[2u * frame], gain);
            samples[2u * frame + 1u] = audio_fade_apply_q16(samples[2u * frame + 1u], gain);
            dma_fade_in_remaining--;
        }
        dma_last_pcm_left = samples[words - 2u];
        dma_last_pcm_right = samples[words - 1u];
    }
    else
    {
        buffer = dma_silence;
        words = DMA_SILENCE_FRAMES * 2u;
        dma_running_pcm = false;
        if (!dma_was_silence)
        {
            uint32_t fade_frames =
                (audio_state.freq * DMA_EMPTY_FADE_MS + 999u) / 1000u;
            if (fade_frames == 0u)
                fade_frames = 1u;
            if (fade_frames > DMA_EMPTY_FADE_MAX_FRAMES)
                fade_frames = DMA_EMPTY_FADE_MAX_FRAMES;
            for (uint32_t frame = 0; frame < fade_frames; frame++)
            {
                const uint32_t numerator = fade_frames - frame - 1u;
                const uint32_t gain = audio_fade_gain_q16(numerator, fade_frames);
                dma_silence[2u * frame] =
                    (uint32_t)audio_fade_apply_q16(dma_last_pcm_left, gain);
                dma_silence[2u * frame + 1u] =
                    (uint32_t)audio_fade_apply_q16(dma_last_pcm_right, gain);
            }
            memset(&dma_silence[fade_frames * 2u], 0,
                   DMA_SILENCE_FRAMES * 2u * sizeof(dma_silence[0]));
            // The trailing zero block lets the PIO drain the fade before a clock change.
            words = (fade_frames + DMA_SILENCE_FRAMES) * 2u;
            dma_fade_out_in_flight = true;
            dma_last_pcm_left = 0;
            dma_last_pcm_right = 0;
            dma_was_silence = true;
            dma_fade_in_remaining = 0u;
        }
        else
        {
            memset(dma_silence, 0, DMA_SILENCE_FRAMES * 2u * sizeof(dma_silence[0]));
        }
    }

    dma_running = true;
    dma_cleanly_stopped = false;
    dma_channel_set_read_addr(dma_ch, buffer, false);
    dma_channel_set_trans_count(dma_ch, words, true);
}

static void __not_in_flash_func(dma_tx_irq_handler)(void)
{
    if (!dma_channel_get_irq0_status(dma_ch))
        return;
    dma_channel_acknowledge_irq0(dma_ch);
    if (dma_tx_check_ahb_errors())
        return;
    if (!dma_running)
        return;
    if (dma_tx_startup_silence_active() && !dma_startup_clock_started)
    {
        pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
        dma_startup_silence_since_us = time_us_32();
        dma_startup_clock_started = true;
    }
    if (dma_restart_in_progress && !dma_restart_clock_started)
    {
        pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
        dma_restart_since_us = time_us_32();
        dma_restart_clock_started = true;
    }
    if (dma_running_pcm)
        dma_tx.using--;
    else if (dma_fade_out_in_flight)
        dma_fade_out_in_flight = false;
    dma_running = false;
    dma_tx_schedule();
}

void reset_i2s_freq(void)
{
    I2S_freq_init(pio, sm, &sm_config, offset);
}

void init_i2s_interface(void)
{
    if(I2S_SLEWRATE_FAST_ENABLE)
    {
        gpio_set_slew_rate(I2S_DATA_PIN, GPIO_SLEW_RATE_FAST);
        gpio_set_slew_rate(I2S_SIDESET_BASE, GPIO_SLEW_RATE_FAST);
        gpio_set_slew_rate(I2S_SIDESET_BASE + 1, GPIO_SLEW_RATE_FAST);
    }

    if(I2S_STRENGTH_REINFORCE_ENABLE)
    {
        gpio_set_drive_strength(I2S_DATA_PIN, GPIO_DRIVE_STRENGTH_8MA);
        gpio_set_drive_strength(I2S_SIDESET_BASE, GPIO_DRIVE_STRENGTH_8MA);
        gpio_set_drive_strength(I2S_SIDESET_BASE + 1, GPIO_DRIVE_STRENGTH_8MA);
    }

    // Initialize PIO I2S.
    if(I2S_SIDESET_CHANGE)
    {
        I2S_program_init(pio, sm, &I2S_32bit_inv_program,
                          I2S_32bit_inv_program_get_default_config,
                          I2S_DATA_PIN, I2S_SIDESET_BASE, AUDIO_INITIAL_FREQ,
                          &sm_config, &offset);
    }
    else
    {
        I2S_program_init(pio, sm, &I2S_32bit_program,
                          I2S_32bit_program_get_default_config,
                          I2S_DATA_PIN, I2S_SIDESET_BASE, AUDIO_INITIAL_FREQ,
                          &sm_config, &offset);
    }

    reset_i2s_freq();

    // Software-trigger each transfer so a delayed IRQ cannot restart stale data.
    dma_ch = dma_claim_unused_channel(true);
    dma_channel_config config = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_dreq(&config, pio_get_dreq(pio, sm, true));

    dma_tx_reset();
    dma_running = false;
    dma_running_pcm = false;
    dma_cleanly_stopped = false;
    atomic_store_explicit(&dma_ahb_fault, 0u, memory_order_relaxed);
    atomic_store_explicit(&dma_fault_quiescent, false, memory_order_relaxed);
    dma_channel_configure(dma_ch, &config,
                          &pio->txf[sm], dma_tx.data[0].tx_buf, 0, false);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_tx_irq_handler);
    dma_channel_set_irq0_enabled(dma_ch, true);
    irq_set_enabled(DMA_IRQ_0, true);

    const uint32_t irq_state = save_and_disable_interrupts();
    dma_tx_schedule();
    restore_interrupts(irq_state);
}

// Maximum number of stereo frames copied into one DMA buffer.
#define DMA_MAX_FRAMES (SIZE_DMA_TX_BUF / 2)

static volatile bool enable_output_prev = false;
extern volatile absolute_time_t time_start_output;

static uint32_t dma_zero_detect_us(void)
{
    return (DAC_ZERO_DETECT_FRAMES * 1000000u + audio_state.freq - 1u) /
        audio_state.freq + DAC_ZERO_SETTLE_MARGIN_US;
}

void __not_in_flash_func(dma_tx_start)(void)
{
    if (atomic_load_explicit(&dma_ahb_fault, memory_order_acquire) != 0u)
    {
        atomic_store_explicit(&dma_fault_quiescent, true, memory_order_release);
        return;
    }
    const uint32_t now_us = time_us_32();
    const uint32_t rx_sequence = usb_audio_rx_sequence();
    const bool received = dma_idle_timer.initialized && dma_idle_timer.sequence != rx_sequence;
    const bool idle_expired = idle_clock_expired(&dma_idle_timer, now_us, rx_sequence,
                                                  dma_tx_startup_silence_active());
    const usb_audio_state_t state = usb_audio_get_state();
    if (dma_idle_clock_stopped)
    {
        if (!received && state != USB_AUDIO_SWITCHING)
            return;
        dma_idle_clock_stopped = false;
        dma_restart_in_progress = true;
        dma_restart_clock_started = false;
        dma_idle_silence_started = false;
        (void)idle_clock_expired(&dma_idle_timer, now_us, rx_sequence, true);
    }
    if (dma_cleanly_stopped)
    {
        reset_i2s_freq();
        dma_idle_silence_started = false;
    }
    if (dma_tx_startup_silence_active())
    {
        if (!dma_startup_clock_started)
        {
            const uint32_t irq_state = save_and_disable_interrupts();
            dma_tx_schedule();
            restore_interrupts(irq_state);
            return;
        }
        const uint32_t now_us = time_us_32();
        const uint32_t stall_bit = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
        if ((pio->fdebug & stall_bit) != 0u)
        {
            pio->fdebug = stall_bit;
            dma_startup_silence_since_us = now_us;
        }
        else if ((uint32_t)(now_us - dma_startup_silence_since_us) >= DMA_STARTUP_SILENCE_US)
        {
            atomic_store_explicit(&dma_startup_silence, false, memory_order_release);
        }
        if (dma_tx_startup_silence_active())
        {
            const uint32_t irq_state = save_and_disable_interrupts();
            dma_tx_schedule();
            restore_interrupts(irq_state);
            return;
        }
    }
    if (dma_restart_in_progress)
    {
        if (!dma_restart_clock_started)
        {
            const uint32_t irq_state = save_and_disable_interrupts();
            dma_tx_schedule();
            restore_interrupts(irq_state);
            return;
        }
        const uint32_t restart_now_us = time_us_32();
        const uint32_t stall_bit = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
        if ((pio->fdebug & stall_bit) != 0u)
        {
            pio->fdebug = stall_bit;
            dma_restart_since_us = restart_now_us;
        }
        else if ((uint32_t)(restart_now_us - dma_restart_since_us) >= dma_zero_detect_us())
        {
            dma_restart_in_progress = false;
            atomic_store_explicit(&dma_clock_ready, true, memory_order_release);
        }
        if (dma_restart_in_progress)
        {
            const uint32_t irq_state = save_and_disable_interrupts();
            dma_tx_schedule();
            restore_interrupts(irq_state);
            return;
        }
    }

    // main.c's 500 ms idle cleanup must remain earlier than the 1 s idle-clock
    // timeout: it exits STREAMING and clears sub-prefill remnants so quiet can become true.
    const uint32_t idle_irq_state = save_and_disable_interrupts();
    const bool quiet = !enable_output && dma_tx_is_silent() &&
        get_size_using(&buffer_audio) == 0;
    if (!quiet)
        dma_idle_silence_started = false;
    else if (!dma_idle_silence_started)
    {
        dma_idle_silence_started = true;
        dma_idle_silence_since_us = now_us;
    }
    if (quiet && idle_expired && state != USB_AUDIO_SWITCHING &&
        (uint32_t)(now_us - dma_idle_silence_since_us) >= dma_zero_detect_us() &&
        usb_audio_rx_sequence() == rx_sequence)
    {
        atomic_store_explicit(&dma_clock_ready, false, memory_order_release);
        dma_idle_clock_stopped = true;
        pio_sm_set_enabled(pio, sm, false);
        dma_stop_and_clear();
        pio_sm_set_pins_with_mask(pio, sm, 0,
            (1u << I2S_DATA_PIN) | (1u << I2S_SIDESET_BASE) | (1u << (I2S_SIDESET_BASE + 1u)));
        restore_interrupts(idle_irq_state);
        return;
    }
    restore_interrupts(idle_irq_state);

    const bool ring_paused = state == USB_AUDIO_BUFFERING;
    if (ring_paused)
    {
        const uint32_t irq_state = save_and_disable_interrupts();
        dma_tx_schedule();
        if (dma_tx_is_silent())
        {
            enable_output = false;
            enable_output_prev = false;
        }
        restore_interrupts(irq_state);
        return;
    }
    int32_t level = (int32_t)get_size_using(&buffer_audio) / 2;
    if ((state == USB_AUDIO_STREAMING && level > 0) ||
        (state != USB_AUDIO_STREAMING && level >= SIZE_AUDIO_PREFILL_FRAMES))
        enable_output = true;
    else if (state != USB_AUDIO_STREAMING && state != USB_AUDIO_OVERFLOW_FATAL &&
             level == 0 && dma_tx_is_silent())
    {
        enable_output = false;
    }


    if (enable_output && !enable_output_prev)
    {
        time_start_output = get_absolute_time();
    }
    enable_output_prev = enable_output;

    if (!enable_output)
    {
        const uint32_t irq_state = save_and_disable_interrupts();
        dma_tx_schedule();
        restore_interrupts(irq_state);
        return;
    }

    while (dma_tx.using < SIZE_DMA_TX_BUF_STACK)
    {
        const uint32_t irq_state = save_and_disable_interrupts();
        const usb_audio_state_t fetch_state = usb_audio_get_state();
        if (fetch_state == USB_AUDIO_BUFFERING)
        {
            restore_interrupts(irq_state);
            break;
        }

        int32_t frames = (int32_t)get_size_using(&buffer_audio) / 2;
        if (frames <= 0)
        {
            restore_interrupts(irq_state);
            break;
        }
        if (frames > DMA_MAX_FRAMES)
            frames = DMA_MAX_FRAMES;

        const uint32_t wp = dma_tx.wp;
        uint32_t *tx = dma_tx.data[wp].tx_buf;
        if (ringbuf_read_array_spinlock((int32_t *)tx, (uint32_t)frames * 2u, &buffer_audio) < 0)
        {
            restore_interrupts(irq_state);
            break;
        }

        dma_tx.data[wp].tx_size = (uint32_t)frames * 2u;
        dma_tx.wp = (wp + 1u) % DEPTH_DMA_TX_BUFFER;
        dma_tx.using++;
        restore_interrupts(irq_state);
    }

    const uint32_t irq_state = save_and_disable_interrupts();
    dma_tx_schedule();
    restore_interrupts(irq_state);
}

void dma_stop_and_clear(void)
{
    const uint32_t irq_state = save_and_disable_interrupts();
    if (dma_cleanly_stopped && !dma_running && dma_tx.using == 0)
    {
        restore_interrupts(irq_state);
        return;
    }
    dma_channel_set_irq0_enabled(dma_ch, false);
    // RP2350-E5: disable the channel before aborting it.
    hw_clear_bits(&dma_hw->ch[dma_ch].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(dma_ch);
    dma_hw->intr = 1u << dma_ch;
    const uint32_t ctrl = dma_hw->ch[dma_ch].al1_ctrl;
    dma_hw->ch[dma_ch].al1_ctrl = ctrl |
        DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS;

    // Clear the FIFO buffer.
    pio_sm_clear_fifos(pio0, 0);
    pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);

    // Reset queue metadata without clearing the DMA buffer pointers.
    dma_tx_reset();
    dma_running = false;
    dma_running_pcm = false;
    dma_cleanly_stopped = true;

    // A clock/rate change starts a fresh playback epoch.
    enable_output = false;
    enable_output_prev = false;
    hw_set_bits(&dma_hw->ch[dma_ch].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_set_irq0_enabled(dma_ch, true);
    restore_interrupts(irq_state);
}

void dma_tx_discard_pending(void)
{
    dma_tx.wp = dma_tx.rp;
    dma_tx.using = (dma_running && dma_running_pcm) ? 1u : 0u;
}

uint8_t dma_tx_recover_fault(void)
{
    const uint32_t error = atomic_load_explicit(&dma_ahb_fault, memory_order_acquire);
    dma_stop_and_clear();
    if (dma_tx_startup_silence_active())
        dma_startup_clock_started = false;
    if (dma_restart_in_progress)
        dma_restart_clock_started = false;
    atomic_store_explicit(&dma_fault_quiescent, false, memory_order_relaxed);
    atomic_store_explicit(&dma_ahb_fault, 0u, memory_order_release);
    return (uint8_t)(((error & DMA_CH0_CTRL_TRIG_READ_ERROR_BITS) ? 1u : 0u) |
                     ((error & DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS) ? 2u : 0u));
}
