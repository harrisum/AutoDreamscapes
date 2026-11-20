#include "timing.h"

static int choose(Timing *t, int low, int high)
{
    uint32_t x = t->random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t->random = x;
    return low + (int)(x % (uint32_t)(high - low + 1));
}

void timing_start(Timing *t, uint32_t seed, int low, int high, int jitter, int64_t now)
{
    *t = (Timing){seed ? seed : UINT32_C(0x71cb53dd), low, high, jitter, now};
}

void timing_clicked(Timing *t, int64_t when)
{
    t->due = when + choose(t, t->min_gap, t->max_gap);
}

Point timing_point(Timing *t, Point p, Rect restrict_to)
{
    Point q = {p.x + choose(t, -t->jitter, t->jitter),
               p.y + choose(t, -t->jitter, t->jitter)};
    return point_in(restrict_to, q) ? q : p;
}
