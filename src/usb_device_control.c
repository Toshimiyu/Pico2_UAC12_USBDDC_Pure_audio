/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

// USB Audio Class 2.0 (TinyUSB uac2_speaker_fb based) front-end of the DDC.
//
//   Host -> UAC2 async OUT EP -> TinyUSB sw FIFO -> usb_audio_task()
//        -> PCM convert (16/24/32bit -> float in int32 full-scale)
//        -> direct PCM-to-int32 conversion -> audio ring buffer -> DMA -> PIO-I2S
//
// Feedback (explicit, 16.16 samples/ms) is computed from the fill level of buffer_audio
// with a small PI controller (XMOS-style), NOT from the TinyUSB FIFO count.

#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "tusb.h"
#include "usb_device_control.h"
#include "usb_audio_class.h"
#include "common_types.h"
#include "common.h"
#include "mute_ramp.h"
#include "ringbuffer.h"
#include "display_rta.h"
#include "transmit_to_dac.h"

extern uint32_t now_playing;

// Referenced from main.c / display_rta.c (activity monitor)
volatile uint32_t audio_last_data_us = 0;
volatile bool audio_data_seen = false;

//--------------------------------------------------------------------+
// Tunables
//--------------------------------------------------------------------+
#define RX_CHUNK_FRAMES (192)
#if (RX_CHUNK_FRAMES > SIZE_EP_BUFFER)
#error RX_CHUNK_FRAMES must not exceed SIZE_EP_BUFFER
#endif

// Ring buffer watermarks (buffer_audio, PCM frames)
#define FB_TARGET_LEVEL (SIZE_BUFFER_FB_THRESHOLD)
#define LEVEL_HIGH_MARK (SIZE_AUDIO_RING_FRAMES - SIZE_AUDIO_RING_FRAMES / 32)   // above -> OVERFLOW_FATAL
#define LEVEL_OVERFLOW_RECOVER (FB_TARGET_LEVEL + SIZE_AUDIO_RING_FRAMES / 8) // OVERFLOW_FATAL -> BUFFERING

// PI controller (units: Hz of requested sample rate per input sample of level error)
#define FB_KP (0.50f)			   // proportional [Hz / sample]
#define FB_KI (0.0625f)			   // integral     [Hz / (sample * s)]  -> zeta ~= 1, wn ~= 0.25 rad/s
#define FB_LEVEL_TAU_S (0.064f)	   // level low-pass time constant (removes block-size sawtooth)
#define FB_UPDATE_PERIOD_US (1000) // one update per USB frame
#define FB_BASE_LIMIT_PPM  (50.0f)
#define FB_BOOST_START_ERR (128.0f)
#define FB_BOOST_FULL_ERR  (512.0f)
#define FB_BOOST_MAX_PPM   (100.0f)

// Sample-rate change: how long to wait for the Ring + DMA pool to play out before the stop is forced.
// Worst case drain = (Ring 2048 + DMA pool 4 x 243 frames) / 44.1 kHz ~= 69 ms.
#define SWITCH_DRAIN_TIMEOUT_US (100000u)

//--------------------------------------------------------------------+
// Module state
//--------------------------------------------------------------------+
static volatile usb_audio_state_t audio_state_machine = USB_AUDIO_IDLE;
static volatile uint8_t cur_alt = 0;
static volatile uint8_t bytes_per_sample = 2;
static volatile bool rate_unsupported = false;
static mute_ramp_t usb_mute_ramp;

typedef struct
{
	float level_f;	 // filtered buffer level
	float integ_hz;	 // integrator [Hz]
	uint32_t last_us;
	uint32_t last_fb;
	bool force_send;
	uint32_t transient_counter_us; // Rate-switch settling timer [us].
} fb_ctrl_t;
static fb_ctrl_t fb;

// Sample-rate switch sequence (Ver1.2), Core0 only
static uint32_t pending_freq = 0;	// rate requested by SET_CUR, applied when the output is quiet
static uint32_t switch_t0_us = 0;	// time the SWITCHING state was entered

static uint8_t rx_raw[RX_CHUNK_FRAMES * 8];
static float ep_Lch[RX_CHUNK_FRAMES];
static float ep_Rch[RX_CHUNK_FRAMES];
static int32_t pcm_stereo[RX_CHUNK_FRAMES * 2u];

//--------------------------------------------------------------------+
// Helpers
//--------------------------------------------------------------------+
bool usb_audio_rate_supported(uint32_t freq)
{
	uint8_t rate_count;
	const uint32_t *rates = usb_audio_class_sample_rates(&rate_count);
	for (uint8_t i = 0; i < rate_count; i++)
		if (rates[i] == freq)
			return true;
	return false;
}

