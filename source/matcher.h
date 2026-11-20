#ifndef AUTODS_MATCHER_H
#define AUTODS_MATCHER_H

#include <stddef.h>
#include <stdint.h>

#define PATCH_W 224
#define PATCH_H 65
#define WORDS ((PATCH_W + 63) / 64)
#define PROBES 20
#define MAX_LESSONS 128
#define SCORE_MIN 850

typedef struct {
    int x, y;
} Point;
typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    uint64_t rows[PATCH_H][WORDS];
    Point anchor, probes[PROBES];
    int w, h, ink, probe_count;
} Pattern;

typedef struct {
    Pattern pattern;
    Point target;
} Lesson;

typedef struct {
    uint64_t *rows;
    uint32_t *anchors;
    size_t words, capacity;
    int width, height, stride, count;
} Frame;

typedef struct {
    Rect box;
    int score;
} Hit;

int point_in(Rect r, Point p);
int rect_in(Rect outer, Rect inner);
int make_pattern(const uint32_t *image, int w, int h, Point clicked, Pattern *out);
int frame_prepare(Frame *f, const uint32_t *pixels, int w, int h);
Hit find_pattern(const Frame *f, const Pattern *p);
int score_patch(const Pattern *p, const uint32_t *pixels);
int same_pattern(const Pattern *a, const Pattern *b);
int close_pattern(const Pattern *a, const Pattern *b);
int same_slot(Hit a, Hit b);

#endif
