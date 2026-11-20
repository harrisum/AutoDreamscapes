#include "matcher.h"

static int lo(int a, int b)
{
    return a < b ? a : b;
}

static int hi(int a, int b)
{
    return a > b ? a : b;
}

int point_in(Rect r, Point p)
{
    return p.x >= r.x && p.y >= r.y && p.x - r.x < r.w && p.y - r.y < r.h;
}

int rect_in(Rect outer, Rect inner)
{
    return inner.w > 0 && inner.w <= outer.w && inner.h > 0 && inner.h <= outer.h &&
           point_in(outer, (Point){inner.x, inner.y}) &&
           inner.x - outer.x <= outer.w - inner.w && inner.y - outer.y <= outer.h - inner.h;
}

static int pop(uint64_t x)
{
    x -= (x >> 1) & UINT64_C(0x5555555555555555);
    x = (x & UINT64_C(0x3333333333333333)) + ((x >> 2) & UINT64_C(0x3333333333333333));
    x = (x + (x >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return (int)((x * UINT64_C(0x0101010101010101)) >> 56);
}

static int is_ink(uint32_t color)
{
    unsigned b = color & 255, g = (color >> 8) & 255, r = (color >> 16) & 255;
    return r >= 150 && g >= 157 && b >= 158 &&
           b <= r + 77 && b <= g + 77 && g <= r + 77 &&
           r <= b + 77 && r <= g + 77 && g <= b + 77;
}

static int bit(const uint64_t *row, int x)
{
    return (int)((row[x >> 6] >> (x & 63)) & 1);
}

static int distance(int p, int left, int right)
{
    return p < left ? left - p : p > right ? p - right : 0;
}

/* Select the line and word group nearest the teaching click. If the strip is
   partially clipped, the foreground in the centered crop is still usable. */
static Rect teach_box(const uint32_t *image, int w, int h, Point p)
{
    uint16_t row_count[PATCH_H] = {0}, col_count[PATCH_W] = {0};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            row_count[y] += (unsigned)is_ink(image[y * w + x]);
    }
    int top = 0, bottom = h - 1, best_d = PATCH_H + 1;
    for (int start = 0; start < h;) {
        if (row_count[start] < 2) {
            ++start;
            continue;
        }
        int end = start;
        for (int j = start + 1; j < h && j <= end + 4; ++j)
            if (row_count[j] >= 2)
                end = j;
        int d = distance(p.y, start, end);
        if (end - start >= 5 && d < best_d) {
            top = start;
            bottom = end;
            best_d = d;
        }
        start = end + 1;
    }
    for (int y = top; y <= bottom; ++y)
        for (int x = 0; x < w; ++x)
            col_count[x] += (unsigned)is_ink(image[y * w + x]);

    int left = 0, right = w - 1;
    best_d = PATCH_W + 1;
    int permitted_gap = hi(5, lo(22, (bottom - top + 1) * 3 / 5));
    for (int start = 0; start < w;) {
        if (!col_count[start]) {
            ++start;
            continue;
        }
        int end = start;
        for (int j = start + 1; j < w && j <= end + permitted_gap; ++j)
            if (col_count[j])
                end = j;
        int d = distance(p.x, start, end);
        if (end - start >= 7 && d < best_d) {
            left = start;
            right = end;
            best_d = d;
        }
        start = end + 1;
    }
    left = hi(0, left - 3);
    right = lo(w - 1, right + 3);
    top = hi(0, top - 2);
    bottom = lo(h - 1, bottom + 2);
    return (Rect){left, top, right - left + 1, bottom - top + 1};
}

int make_pattern(const uint32_t *image, int w, int h, Point clicked, Pattern *p)
{
    if (w < 10 || h < 8 || w > PATCH_W || h > PATCH_H || !point_in((Rect){0, 0, w, h}, clicked))
        return 0;
    Rect box = teach_box(image, w, h, clicked);
    p->w = box.w;
    p->h = box.h;
    p->ink = 0;
    p->probe_count = 0;
    for (int y = 0; y < p->h; ++y) {
        for (int i = 0; i < WORDS; ++i)
            p->rows[y][i] = 0;
        for (int x = 0; x < p->w; ++x) {
            if (!is_ink(image[(box.y + y) * w + box.x + x]))
                continue;
            p->rows[y][x >> 6] |= UINT64_C(1) << (x & 63);
            ++p->ink;
        }
    }
    if (p->ink < 22 || p->ink * 100 > p->w * p->h * 75)
        return 0;

    int seen = 0;
    int anchor_rank = p->ink / 3;
    int chosen = 0;
    for (int y = 0; y < p->h; ++y) {
        for (int x = 0; x < p->w; ++x) {
            if (!bit(p->rows[y], x))
                continue;
            if (seen == anchor_rank)
                p->anchor = (Point){x, y};
            if (chosen < PROBES && seen == (chosen * 2 + 1) * p->ink / (2 * PROBES))
                p->probes[chosen++] = (Point){x, y};
            ++seen;
        }
    }
    p->probe_count = chosen;
    return chosen == PROBES;
}

int frame_prepare(Frame *f, const uint32_t *pixels, int w, int h)
{
    if (!pixels || w < 1 || w > 8192 || h < 1 || h > 2048)
        return 0;
    int stride = (w + 63) / 64 + 1;
    if ((size_t)stride * h > f->words || (size_t)w * h > f->capacity)
        return 0;

    f->width = w;
    f->height = h;
    f->stride = stride;
    f->count = 0;
    for (int y = 0; y < h; ++y) {
        uint64_t *row = f->rows + y * stride;
        int x = 0;
        for (int word = 0; word < stride - 1; ++word) {
            int end = lo(x + 64, w), base = x;
            uint64_t bits = 0;
            for (; x < end; ++x) {
                if (!is_ink(pixels[y * w + x]))
                    continue;
                f->anchors[f->count++] = ((uint32_t)y << 16) | (uint32_t)x;
                bits |= UINT64_C(1) << (x - base);
            }
            row[word] = bits;
        }
        row[stride - 1] = 0;
    }
    return 1;
}

static uint64_t shifted(const uint64_t *row, int shift)
{
    return shift ? (row[0] >> shift) | (row[1] << (64 - shift)) : row[0];
}

static int overlap_score(const Frame *f, const Pattern *p, int x, int y)
{
    int words = (p->w + 63) / 64, shift = x & 63, offset = x >> 6;
    uint64_t mask = UINT64_MAX >> ((64 - (p->w & 63)) & 63);
    int common = 0, ink = 0;
    for (int row = 0; row < p->h; ++row) {
        const uint64_t *src = f->rows + (y + row) * f->stride + offset;
        for (int w = 0; w < words; ++w) {
            uint64_t region = shifted(src + w, shift);
            if (w == words - 1)
                region &= mask;
            common += pop(region & p->rows[row][w]);
            ink += pop(region);
        }
    }
    return 2000 * common / (ink + p->ink);
}

static int probes_pass(const Frame *f, const Pattern *p, int x, int y)
{
    int misses = 0;
    for (int j = 1; j < p->probe_count; j += 4) {
        Point q = p->probes[j];
        misses += !bit(f->rows + (y + q.y) * f->stride, x + q.x);
        if (misses > 2)
            return 0;
    }
    for (int j = 0; j < p->probe_count; ++j) {
        if ((j & 3) == 1)
            continue;
        Point q = p->probes[j];
        misses += !bit(f->rows + (y + q.y) * f->stride, x + q.x);
        if (misses > 5)
            return 0;
    }
    return 1;
}

Hit find_pattern(const Frame *f, const Pattern *p)
{
    Hit best = {0};
    if (!p->ink || p->w > f->width || p->h > f->height)
        return best;
    int max_x = f->width - p->w, max_y = f->height - p->h;
    for (int i = 0; i < f->count; ++i) {
        uint32_t anchor = f->anchors[i];
        int y = (int)(anchor >> 16) - p->anchor.y;
        int x = (int)(anchor & 0xffff) - p->anchor.x;
        if (y < 0 || x < 0 || y > max_y || x > max_x || !probes_pass(f, p, x, y))
            continue;
        int score = overlap_score(f, p, x, y);
        if (score <= best.score)
            continue;
        best = (Hit){{x, y, p->w, p->h}, score};
        if (score == 1000)
            break;
    }
    return best;
}

int score_patch(const Pattern *p, const uint32_t *pixels)
{
    int common = 0, ink = 0;
    for (int y = 0; y < p->h; ++y) {
        for (int x = 0; x < p->w; ++x) {
            if (!is_ink(pixels[y * p->w + x]))
                continue;
            ++ink;
            common += bit(p->rows[y], x);
        }
    }
    return 2000 * common / (ink + p->ink);
}

int same_pattern(const Pattern *a, const Pattern *b)
{
    if (a->w != b->w || a->h != b->h || a->ink != b->ink)
        return 0;
    for (int y = 0; y < a->h; ++y)
        for (int w = 0; w < WORDS; ++w)
            if (a->rows[y][w] != b->rows[y][w])
                return 0;
    return 1;
}

int close_pattern(const Pattern *a, const Pattern *b)
{
    if (a->w - b->w > 8 || b->w - a->w > 8 || a->h - b->h > 5 || b->h - a->h > 5)
        return 0;
    int required = (a->ink + b->ink) * 92 / 200;
    for (int dy = -4; dy <= 4; ++dy) {
        int first = hi(0, -dy), last = lo(a->h, b->h - dy);
        for (int dx = -5; dx <= 5; ++dx) {
            int common = 0;
            for (int y = first; y < last; ++y) {
                const uint64_t *row = b->rows[y + dy];
                for (int w = 0; w < WORDS; ++w) {
                    uint64_t aligned = row[w];
                    if (dx > 0)
                        aligned = (aligned << dx) | (w ? row[w - 1] >> (64 - dx) : 0);
                    else if (dx < 0)
                        aligned = (aligned >> -dx) | (w + 1 < WORDS ? row[w + 1] << (64 + dx) : 0);
                    common += pop(aligned & a->rows[y][w]);
                }
            }
            if (common >= required)
                return 1;
        }
    }
    return 0;
}

int same_slot(Hit a, Hit b)
{
    int w = lo(a.box.x + a.box.w, b.box.x + b.box.w) - hi(a.box.x, b.box.x);
    int h = lo(a.box.y + a.box.h, b.box.y + b.box.h) - hi(a.box.y, b.box.y);
    if (w <= 0 || h <= 0)
        return 0;
    int area = lo(a.box.w * a.box.h, b.box.w * b.box.h);
    return w * h * 100 >= area * 56;
}
