#ifndef AUTODS_TIMING_H
#define AUTODS_TIMING_H
#include "matcher.h"

typedef struct {
    uint32_t random;
    int min_gap, max_gap, jitter;
    int64_t due;
} Timing;

void timing_start(Timing *t, uint32_t seed, int low, int high, int jitter, int64_t now);
void timing_clicked(Timing *t, int64_t when);
Point timing_point(Timing *t, Point p, Rect restrict_to);
#endif
