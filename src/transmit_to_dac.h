/*
* Copyright (c) 2026 Toshimiyu 
*
* Released under the MIT license
* https://opensource.org/licenses/mit-license.php
*/

#ifndef _TRANSMIT_TO_DAC_H_
#define _TRANSMIT_TO_DAC_H_

#include "pico/stdlib.h"
#include "common.h"

#define SIZE_DMA_TX_BUF ((96 * 2 * DMA_BLOCK_US) / 1000 + 64)

// Two DMA transfer buffers are sufficient because they are used as a double buffer.
#define SIZE_DMA_TX_BUF_STACK (DEPTH_DMA_TX_BUFFER)

typedef struct
{
    uint32_t *tx_buf;
    uint32_t tx_size;
} DMA_TX_DATA;

typedef struct
{
    DMA_TX_DATA data[DEPTH_DMA_TX_BUFFER];
    volatile uint32_t wp;
    volatile uint32_t rp;
    volatile uint32_t using;
    volatile uint32_t prev_write_length;
} DMA_TX_STRUCTURE;

extern void init_i2s_interface(void);
extern void reset_i2s_freq(void);
extern void __not_in_flash_func(dma_tx_start)(void);
extern void dma_stop_and_clear(void);
extern void pwm_i2s_streaming_rate_change(void);
extern void set_pwm_isr_1(float period_us);
extern DMA_TX_STRUCTURE dma_tx;
extern volatile bool enable_output;

extern uint32_t dma_tx_pool_frames(void); // frames queued in the DMA pool (upper bound, includes the block being played)

#endif /* _TRANSMIT_TO_DAC_H_ */
