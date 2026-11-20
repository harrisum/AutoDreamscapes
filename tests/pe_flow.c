/* Exercise the shipped executable against a clock-driven, fake Win32 desktop.
   Calls and callbacks use the Microsoft x64 ABI. No real mouse is touched. */
#include "../source/win32.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define WIN __attribute__((ms_abi))
typedef WIN LRESULT (*MockWndProc)(HWND, UINT, WPARAM, LPARAM);
typedef WIN LRESULT (*MockHookProc)(int, WPARAM, LPARAM);

enum { L = 500, T = 80, WIDE = 640, HIGH = 600, SCREEN_W = 1600, SCREEN_H = 900 };
enum { SCAN_PATCH_MAX = 240, PANEL = 100, GAME = 200 };
enum { START, CANCEL, PARTNER_CLEAR, LOSE_FOCUS, ZERO_JITTER, FLAG_ONLY, TAG_ONLY, REOPEN };
enum { HOTKEY, PRESS, FG, MOVE };
typedef struct { int time, type, first, second; } Event;
typedef struct { int x, y, w, h; uint32_t *pixels; } Bitmap;
typedef struct { Bitmap *selected; } DC;
typedef struct { W words[150]; int id; } Widget;

static const Event events[] = {
    {20, HOTKEY, 101, 0}, {44, PRESS, L + 112, T + 539}, {92, PRESS, L + 124, T + 170},
    {130, HOTKEY, 101, 0}, {156, PRESS, L + 420, T + 539}, {210, PRESS, L + 397, T + 319},
    {238, MOVE, 22, 0}, {241, FG, PANEL, 0}, {251, HOTKEY, 102, 0}, {277, FG, GAME, 0}
};
static const uint8_t glyphs[][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}
};
static jmp_buf done;
static MockWndProc window_proc;
static MockHookProc mouse_hook;
static Widget widgets[32];
static int setup_count, shown, hotkeys, exit_code;
static int64_t clock_ms, dispatch_times[8];
static int scenario, event_index, clicks, overlap_recheck, cleared_first, late_event;
static int32_t got_x[8], got_y[8];
static HWND fg;
static int move_x, pending_ms, quit_at;
enum { ACTIVE, RECOVERED, TEMP };
static uint8_t files[3][300000];
static size_t file_size[3], position[3];

static int near(int a, int b, int r) { return a >= b - r && a <= b + r; }

static WIN void exit_process(UINT code)
{
    exit_code = (int)code;
    longjmp(done, 1);
}

static WIN uintptr_t success(void) { return 1; }
static WIN uintptr_t fail_unexpected(void) { exit_process(98); return 0; }
static WIN short key_state(int ignored) { (void)ignored; return 0; }
static WIN UINT device_dpi(void) { return 96; }
static WIN BOOL frequency(int64_t *p) { *p = 1000; return 1; }
static WIN BOOL counter(int64_t *p) { *p = clock_ms; return 1; }

static WIN W register_class(const WNDCLASSEX *cls)
{
    assert(cls->cbSize == 80);
    window_proc = (MockWndProc)cls->proc;
    return 1;
}

static void copy_w(W *to, const W *from, int n)
{
    int i = 0;
    while (i + 1 < n && from[i]) { to[i] = from[i]; ++i; }
    to[i] = 0;
}

static WIN HWND build_window(DWORD ex, const W *wc, const W *name, DWORD style,
                             int x, int y, int w, int h, HWND parent, HMENU id,
                             HINSTANCE instance, void *payload)
{
    (void)ex; (void)wc; (void)style; (void)x; (void)y; (void)w; (void)h;
    (void)parent; (void)instance; (void)payload;
    int index = setup_count++;
    assert(index < 32);
    Widget *widget = widgets + index;
    widget->id = (int)(intptr_t)id;
    copy_w(widget->words, name, 150);
    if (widget->id == 104) copy_w(widget->words, WSTR("56"), 150);
    if (widget->id == 105) copy_w(widget->words, WSTR("66"), 150);
    if (widget->id == 106 && scenario == ZERO_JITTER) copy_w(widget->words, WSTR("0"), 150);
    return (HWND)(intptr_t)(PANEL + index);
}

