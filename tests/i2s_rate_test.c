#include "i2s_rate.h"

static int close_to(float actual, float expected, float tolerance)
{
    return actual >= expected - tolerance && actual <= expected + tolerance;
}

int main(void)
{
    if (!close_to(i2s_rate_from_clkdiv(336000000u, 3809u << 8), 176424.26f, 0.1f))
        return 1;
    if (!close_to(i2s_rate_from_clkdiv(360000000u, 3750u << 8), 192000.0f, 0.1f))
        return 2;
    if (!close_to(i2s_rate_from_clkdiv(336000000u, 0), 40.05432f, 0.01f))
        return 3;
    return 0;
}
