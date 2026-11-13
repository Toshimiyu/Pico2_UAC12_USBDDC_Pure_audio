/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#ifndef _USB_DEVICE_CONTROL_H_
#define _USB_DEVICE_CONTROL_H_

#include <stdint.h>
#include <stdbool.h>
#include "usb_audio_config.h"

// Streaming state (XMOS-style buffering control)
typedef enum
{
	USB_AUDIO_IDLE = 0,		   // no stream (AS interface alt0 / not configured)
	USB_AUDIO_BUFFERING,	   // stream open, nominal feedback only until the prefill level is reached
	USB_AUDIO_STREAMING,	   // normal operation, PI feedback active
	USB_AUDIO_OVERFLOW_FATAL,  // ring buffer too full : new USB data is discarded, playback continues
	USB_AUDIO_UNDERFLOW_FATAL, // Ring + DMA pool too empty : accumulate again
	USB_AUDIO_SWITCHING,	   // sample-rate change: USB data discarded, Ring + DMA pool are played out, then clock switch -> BUFFERING
} usb_audio_state_t;

// ---- Application API (main.c) -----------------------------------------------
// Initialise TinyUSB and publish the UAC2 device (name kept for main.c compatibility)
extern void usb_sound_card_init(void);

// Must be called from the Core0 main loop as often as possible:
// tud_task + PCM receive/convert -> audio ring buffer + feedback control + state machine
extern void usb_audio_task(void);

extern usb_audio_state_t usb_audio_get_state(void);

// ---- USB Audio class adapter API ---------------------------------------------
extern bool usb_audio_rate_supported(uint32_t freq);
extern void usb_audio_apply_sample_rate(uint32_t freq); // reconfigure I2S and clear queued PCM
extern bool usb_audio_stream_open(uint8_t alt);			 // AS interface alternate setting
extern void usb_audio_stream_close(void);				 // AS interface alt 0 / unmount

// ----- Switch sequence (Ver1.2) -----
// Rate to report for GET_CUR: the pending rate while SWITCHING, otherwise the rate in use
uint32_t usb_audio_get_current_rate(void);

#endif /* _USB_DEVICE_CONTROL_H_ */