static WIN BOOL change_text(HWND h, const W *text)
{
    Widget *widget = widgets + (intptr_t)h - PANEL;
    copy_w(widget->words, text, 150);
    return 1;
}

static WIN int read_text(HWND h, W *out, int cap)
{
    Widget *widget = widgets + (intptr_t)h - PANEL;
    copy_w(out, widget->words, cap);
    int n = 0;
    while (out[n]) ++n;
    return n;
}

static WIN int format_w(W *out, const W *format, ...)
{
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, format);
    int count = 0;
    for (int i = 0; format[i]; ++i) {
        if (format[i] != '%') { out[count++] = format[i]; continue; }
        assert(format[++i] == 'u');
        unsigned value = __builtin_va_arg(ap, unsigned);
        W digits[11];
        int n = 0;
        do { digits[n++] = (W)('0' + value % 10); value /= 10; } while (value);
        while (n > 0) out[count++] = digits[--n];
    }
    __builtin_ms_va_end(ap);
    out[count] = 0;
    return count;
}

static WIN BOOL show(void) { ++shown; return 1; }
static WIN BOOL hotkey(void) { ++hotkeys; return 1; }

static WIN HHOOK hook(int id, MockHookProc callback, HINSTANCE dll, DWORD thread)
{
    assert(id == 14 && dll && !thread && callback);
    mouse_hook = callback;
    return (HHOOK)1;
}

static void press(int x, int y, int injected, ULONG_PTR tag)
{
    MOUSEHOOK event = {.pt = {x, y}, .flags = injected, .extra = tag};
    mouse_hook(0, 0x0201, (LPARAM)&event);
}

static WIN BOOL peek(MSG *msg, HWND w, UINT low, UINT high, UINT remove)
{
    (void)w; (void)low; (void)high;
    assert(shown == 1);
    if (late_event && remove) {
        late_event = 0;
        if (scenario == CANCEL) press(L + 11, T + 11, 0, 0);
        if (scenario == LOSE_FOCUS) fg = (HWND)PANEL;
    }
    if (event_index < (int)(sizeof(events) / sizeof(*events)) && clock_ms >= events[event_index].time) {
        if (!remove) return 1;
        Event e = events[event_index++];
        *msg = (MSG){0};
        if (e.type == HOTKEY) {
            msg->message = 0x0312;
            msg->hwnd = (HWND)PANEL;
            msg->wParam = e.first;
            return 1;
        }
        if (e.type == FG) fg = (HWND)(intptr_t)e.first;
        else if (e.type == MOVE) move_x = e.first;
        else press(e.first, e.second, 0, 0);
    }
    if (clock_ms >= quit_at) {
        *msg = (MSG){.message = 0x12};
        return 1;
    }
    return 0;
}

static WIN LRESULT dispatch(const MSG *m)
{
    return window_proc(m->hwnd, m->message, m->wParam, m->lParam);
}

static WIN BOOL timer_set(HANDLE t, const int64_t *due, LONG period, void *cb, void *arg, BOOL resume)
{
    (void)t; (void)period; (void)cb; (void)arg; (void)resume;
    pending_ms = (int)(-*due / 10000);
    assert(pending_ms > 0);
    return 1;
}

static WIN DWORD wait_messages(DWORD n, const HANDLE *handles, DWORD limit, DWORD mask, DWORD flags)
{
    (void)handles; (void)mask; (void)flags;
    int64_t wake = clock_ms + (n ? pending_ms : limit);
    if (event_index < (int)(sizeof(events) / sizeof(*events)) && events[event_index].time < wake)
        wake = events[event_index].time;
    assert(wake > clock_ms);
    clock_ms = wake;
    return 0;
}