static bool stream_rate_supported(uint8_t alt, uint32_t rate)
{
	usb_audio_format_t format;
	return usb_audio_class_get_format(alt, &format) && rate <= format.max_rate_hz;
}

static void set_state(usb_audio_state_t s)
{
	audio_state_machine = s;
}

static void fb_reset(void)
{
	fb.level_f = (float)audio_ring_total_frames();
	fb.integ_hz = 0.0f;
	fb.last_us = time_us_32();
	fb.force_send = true;
	fb.transient_counter_us = 1000000u;
}

static void service_rx(void);

static void clear_audio_buffers(void)
{
	mute_ramp_reset(&usb_mute_ramp, audio_state.freq, audio_state.mute);
	clear_ringbuffer(&buffer_audio);
}

static void flush_audio_fifo(void)
{
	tud_audio_clear_ep_out_ff();
}

//--------------------------------------------------------------------+
// Sample-rate change (Ver1.2 switch state machine)
//
//   SET_CUR       -> remember pending_freq, state = SWITCHING (nothing else is touched yet)
//   SWITCHING     -> USB data is discarded and feedback is paused. Core1 keeps playing the Ring and the
//                    DMA pool with the OLD clock and stops by itself (enable_output = false) when both
//                    are empty (dma_tx_start()).
//   quiet         -> [Core1 locked out] renew_clock, clear Ring / USB FIFO, fb_reset
//   BUFFERING     -> the existing gate refills to the FB target and restarts PIO (reset_i2s_freq)
//
// Only Core0 takes part: no change is needed in dma_tx_start().
//--------------------------------------------------------------------+
static void switch_begin(uint32_t freq)
{
	pending_freq = freq;
	if (audio_state_machine == USB_AUDIO_SWITCHING)
		return; // already draining: only the target rate changes, keep the start time

	switch_t0_us = time_us_32();
	set_state(USB_AUDIO_SWITCHING);
}

// GET_CUR must already report the new rate while the switch is pending
uint32_t usb_audio_get_current_rate(void)
{
	return (audio_state_machine == USB_AUDIO_SWITCHING) ? pending_freq : audio_state.freq;
}

void usb_audio_apply_sample_rate(uint32_t new_freq)
{
	if (audio_state_machine != USB_AUDIO_SWITCHING && audio_state.freq == new_freq)
		return;

	switch_begin(new_freq);
}

// Called every usb_audio_task() pass, right after tud_task()
static void service_switch(void)
{
	if (audio_state_machine != USB_AUDIO_SWITCHING)
		return;

	const uint32_t waited_us = time_us_32() - switch_t0_us;
	const bool quiet = !enable_output && (dma_tx.using == 0);
	const bool timeout = !quiet && (waited_us >= SWITCH_DRAIN_TIMEOUT_US);
	if (!quiet && !timeout)
		return;

	const bool freq_changed = (pending_freq != audio_state.freq);

	multicore_lockout_start_blocking();

	// Check again with Core1 frozen: it may have (re)started output after the check above.
	if (enable_output || dma_tx.using != 0)
	{
		dma_stop_and_clear();
	}

	if (freq_changed)
	{
		audio_state.freq = pending_freq;
		renew_clock();
	}
	clear_audio_buffers();
	flush_audio_fifo();
	fb_reset();

	multicore_lockout_end_blocking();

	// Full-Speed can not carry 24/32bit above 96kHz : do not pretend to play it
	rate_unsupported = (cur_alt != 0) && !stream_rate_supported(cur_alt, audio_state.freq);

	set_state((cur_alt != 0 && !rate_unsupported) ? USB_AUDIO_BUFFERING : USB_AUDIO_IDLE);
}

//--------------------------------------------------------------------+
// Stream open / close (AS interface alternate setting)
//--------------------------------------------------------------------+
bool usb_audio_stream_open(uint8_t alt)
{
    usb_audio_format_t format;
    if (!usb_audio_class_get_format(alt, &format))
        return false;

    const uint8_t prev_alt = cur_alt;

    cur_alt = alt;
    bytes_per_sample = format.bytes_per_sample;
    audio_state.bit_depth = format.bit_depth;

    if (prev_alt == alt &&
        (audio_state_machine == USB_AUDIO_STREAMING ||
         audio_state_machine == USB_AUDIO_BUFFERING))
    {
        return true;
    }

	// A rate change is in progress: service_switch() finishes it and picks up the alt recorded above.
	if (audio_state_machine == USB_AUDIO_SWITCHING)
		return true;

	// Same-rate (re)open: the clock does not change, so there is nothing to wait for. Start a fresh
	// buffer epoch, but with Core1 frozen: clear_ringbuffer() takes no spinlock, so clearing while
	// Core1 reads the Ring could leave size_using negative.
	// Full-Speed can not carry 24/32bit above 96kHz : do not pretend to play it
	rate_unsupported = !stream_rate_supported(alt, audio_state.freq);

	multicore_lockout_start_blocking();
	flush_audio_fifo();
	clear_audio_buffers();
	fb_reset();
	multicore_lockout_end_blocking();
	set_state(rate_unsupported ? USB_AUDIO_IDLE : USB_AUDIO_BUFFERING);
	return true;
}

