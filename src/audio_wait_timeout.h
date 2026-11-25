#ifndef AUDIO_WAIT_TIMEOUT_H_
#define AUDIO_WAIT_TIMEOUT_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t since_us;
    bool active;
    bool reported;
} audio_wait_timeout_t;

static inline bool audio_wait_timeout_update(audio_wait_timeout_t *timer, bool waiting,
                                             uint32_t now_us, uint32_t timeout_us)
{
    if (!waiting)
    {
        timer->active = false;
        timer->reported = false;
        return false;
    }
    if (!timer->active)
    {
        timer->active = true;
        timer->since_us = now_us;
    }
    if (!timer->reported && (uint32_t)(now_us - timer->since_us) >= timeout_us)
    {
        timer->reported = true;
        return true;
    }
    return false;
}

#endif