static WIN HWND foreground(void) { return fg; }
static WIN HWND ancestor(HWND w, UINT rule) { assert(rule == 2); return w; }
static WIN HWND window_at(POINT p)
{
    return p.x >= L + move_x && p.x < L + move_x + WIDE && p.y >= T && p.y < T + HIGH
         ? (HWND)GAME : (HWND)PANEL;
}
static WIN DWORD process_id(HWND w, DWORD *pid) { assert(w == (HWND)GAME); *pid = 111; return 1; }
static WIN BOOL is_iconic(HWND w) { (void)w; return 0; }
static WIN BOOL get_client(HWND w, RECT *r)
{
    assert(w == (HWND)GAME);
    *r = (RECT){0, 0, WIDE, HIGH};
    return 1;
}
static WIN BOOL to_screen(HWND w, POINT *p)
{
    assert(w == (HWND)GAME);
    p->x += L + move_x;
    p->y += T;
    return 1;
}
static WIN int metric(int key)
{
    switch (key) {
    case 76: case 77: return 0;
    case 78: return SCREEN_W;
    case 79: return SCREEN_H;
    }
    assert(!"unexpected metric");
    return 0;
}
static WIN void *alloc_memory(void *hint, size_t n, DWORD type, DWORD protection)
{
    (void)hint; (void)type; (void)protection;
    return calloc(1, n);
}
static WIN BOOL release_memory(void *p, size_t n, DWORD flags)
{
    (void)n; (void)flags;
    free(p);
    return 1;
}
static WIN HDC make_dc(void) { return calloc(1, sizeof(DC)); }
static WIN BOOL free_dc(HDC h) { free(h); return 1; }

static WIN HBITMAP make_dib(HDC dc, const BITMAPINFO *i, UINT colors, void **out, HANDLE section, DWORD offset)
{
    (void)dc; (void)colors; (void)section; (void)offset;
    assert(i->size == 40 && i->bitCount == 32 && i->height < 0);
    Bitmap *b = calloc(1, sizeof(*b));
    b->w = i->width;
    b->h = -i->height;
    *out = b->pixels = calloc((size_t)b->w * b->h, 4);
    return b;
}

static WIN HGDIOBJ select_bitmap(HDC h, HGDIOBJ obj)
{
    DC *d = h;
    Bitmap *old = d->selected;
    d->selected = obj == (HGDIOBJ)1 ? 0 : obj;
    return old ? old : (HGDIOBJ)1;
}

static WIN BOOL delete_object(HGDIOBJ obj)
{
    if (obj == (HGDIOBJ)1) return 1;
    Bitmap *b = obj;
    free(b->pixels);
    free(b);
    return 1;
}

static uint32_t puzzle_pixel(int x, int y)
{
    int positions[2] = {clock_ms < 238 ? 112 : 235, clock_ms < 238 ? 420 : 507};
    const char *labels[2] = {"ABAC", "EDF"};
    const uint32_t bg = 0xff5665aa, white = 0xfffbf7ff;
    y -= 527;
    if (y < 0 || y >= 21) return bg;
    for (int i = 0; i < 2; ++i) {
        if (i == 0 && cleared_first) continue;
        int len = (int)strlen(labels[i]);
        int relative = x - positions[i] + len * 9;
        if (relative < 0 || relative >= len * 18 || relative % 18 >= 15) continue;
        int letter = labels[i][relative / 18] - 'A';
        if (glyphs[letter][y / 3] & (16 >> (relative % 18 / 3))) return white;
    }
    return bg;
}

static WIN BOOL blit(HDC dst, int x, int y, int w, int h, HDC src, int x0, int y0, DWORD rop)
{
    (void)src;
    assert(!x && !y && rop == SRCCOPY);
    DC *d = dst;
    assert(d->selected && w == d->selected->w && h == d->selected->h);
    if (clock_ms > 270 && w < SCAN_PATCH_MAX && h < 65) {
        ++overlap_recheck;
        if (scenario == PARTNER_CLEAR && overlap_recheck == 1) cleared_first = 1;
    }
    for (int py = 0; py < h; ++py)
        for (int px = 0; px < w; ++px)
            d->selected->pixels[py * w + px] = puzzle_pixel(x0 + px - L - move_x, y0 + py - T);
    return 1;
}

