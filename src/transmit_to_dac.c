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
#include <stdatomic.h>
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "DMA fault handling requires lock-free atomic integers");
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "DMA fault handling requires lock-free atomic booleans");

static const PIO pio = pio0;
static const uint sm = 0;
static pio_sm_config sm_config;
static uint offset;

#define DMA_TX_SRAM_BANK_BYTES (64u * 1024u)
#define DMA_SILENCE_FRAMES (64u)

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
static uint32_t dma_silence[DMA_SILENCE_FRAMES * 2u];
static volatile bool dma_was_silence = true;
static volatile int32_t dma_last_pcm_left;
static volatile int32_t dma_last_pcm_right;
volatile uint32_t dma_tx_underrun_count;
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
    dma_last_pcm_left = 0;
    dma_last_pcm_right = 0;
}

volatile bool enable_output = false;

float i2s_actual_sample_rate(void)
{
    return i2s_rate_from_clkdiv(clock_get_hz(clk_sys), pio->sm[sm].clkdiv);
}

void __not_in_flash_func(dma_tx_start)(void);
static bool prefill_waiting_for_pio = false;

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

static void __not_in_flash_func(dma_tx_schedule)(void)
{
    if (dma_running || atomic_load_explicit(&dma_ahb_fault, memory_order_acquire) != 0u)
        return;

    uint32_t *buffer;
    uint32_t words;
    if (dma_tx.using != 0)
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
            const uint32_t fade_frames = frames < DMA_SILENCE_FRAMES ? frames : DMA_SILENCE_FRAMES;
            for (uint32_t frame = 0; frame < fade_frames; frame++)
            {
                const int64_t numerator = frame + 1u;
                samples[2u * frame] = (int32_t)(((int64_t)samples[2u * frame] * numerator) / fade_frames);
                samples[2u * frame + 1u] = (int32_t)(((int64_t)samples[2u * frame + 1u] * numerator) / fade_frames);
            }
        }
        dma_last_pcm_left = samples[words - 2u];
        dma_last_pcm_right = samples[words - 1u];
        dma_was_silence = false;
    }
    else
    {
        const usb_audio_state_t state = usb_audio_get_state();
        if (!enable_output || (state != USB_AUDIO_STREAMING && state != USB_AUDIO_OVERFLOW_FATAL))
            return;
        buffer = dma_silence;
        words = DMA_SILENCE_FRAMES * 2u;
        dma_running_pcm = false;
        if (!dma_was_silence)
        {
            dma_tx_underrun_count++;
            for (uint32_t frame = 0; frame < DMA_SILENCE_FRAMES; frame++)
            {
                const int64_t numerator = DMA_SILENCE_FRAMES - frame - 1u;
                dma_silence[2u * frame] = (uint32_t)(((int64_t)dma_last_pcm_left * numerator) / DMA_SILENCE_FRAMES);
                dma_silence[2u * frame + 1u] = (uint32_t)(((int64_t)dma_last_pcm_right * numerator) / DMA_SILENCE_FRAMES);
            }
            dma_last_pcm_left = 0;
            dma_last_pcm_right = 0;
            dma_was_silence = true;
        }
        else
        {
            memset(dma_silence, 0, sizeof(dma_silence));
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
    if (dma_running_pcm)
        dma_tx.using--;
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
}

// Maximum number of stereo frames copied into one DMA buffer.
#define DMA_MAX_FRAMES (SIZE_DMA_TX_BUF / 2)

static volatile bool enable_output_prev = false;
extern volatile absolute_time_t time_start_output;

void __not_in_flash_func(dma_tx_start)(void)
{
    if (atomic_load_explicit(&dma_ahb_fault, memory_order_acquire) != 0u)
    {
        atomic_store_explicit(&dma_fault_quiescent, true, memory_order_release);
        return;
    }
    const usb_audio_state_t state = usb_audio_get_state();
    const bool ring_paused = state == USB_AUDIO_BUFFERING;
    if (ring_paused)
    {
        if (enable_output)
        {
            if (dma_tx.using != 0)
                prefill_waiting_for_pio = false;
            else if (!prefill_waiting_for_pio)
            {
                // DMA completion precedes playback of the words still in the PIO.
                pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
                prefill_waiting_for_pio = true;
            }
            else if ((pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + sm))) != 0u)
                dma_stop_and_clear();
        }
        return;
    }
    int32_t level = (int32_t)get_size_using(&buffer_audio) / 2;
    if (enable_output && (state == USB_AUDIO_IDLE || state == USB_AUDIO_SWITCHING) &&
        level == 0 && dma_tx.using == 0)
    {
        if (!prefill_waiting_for_pio)
        {
            pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
            prefill_waiting_for_pio = true;
        }
        else if ((pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + sm))) != 0u)
            dma_stop_and_clear();
        return;
    }
    prefill_waiting_for_pio = false;

    if ((state == USB_AUDIO_STREAMING && level > 0) ||
        (state != USB_AUDIO_STREAMING && level >= SIZE_AUDIO_PREFILL_FRAMES))
        enable_output = true;
    else if (state != USB_AUDIO_STREAMING && state != USB_AUDIO_OVERFLOW_FATAL &&
             level == 0 && dma_tx.using == 0)
    {
        enable_output = false;
    }


    if (enable_output && !enable_output_prev)
    {
        pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
        time_start_output = get_absolute_time();
        pio_sm_set_enabled(pio, sm, false);
        pio_sm_clear_fifos(pio, sm);
        pio_sm_restart(pio, sm);
        reset_i2s_freq();
    }
    enable_output_prev = enable_output;

    if (!enable_output)
    {
        if (!dma_cleanly_stopped)
            dma_stop_and_clear();
        return;
    }

    while (dma_tx.using < SIZE_DMA_TX_BUF_STACK)
    {
        const usb_audio_state_t fetch_state = usb_audio_get_state();
        if (fetch_state == USB_AUDIO_BUFFERING)
            break;

        int32_t frames = (int32_t)get_size_using(&buffer_audio) / 2;
        if (frames <= 0)
            break;
        if (frames > DMA_MAX_FRAMES)
            frames = DMA_MAX_FRAMES;

        const uint32_t wp = dma_tx.wp;
        uint32_t *tx = dma_tx.data[wp].tx_buf;
        if (ringbuf_read_array_spinlock((int32_t *)tx, (uint32_t)frames * 2u, &buffer_audio) < 0)
            break;

        const uint32_t irq_state = save_and_disable_interrupts();
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

    // A clock/rate change starts a fresh playback epoch. Require the normal
    // buffer threshold and I2S phase resynchronization before output resumes.
    enable_output = false;
    enable_output_prev = false;
    prefill_waiting_for_pio = false;
    hw_set_bits(&dma_hw->ch[dma_ch].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_set_irq0_enabled(dma_ch, true);
    restore_interrupts(irq_state);
}

uint8_t dma_tx_recover_fault(void)
{
    const uint32_t error = atomic_load_explicit(&dma_ahb_fault, memory_order_acquire);
    dma_stop_and_clear();
    atomic_store_explicit(&dma_fault_quiescent, false, memory_order_relaxed);
    atomic_store_explicit(&dma_ahb_fault, 0u, memory_order_release);
    return (uint8_t)(((error & DMA_CH0_CTRL_TRIG_READ_ERROR_BITS) ? 1u : 0u) |
                     ((error & DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS) ? 2u : 0u));
}
