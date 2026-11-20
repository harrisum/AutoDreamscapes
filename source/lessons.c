#include "lessons.h"

#define PATH_CAP 768
#define FILE_MAGIC UINT64_C(0x3150414d534441)
typedef struct {
    uint64_t magic;
    uint32_t bytes, hash;
} Header;

_Static_assert(sizeof(Lesson) == 2272, "Stable lesson format");
_Static_assert(offsetof(SavedLessons, items) == 32, "Stable lesson header");

static size_t lesson_bytes(const SavedLessons *map)
{
    return offsetof(SavedLessons, items) + sizeof(Lesson) * (size_t)map->count;
}

static uint32_t lesson_fingerprint(const SavedLessons *map)
{
    uint32_t hash = UINT32_C(2166136261);
    const uint8_t *b = (const uint8_t *)map;
    size_t n = lesson_bytes(map);
    for (size_t i = 0; i < n; ++i)
        hash = (hash ^ b[i]) * UINT32_C(16777619);
    return hash;
}

static int has_ink(const Pattern *r, Point p)
{
    return point_in((Rect){0, 0, r->w, r->h}, p) &&
           ((r->rows[p.y][p.x >> 6] >> (p.x & 63)) & 1);
}

static int lessons_valid(const SavedLessons *map)
{
    if (map->count < 0 || map->count > MAX_LESSONS)
        return 0;
    if (!map->count)
        return 1;
    if (!map->window || !map->process || map->width < 200 || map->width > 8192 ||
        map->height < 200 || map->height > 8192 ||
        map->top_label < 0 || map->top_label >= map->height) return 0;
    for (int i = 0; i < map->count; ++i) {
        const Lesson *item = &map->items[i];
        const Pattern *r = &item->pattern;
        if (!point_in((Rect){0, 0, map->width, map->height}, item->target) ||
            r->w < 1 || r->w > PATCH_W || r->h < 1 || r->h > PATCH_H ||
            r->ink < 22 || r->ink > r->w * r->h || r->probe_count != PROBES ||
            !has_ink(r, r->anchor))
            return 0;
        for (int j = 0; j < r->probe_count; ++j)
            if (!has_ink(r, r->probes[j]))
                return 0;
    }
    return 1;
}

static int make_path(W *out, LessonFile file)
{
    DWORD size = GetModuleFileNameW(0, out, PATH_CAP);
    if (!size || size >= PATH_CAP - 46)
        return 0;
    int n = (int)size;
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/')
        --n;
    const W *name = file == LESSONS_RECOVERED ? WSTR("AutoDS-Recovered.dat")
                                              : WSTR("AutoDS-Lessons.dat");
    while (*name)
        out[n++] = *name++;
    out[n] = 0;
    return 1;
}

static int read_exact(HANDLE file, void *buf, DWORD length)
{
    DWORD got = 0;
    return ReadFile(file, buf, length, &got, 0) && got == length;
}

static int write_exact(HANDLE file, const void *buf, DWORD length)
{
    DWORD got = 0;
    return WriteFile(file, buf, length, &got, 0) && got == length;
}

int load_lessons(SavedLessons *map, LessonFile file)
{
    W path[PATH_CAP];
    if (!make_path(path, file))
        return LESSONS_ERROR;
    HANDLE h = CreateFileW(path, 0x80000000u, 7, 0, 3, 0x80, 0);
    if (h == INVALID_HANDLE) {
        DWORD err = GetLastError();
        return err == 2 || err == 3 ? LESSONS_MISSING : LESSONS_ERROR;
    }
    Header header;
    int64_t size = 0;
    int ok = GetFileSizeEx(h, &size) && read_exact(h, &header, sizeof(header)) &&
             header.magic == FILE_MAGIC && header.bytes >= offsetof(SavedLessons, items) &&
             header.bytes <= sizeof(*map) && size == (int64_t)header.bytes + (int64_t)sizeof(header) &&
             read_exact(h, map, header.bytes) &&
             lessons_valid(map) && lesson_bytes(map) == header.bytes &&
             lesson_fingerprint(map) == header.hash;
    CloseHandle(h);
    return ok ? LESSONS_OK : LESSONS_ERROR;
}

int save_lessons(const SavedLessons *map, LessonFile file)
{
    if (!lessons_valid(map))
        return 0;
    W final[PATH_CAP], temp[PATH_CAP];
    if (!make_path(final, file))
        return 0;
    int n = 0;
    for (; final[n]; ++n)
        temp[n] = final[n];
    wsprintfW(temp + n, WSTR(".%u.tmp"), (unsigned)GetCurrentProcessId());
    HANDLE h = CreateFileW(temp, 0x40000000, 0, 0, 2, 0x80, 0);
    if (h == INVALID_HANDLE)
        return 0;
    Header header = {FILE_MAGIC, (uint32_t)lesson_bytes(map), lesson_fingerprint(map)};
    int ok = write_exact(h, &header, sizeof(header)) && write_exact(h, map, header.bytes) &&
             FlushFileBuffers(h);
    if (!CloseHandle(h))
        ok = 0;
    if (ok)
        ok = MoveFileExW(temp, final, file == LESSONS_ACTIVE ? 9 : 8);
    if (!ok)
        DeleteFileW(temp);
    return ok;
}