static WIN UINT input(UINT n, const INPUT *events, int cb_size)
{
    assert(n == 3 && cb_size == 40 && fg == (HWND)GAME);
    assert(events[0].mouse.flags == 0xc001 && events[1].mouse.flags == 2 && events[2].mouse.flags == 4);
    assert(events[0].mouse.extra && events[1].mouse.extra == events[0].mouse.extra);
    int x = (int)((int64_t)events[0].mouse.dx * SCREEN_W / 65536);
    int y = (int)((int64_t)events[0].mouse.dy * SCREEN_H / 65536);
    int flags = scenario == TAG_ONLY ? 0 : 1;
    ULONG_PTR extra = scenario == FLAG_ONLY ? 0 : events[1].mouse.extra;
    press(x, y, flags, extra);
    assert(clicks < 8);
    got_x[clicks] = x - L - move_x;
    got_y[clicks] = y - T;
    dispatch_times[clicks++] = clock_ms;
    if (scenario == CANCEL || scenario == LOSE_FOCUS) late_event = 1;
    return n;
}

static int contains_w(const W *word, const W *needle)
{
    for (int i = 0; word[i]; ++i) {
        int j = 0;
        while (needle[j] && word[i + j] == needle[j]) ++j;
        if (!needle[j]) return 1;
    }
    return 0;
}

static WIN DWORD exe_path(HMODULE instance, W *out, DWORD n)
{
    (void)instance;
    const W *path = WSTR("C:\\test\\AutoDS.exe");
    copy_w(out, path, (int)n);
    DWORD size = 0;
    while (out[size]) ++size;
    return size;
}
static WIN DWORD last_error(void) { return 2; }
static WIN DWORD own_process(void) { return 444; }

static WIN HANDLE open_file(const W *path, DWORD access, DWORD share, void *security,
                            DWORD mode, DWORD flags, HANDLE template)
{
    (void)access; (void)share; (void)security; (void)flags; (void)template;
    int id = contains_w(path, WSTR(".tmp")) ? TEMP
           : contains_w(path, WSTR("Recovered")) ? RECOVERED : ACTIVE;
    if (mode == 3 && !file_size[id]) return INVALID_HANDLE;
    if (mode == 2) file_size[id] = 0;
    position[id] = 0;
    return (HANDLE)(intptr_t)(id + 1);
}
static WIN BOOL size_file(HANDLE file, int64_t *size)
{
    *size = (int64_t)file_size[(intptr_t)file - 1];
    return 1;
}
static WIN BOOL read_file(HANDLE file, void *data, DWORD n, DWORD *got, void *overlapped)
{
    (void)overlapped;
    int id = (int)(intptr_t)file - 1;
    assert(position[id] + n <= file_size[id]);
    memcpy(data, files[id] + position[id], n);
    position[id] += n;
    *got = n;
    return 1;
}
static WIN BOOL write_file(HANDLE file, const void *data, DWORD n, DWORD *got, void *overlapped)
{
    (void)overlapped;
    int id = (int)(intptr_t)file - 1;
    assert(id == TEMP && file_size[id] + n <= sizeof(files[id]));
    memcpy(files[id] + file_size[id], data, n);
    file_size[id] += n;
    *got = n;
    return 1;
}
static WIN BOOL replace_file(const W *temp, const W *dest, DWORD flags)
{
    assert(contains_w(temp, WSTR(".tmp")));
    int id = contains_w(dest, WSTR("Recovered")) ? RECOVERED : ACTIVE;
    if (file_size[id] && !(flags & 1)) return 0;
    memcpy(files[id], files[TEMP], file_size[TEMP]);
    file_size[id] = file_size[TEMP];
    file_size[TEMP] = 0;
    return 1;
}
static WIN int popup(HWND parent, const W *text, const W *title, UINT flags)
{
    (void)parent; (void)text; (void)title; (void)flags;
    return 1;
}

