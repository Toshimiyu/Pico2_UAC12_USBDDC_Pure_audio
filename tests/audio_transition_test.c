#include <limits.h>
#include "audio_fade.h"
#include "pcm_silence_fade.h"

static int test_pcm_silence_fade(void)
{
    pcm_silence_fade_t fade;
    pcm_silence_fade_reset(&fade, 48000u);
    for (uint32_t i = 0u; i < fade.silence_threshold_frames; i++)
    {
        if (pcm_silence_fade_next_gain(&fade, true, false) != 65536u)
            return 5;
    }

    const uint32_t first_gain = pcm_silence_fade_next_gain(&fade, false, false);
    if (first_gain == 0u || first_gain >= 65536u ||
        fade.fade_remaining != fade.fade_frames - 1u)
        return 6;
    for (uint32_t i = 1u; i < fade.fade_frames; i++)
    {
        const uint32_t expected =
            (uint32_t)(((uint64_t)(i + 1u) << 16) / fade.fade_frames);
        if (pcm_silence_fade_next_gain(&fade, false, false) != expected)
            return 7;
    }
    if (pcm_silence_fade_next_gain(&fade, false, false) != 65536u)
        return 8;

    pcm_silence_fade_reset(&fade, 48000u);
    for (uint32_t i = 0u; i < fade.silence_threshold_frames; i++)
        (void)pcm_silence_fade_next_gain(&fade, true, false);
    if (pcm_silence_fade_next_gain(&fade, false, true) != 65536u ||
        pcm_silence_fade_next_gain(&fade, false, false) != 65536u)
        return 9;
    return 0;
}

int main(void)
{
    const int silence_fade_result = test_pcm_silence_fade();
    if (silence_fade_result != 0)
        return silence_fade_result;

    if (audio_fade_gain_q16(1u, 4u) != 16384u ||
        audio_fade_gain_q16(3u, 4u) != 49152u ||
        audio_fade_gain_q16(960u, 960u) != 65536u ||
        audio_fade_gain_q16(1u, 960u) != 68u)
        return 1;
    if (audio_fade_apply_q16(INT32_MAX, 65536u) != INT32_MAX ||
        audio_fade_apply_q16(INT32_MIN, 65536u) != INT32_MIN ||
        audio_fade_apply_q16(INT32_MIN, 32768u) != INT32_MIN / 2 ||
        audio_fade_apply_q16(INT32_MAX, 0u) != 0 ||
        audio_fade_apply_q16(-1, 32768u) != -1)
        return 2;

    return 0;
}
