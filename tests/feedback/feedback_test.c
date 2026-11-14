#include <stdbool.h>
#include <stdint.h>

// Exercise the actual TinyUSB packet formatting and transfer length.
#include "../../tinyusb/src/class/audio/audio_device.c"

static uint8_t sent[4];
static uint16_t sent_len;

bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer,
                    uint16_t total_bytes, bool is_isr)
{
    if (rhport != 0 || ep_addr != 0x81 || is_isr || total_bytes > sizeof(sent))
        return false;
    sent_len = total_bytes;
    for (uint16_t i = 0; i < total_bytes; ++i)
        sent[i] = buffer[i];
    return true;
}

static int check(uint8_t protocol, uint32_t value, uint16_t len,
                 uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    tusb_desc_interface_t descriptor = {0};
    uint32_t buffer = 0;
    descriptor.bInterfaceProtocol = protocol;
    _audiod_fct[0].p_desc = (const uint8_t *)&descriptor;
    _audiod_fct[0].rhport = 0;
    _audiod_fct[0].ep_fb = 0x81;
    _audiod_fct[0].fb_buf = &buffer;
    sent_len = 0;

    if (!tud_audio_n_fb_set(0, value) || !audiod_fb_send(0, false))
        return 1;
    if (sent_len != len || sent[0] != b0 || sent[1] != b1 || sent[2] != b2)
        return 2;
    if (len == 4 && sent[3] != b3)
        return 3;
    return 0;
}

int main(void)
{
    if (check(AUDIO_INT_PROTOCOL_CODE_V1, 0x002c199a, 3, 0x66, 0x06, 0x0b, 0))
        return 1;
    if (check(AUDIO_INT_PROTOCOL_CODE_V1, 0x00300000, 3, 0x00, 0x00, 0x0c, 0))
        return 2;
    if (check(AUDIO_INT_PROTOCOL_CODE_V2, 0x002c199a, 4, 0x9a, 0x19, 0x2c, 0x00))
        return 3;
    if (check(AUDIO_INT_PROTOCOL_CODE_V2, 0x00c00000, 4, 0x00, 0x00, 0xc0, 0x00))
        return 4;
    return 0;
}