uintptr_t resolve_for_test(const char *name)
{
#define API(name_, stub) if (!strcmp(name, #name_)) return (uintptr_t)stub
    API(ExitProcess, exit_process);
    API(GetModuleFileNameW, exe_path);
    API(GetLastError, last_error);
    API(GetCurrentProcessId, own_process);
    API(CreateFileW, open_file);
    API(GetFileSizeEx, size_file);
    API(ReadFile, read_file);
    API(WriteFile, write_file);
    API(MoveFileExW, replace_file);
    API(MessageBoxW, popup);
    API(QueryPerformanceFrequency, frequency);
    API(QueryPerformanceCounter, counter);
    API(RegisterClassExW, register_class);
    API(CreateWindowExW, build_window);
    API(ShowWindow, show);
    API(RegisterHotKey, hotkey);
    API(SetWindowsHookExW, hook);
    API(GetWindowTextW, read_text);
    API(SetWindowTextW, change_text);
    API(wsprintfW, format_w);
    API(PeekMessageW, peek);
    API(DispatchMessageW, dispatch);
    API(SetWaitableTimer, timer_set);
    API(MsgWaitForMultipleObjectsEx, wait_messages);
    API(GetForegroundWindow, foreground);
    API(GetAncestor, ancestor);
    API(WindowFromPoint, window_at);
    API(GetWindowThreadProcessId, process_id);
    API(IsIconic, is_iconic);
    API(IsDialogMessageW, is_iconic);
    API(GetClientRect, get_client);
    API(ClientToScreen, to_screen);
    API(GetDpiForWindow, device_dpi);
    API(GetSystemMetrics, metric);
    API(GetAsyncKeyState, key_state);
    API(VirtualAlloc, alloc_memory);
    API(VirtualFree, release_memory);
    API(CreateCompatibleDC, make_dc);
    API(DeleteDC, free_dc);
    API(CreateDIBSection, make_dib);
    API(SelectObject, select_bitmap);
    API(DeleteObject, delete_object);
    API(BitBlt, blit);
    API(SendInput, input);
#undef API
    const char *ordinary[] = {
        "SetProcessDpiAwarenessContext", "GetModuleHandleW", "LoadIconW", "LoadCursorW",
        "MoveWindow", "SendMessageW", "CreateFontW", "CloseHandle", "CreateWaitableTimerExW",
        "UnhookWindowsHookEx", "UnregisterHotKey", "GetDC", "ReleaseDC", "GdiFlush",
        "IsWindow", "IsWindowVisible", "TranslateMessage", "CallNextHookEx", "FlushFileBuffers", "DeleteFileW"
    };
    for (size_t i = 0; i < sizeof(ordinary) / sizeof(*ordinary); ++i)
        if (!strcmp(name, ordinary[i])) return (uintptr_t)success;
    return (uintptr_t)fail_unexpected;
}

int run_flow(void *entry, int mode)
{
    scenario = mode;
    quit_at = mode == PARTNER_CLEAR ? 1550 : 740;
    clock_ms = 0;
    event_index = mode == REOPEN ? 6 : 0;
    clicks = overlap_recheck = cleared_first = late_event = 0;
    if (mode != REOPEN) {
        file_size[ACTIVE] = 0;
        file_size[RECOVERED] = 0;
    }
    setup_count = shown = hotkeys = move_x = pending_ms = 0;
    exit_code = -1;
    fg = (HWND)GAME;
    if (setjmp(done) == 0) ((WIN void (*)(void))entry)();
    int expected = mode == CANCEL || mode == PARTNER_CLEAR || mode == LOSE_FOCUS ? 1 : 2;
    if (exit_code || shown != 1 || hotkeys != 3 || clicks != expected) {
        fprintf(stderr, "Flow %d stopped at time %lld with status %d, windows %d, clicks %d\n",
                mode, (long long)clock_ms, exit_code, setup_count, clicks);
        return 0;
    }
    int range = mode == ZERO_JITTER ? 0 : 2;
    if (mode == PARTNER_CLEAR) {
        return overlap_recheck >= 2 && near(got_x[0], 397, range) && near(got_y[0], 319, range);
    }
    if (!near(got_x[0], 124, range) || !near(got_y[0], 170, range)) return 0;
    if (expected == 2) {
        int64_t gap = dispatch_times[1] - dispatch_times[0];
        return near(got_x[1], 397, range) && near(got_y[1], 319, range) && gap >= 56 && gap <= 70;
    }
    return 1;
}