void usb_audio_stream_close(void)
{
	cur_alt = 0;
	rate_unsupported = false;
	// A pending rate change keeps running: it will end in IDLE because cur_alt is 0 now.
	// Otherwise Core1 simply plays out what is left in the Ring / DMA pool.
	if (audio_state_machine != USB_AUDIO_SWITCHING)
		set_state(USB_AUDIO_IDLE);
	flush_audio_fifo(); // FIFO tail the host sent just before alt 0
}

//--------------------------------------------------------------------+
// PCM receive: TinyUSB FIFO -> 32-bit I2S samples -> audio ring buffer
//--------------------------------------------------------------------+
static inline int32_t rd_s16(const uint8_t *p)
{
	return (int32_t)(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 24));
}

static inline int32_t rd_s24(const uint8_t *p)
{
	return (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24));
}

static inline int32_t rd_s32(const uint8_t *p)
{
	return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void __not_in_flash_func(pcm_to_float)(const uint8_t *src, uint32_t frames, uint8_t bps, float *left, float *right)
{
	switch (bps)
	{
	case 4:
		for (uint32_t i = 0; i < frames; i++, src += 8)
		{
			left[i] = (float)rd_s32(src);
			right[i] = (float)rd_s32(src + 4);
		}
		break;
	case 3:
		for (uint32_t i = 0; i < frames; i++, src += 6)
		{
			left[i] = (float)rd_s24(src);
			right[i] = (float)rd_s24(src + 3);
		}
		break;
	case 2:
	default:
		for (uint32_t i = 0; i < frames; i++, src += 4)
		{
			left[i] = (float)rd_s16(src);
			right[i] = (float)rd_s16(src + 2);
		}
		break;
	}
}

static void report_peak(const float *left, const float *right, uint32_t frames)
{
	float peak_l = 0.0f, peak_r = 0.0f;
	for (uint32_t i = 0; i < frames; i++)
	{
		float abs_l = fabsf(left[i]);
		float abs_r = fabsf(right[i]);
		if (abs_l > peak_l)
			peak_l = abs_l;
		if (abs_r > peak_r)
			peak_r = abs_r;
	}

	float db_l = 20.0f * log10f(fmaxf(peak_l, 1.0f) / 2147483647.0f);
	float db_r = 20.0f * log10f(fmaxf(peak_r, 1.0f) / 2147483647.0f);
	// -80dBFS .. +3dBFS -> 0..100
	float level_l_f = ((db_l + 80.0f) / 83.0f) * 100.0f;
	float level_r_f = ((db_r + 80.0f) / 83.0f) * 100.0f;
	uint8_t level_l = (uint8_t)fminf(fmaxf(level_l_f, 0.0f), 100.0f);
	uint8_t level_r = (uint8_t)fminf(fmaxf(level_r_f, 0.0f), 100.0f);
	display_rta_set_peak(level_l, level_r);
}

static void discard_invalid_pcm(void)
{
	tud_audio_clear_ep_out_ff();
}

static void service_rx(void)
{
	const uint8_t bps = bytes_per_sample;
	if (bps != 2u && bps != 3u && bps != 4u)
	{
		if (tud_audio_available() != 0)
			discard_invalid_pcm();
		return;
	}

	const uint32_t frame_bytes = 2u * bps;
	const usb_audio_state_t st = audio_state_machine;
	if (st == USB_AUDIO_IDLE || st == USB_AUDIO_SWITCHING)
	{
    	const uint16_t discard = tud_audio_available();

    	if (discard)
    	{
        	tud_audio_clear_ep_out_ff();
    	}

    	return;
	}

	uint16_t avail = tud_audio_available();
	if (avail % frame_bytes != 0)
	{
		discard_invalid_pcm();
		return;
	}

	const uint16_t fifo_depth = CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ;
	if (avail > (fifo_depth / 4u) * 3u)
	{
		const uint16_t retain = (uint16_t)((fifo_depth / 2u / frame_bytes) * frame_bytes);
		uint32_t discard = avail - retain;
		while (discard != 0)
		{
			uint16_t n = (uint16_t)(discard > sizeof(rx_raw) ? sizeof(rx_raw) : discard);
			n = (uint16_t)((n / frame_bytes) * frame_bytes);
			if (n == 0 || tud_audio_read(rx_raw, n) != n)
			{
				discard_invalid_pcm();
				return;
			}
			discard -= n;
		}
		avail = tud_audio_available();
	}

	if (st == USB_AUDIO_OVERFLOW_FATAL)
		return;

	while (avail >= frame_bytes)
	{
		int64_t space = get_size_remain(&buffer_audio) / 2;
		if (space <= 0)
			break;

		uint32_t frames = avail / frame_bytes;
		if (frames > RX_CHUNK_FRAMES)
			frames = RX_CHUNK_FRAMES;
		if ((int64_t)frames > space)
			frames = (uint32_t)space;

		const uint16_t requested_bytes = (uint16_t)(frames * frame_bytes);
		const uint16_t got = tud_audio_read(rx_raw, requested_bytes);
		if (got > requested_bytes)
		{
			discard_invalid_pcm();
			return;
		}
		if (got % frame_bytes != 0)
		{
			discard_invalid_pcm();
			return;
		}
		frames = got / frame_bytes;

			if (frames == 0)
    		break;

		pcm_to_float(rx_raw, frames, bps, ep_Lch, ep_Rch);
		const float scale = audio_state.vol_float * DEFAULT_GAIN_RATIO;
		const bool muted = audio_state.mute;
		const mute_ramp_t ramp_before = usb_mute_ramp;
		const float ramp_unit = 1.0f / (float)usb_mute_ramp.frames;
		for (uint32_t i = 0; i < frames; ++i)
		{
			const uint32_t position = mute_ramp_next(&usb_mute_ramp, muted);
			const float sample_scale = position == usb_mute_ramp.frames ? scale :
				(position == 0u ? 0.0f : scale * ((float)position * ramp_unit));
			ep_Lch[i] *= sample_scale;
			ep_Rch[i] *= sample_scale;
			pcm_stereo[2u * i] = (int32_t)ep_Lch[i];
			pcm_stereo[2u * i + 1u] = (int32_t)ep_Rch[i];
		}

		report_peak(ep_Lch, ep_Rch, frames);
		spectrum_feed_block(ep_Lch, ep_Rch, frames);
		if (ringbuf_write_array_spinlock(pcm_stereo, frames * 2u, &buffer_audio) < 0)
		{
			usb_mute_ramp = ramp_before;
			set_state(USB_AUDIO_OVERFLOW_FATAL);
			break;
		}

		audio_last_data_us = time_us_32();
		audio_data_seen = true;
		now_playing++;
		avail = tud_audio_available();
	}
}

//--------------------------------------------------------------------+
// State machine (IDLE / BUFFERING / STREAMING / OVERFLOW_FATAL / SWITCHING)
//--------------------------------------------------------------------+
static void service_state(void)
{
	const int32_t level = (int32_t)audio_ring_total_frames();
	switch (audio_state_machine)
	{
	case USB_AUDIO_BUFFERING:
	{
		uint32_t prefill_frames =
			(audio_state.freq * TARGET_PREFILL_MS) / 1000u;

		if (prefill_frames < FB_TARGET_LEVEL)
			prefill_frames = FB_TARGET_LEVEL;

		if (prefill_frames >= SIZE_AUDIO_RING_FRAMES)
			prefill_frames = SIZE_AUDIO_RING_FRAMES - 1u;

		if ((uint32_t)level >= prefill_frames)
		{
			set_state(USB_AUDIO_STREAMING);
		}
		break;
	}

	case USB_AUDIO_STREAMING:
		if (level >= LEVEL_HIGH_MARK)
		{
			set_state(USB_AUDIO_OVERFLOW_FATAL);
		}
		break;

	case USB_AUDIO_OVERFLOW_FATAL:
		// DMA / PIO keep draining; retain FIFO data until it approaches capacity.
		if (level <= LEVEL_OVERFLOW_RECOVER)
			set_state(USB_AUDIO_BUFFERING);
		break;

	case USB_AUDIO_SWITCHING: // handled by service_switch()
	case USB_AUDIO_IDLE:
	default:
		break;
	}
}

//--------------------------------------------------------------------+
// Feedback (PI control on buffer_audio fill level)
//--------------------------------------------------------------------+
static void service_feedback(void)
{
const usb_audio_state_t st = audio_state_machine;
if (st == USB_AUDIO_IDLE || st == USB_AUDIO_SWITCHING)
return;

const uint32_t now = time_us_32();
const uint32_t dt_us = now - fb.last_us;
if (!fb.force_send && dt_us < FB_UPDATE_PERIOD_US)
    return;
fb.last_us = now;

float dt = (float)dt_us * 1e-6f;
if (dt > 0.02f)
    dt = 0.02f;
// 【ここから追加】過渡期カウンタの減衰処理とゲインのスケール
    float transient_scale = 1.0f;
    if (fb.transient_counter_us > 0)
    {
        if (fb.transient_counter_us > dt_us)
            fb.transient_counter_us -= dt_us;
        else
            fb.transient_counter_us = 0;

        // 切替直後はゲインを 0.25倍からスタートし、1秒かけて 1.0倍 に滑らかに戻す
        float progress = 1.0f - ((float)fb.transient_counter_us / 1000000.0f);
        transient_scale = 0.25f + 0.75f * progress;
    }

    float effective_ki = FB_KI * transient_scale;
    float effective_kp = FB_KP * transient_scale;
const uint32_t fs = audio_state.freq;
const float level = (float)audio_ring_total_frames();

// Low-pass the level so that block-wise Core0 / DMA access does not leak into the feedback
const float alpha = dt / (FB_LEVEL_TAU_S + dt);
fb.level_f += (level - fb.level_f) * alpha;

float adj_hz = 0.0f;
if (st == USB_AUDIO_BUFFERING)
{
    fb.integ_hz = 0.0f;
	// BUFFERING中はバッファを早く溜めるため、強制的に定常レートより +0.025% 高い要求を送る
    // 例: 192000Hz なら +0.00025f (+0.025%)250 ppm
    adj_hz = (float)fs * 0.00030f;
}
else
{

// error in input-rate samples
const float err = (float)FB_TARGET_LEVEL - fb.level_f;

float dynamic_limit_ppm = FB_BASE_LIMIT_PPM;

if (err > FB_BOOST_START_ERR)
{
    float ratio =
        (err - FB_BOOST_START_ERR) /
        (FB_BOOST_FULL_ERR - FB_BOOST_START_ERR);

    if (ratio > 1.0f)
        ratio = 1.0f;

    dynamic_limit_ppm =
        FB_BASE_LIMIT_PPM +
        (FB_BOOST_MAX_PPM - FB_BASE_LIMIT_PPM) * ratio;
}

float lim =
    (float)fs * dynamic_limit_ppm * 1e-6f;

if (lim > (float)FB_ADJ_LIMIT)
    lim = (float)FB_ADJ_LIMIT;

float p = effective_kp * err;

float integ_next = fb.integ_hz + effective_ki * err * dt; // 【変更】FB_KI から effective_ki に変更

float integ_lim =
    (float)fs * FB_BASE_LIMIT_PPM * 1e-6f;

if (integ_lim > (float)FB_ADJ_LIMIT)
    integ_lim = (float)FB_ADJ_LIMIT;

if (integ_next > integ_lim)
    integ_next = integ_lim;

if (integ_next < -integ_lim)
    integ_next = -integ_lim;

adj_hz = p + integ_next;

if (adj_hz > lim)
{
    adj_hz = lim;
}
else if (adj_hz < -lim)
{
    adj_hz = -lim;
}

fb.integ_hz = integ_next;

}

const float rate_hz = i2s_actual_sample_rate() + adj_hz;

// TinyUSB's feedback API always accepts 16.16 samples per 1 ms frame.
// The driver converts it to 3-byte 10.14 for UAC1; UAC2 remains 4-byte 16.16.
const uint32_t value = (uint32_t)((double)rate_hz * 65.536 + 0.5);
if (fb.force_send || value != fb.last_fb)
{
    tud_audio_fb_set(value);
    fb.last_fb = value;
    fb.force_send = false;
}
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+
void usb_sound_card_init(void)
{
	set_state(USB_AUDIO_IDLE);

	audio_state.acq_volume = DEFAULT_VOLUME;
	volume_control();
	mute_ramp_reset(&usb_mute_ramp, audio_state.freq, audio_state.mute);

	tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
	tusb_init(BOARD_TUD_RHPORT, &dev_init);
}

void usb_audio_task(void)
{
    tud_task();
    service_switch();
    service_rx();
    service_state();
    service_feedback();
}

usb_audio_state_t usb_audio_get_state(void)
{
	return audio_state_machine;
}