#include "../source/matcher.h"
#include "../source/timing.h"

#include <assert.h>
#include <stdio.h>

static const unsigned char glyphs[][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
    {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
    {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}
};
enum { FW = 531, FH = 122, ROWS = ((FW + 63) / 64 + 1) * FH };
static uint32_t crop[PATCH_W * PATCH_H], screen[FW * FH], local[PATCH_W * PATCH_H];
static uint32_t anchors[FW * FH];
static uint64_t rows[ROWS];
static Frame f = {.rows = rows, .anchors = anchors, .words = ROWS, .capacity = FW * FH};
static Pattern target, other;
static const uint32_t blue = 0xff6576af, white = 0xfff8fbfd;

static void fill(uint32_t *dst, size_t n)
{
    for (size_t i = 0; i < n; ++i) dst[i] = blue;
}

static void word(uint32_t *dst, int width, const char *s, Point pos, uint32_t ink)
{
    for (int i = 0; s[i]; ++i) {
        const unsigned char *g = glyphs[s[i] - 'A'];
        for (int y = 0; y < 21; ++y) {
            for (int x = 0; x < 15; ++x) {
                if (!(g[y / 3] & (16 >> (x / 3)))) continue;
                dst[(pos.y + y) * width + pos.x + i * 18 + x] = ink;
            }
        }
    }
}

static void test_matcher(void)
{
    fill(crop, PATCH_W * PATCH_H);
    word(crop, PATCH_W, "BEADCAFE", (Point){30, 21}, white);
    assert(make_pattern(crop, PATCH_W, PATCH_H, (Point){80, 32}, &target));
    fill(crop, PATCH_W * PATCH_H);
    word(crop, PATCH_W, "DECAF", (Point){45, 22}, white);
    assert(make_pattern(crop, PATCH_W, PATCH_H, (Point){85, 30}, &other));
    assert(!same_pattern(&target, &other));
    assert(!close_pattern(&target, &other));

    fill(screen, FW * FH);
    word(screen, FW, "BEADCAFE", (Point){66, 70}, 0xffeaf3f7);
    word(screen, FW, "DECAF", (Point){417, 70}, white);
    assert(frame_prepare(&f, screen, FW, FH));
    Hit exact = find_pattern(&f, &target);
    assert(exact.score == 1000 && exact.box.x > 60 && exact.box.x < 66);
    assert(find_pattern(&f, &other).score == 1000);
    assert(!same_slot(exact, find_pattern(&f, &other)));
    assert(same_slot(exact, exact));
    for (int y = 0; y < exact.box.h; ++y)
        for (int x = 0; x < exact.box.w; ++x)
            local[y * exact.box.w + x] = screen[(exact.box.y + y) * FW + exact.box.x + x];
    assert(score_patch(&target, local) == 1000);
    fill(local, exact.box.w * exact.box.h);
    assert(score_patch(&target, local) == 0);

    fill(screen, FW * FH);
    word(screen, FW, "DECAF", (Point){51, 36}, white);
    assert(frame_prepare(&f, screen, FW, FH));
    assert(find_pattern(&f, &target).score < SCORE_MIN);

    fill(crop, PATCH_W * PATCH_H);
    assert(!make_pattern(crop, PATCH_W, PATCH_H, (Point){110, 32}, &other));
    assert(!frame_prepare(&f, screen, FW, FH * 2));
}

static void test_range(void)
{
    Timing timer;
    timing_start(&timer, 90, 61, 72, 3, 200);
    assert(timer.due == 200);
    Rect canvas = {500, 140, 320, 600};
    int delay[12] = {0}, x[7] = {0}, y[7] = {0};
    for (int i = 0; i < 2000; ++i) {
        int64_t when = 200 + i * 110;
        Point p = timing_point(&timer, (Point){590, 160}, canvas);
        assert(p.x >= 587 && p.x <= 593 && p.y >= 157 && p.y <= 163);
        x[p.x - 587]++;
        y[p.y - 157]++;
        timing_clicked(&timer, when);
        assert(timer.due >= when + 61 && timer.due <= when + 72);
        delay[timer.due - when - 61]++;
        assert(point_in(canvas, timing_point(&timer, (Point){500, 140}, canvas)));
    }
    for (int i = 0; i < 7; ++i) assert(x[i] && y[i]);
    for (int i = 0; i < 12; ++i) assert(delay[i]);

    timing_start(&timer, 2, 40, 40, 0, 151);
    assert(timing_point(&timer, (Point){800, 400}, canvas).x == 800);
    assert(timing_point(&timer, (Point){800, 400}, canvas).y == 400);
    timing_clicked(&timer, 160);
    assert(timer.due == 200);
}

int main(void)
{
    test_matcher();
    test_range();
    puts("Matcher, close-in-time verification, random interval, and jitter tests passed");
    return 0;
}
