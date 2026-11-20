#include "desktop.h"

static int64_t tick_start, tick_frequency;

void init_clock(void)
{
    QueryPerformanceFrequency(&tick_frequency);
    QueryPerformanceCounter(&tick_start);
}

int64_t now_ms(void)
{
    int64_t now = tick_start;
    QueryPerformanceCounter(&now);
    return (now - tick_start) * 1000 / tick_frequency;
}

HWND foreground_root(void)
{
    HWND fg = GetForegroundWindow();
    return fg ? GetAncestor(fg, 2) : 0;
}

int viewport(HWND window, Rect *out)
{
    RECT client;
    POINT origin = {0};
    if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) ||
        !GetClientRect(window, &client) || !ClientToScreen(window, &origin)) return 0;
    *out = (Rect){origin.x, origin.y, client.right - client.left, client.bottom - client.top};
    return out->w >= 200 && out->h >= 200 && out->w <= 8192 && out->h <= 8192;
}

int target_geometry(const TargetWindow *t, Rect *view)
{
    DWORD pid = 0;
    if (!t->window || foreground_root() != t->window || !viewport(t->window, view) ||
        !GetWindowThreadProcessId(t->window, &pid) || pid != t->process) return 0;
    return view->w == t->view.w && view->h == t->view.h && screen_holds(*view);
}

int over_window(HWND window, Point screen)
{
    HWND at = WindowFromPoint((POINT){screen.x, screen.y});
    return at && GetAncestor(at, 2) == window;
}

static Rect virtual_screen(void)
{
    return (Rect){GetSystemMetrics(76), GetSystemMetrics(77),
                  GetSystemMetrics(78), GetSystemMetrics(79)};
}

int screen_holds(Rect view)
{
    return rect_in(virtual_screen(), view);
}

void release_capture(Grabber *g)
{
    if (g->old)
        SelectObject(g->dc, g->old);
    if (g->bitmap)
        DeleteObject(g->bitmap);
    if (g->dc)
        DeleteDC(g->dc);
    *g = (Grabber){0};
}

static int init_capture(Grabber *g, HDC screen, int w, int h)
{
    if (g->dc && g->w == w && g->h == h)
        return 1;
    release_capture(g);
    BITMAPINFO info = {0};
    info.size = 40;
    info.width = w;
    info.height = -h;
    info.planes = 1;
    info.bitCount = 32;
    g->dc = CreateCompatibleDC(screen);
    if (!g->dc)
        return 0;
    void *pixels = 0;
    g->bitmap = CreateDIBSection(screen, &info, 0, &pixels, 0, 0);
    g->pixels = pixels;
    if (g->bitmap && g->pixels) {
        HGDIOBJ old = SelectObject(g->dc, g->bitmap);
        if (old && old != INVALID_HANDLE) {
            g->old = old;
            g->w = w;
            g->h = h;
            return 1;
        }
    }
    release_capture(g);
    return 0;
}

int capture_rect(Grabber *g, Rect source)
{
    if (!screen_holds(source))
        return 0;
    HDC screen = GetDC(0);
    if (!screen)
        return 0;
    int ok = init_capture(g, screen, source.w, source.h) &&
             BitBlt(g->dc, 0, 0, source.w, source.h, screen, source.x, source.y, SRCCOPY) &&
             GdiFlush();
    ReleaseDC(0, screen);
    return ok;
}


int mouse_down(void)
{
    return ((unsigned short)GetAsyncKeyState(1) | (unsigned short)GetAsyncKeyState(2) |
            (unsigned short)GetAsyncKeyState(4) | (unsigned short)GetAsyncKeyState(5) |
            (unsigned short)GetAsyncKeyState(6)) & 0x8000;
}

int dispatch_click(Point p)
{
    Rect screen = virtual_screen();
    if (!point_in(screen, p))
        return 0;
    int dx = (int)(((int64_t)(p.x - screen.x) * 65536 + 32768) / screen.w);
    int dy = (int)(((int64_t)(p.y - screen.y) * 65536 + 32768) / screen.h);
    const DWORD move = 0x0001, down = 0x0002, up = 0x0004;
    const DWORD absolute = 0x8000, virtualdesk = 0x4000;
    INPUT events[3] = {
        {.mouse = {.dx = dx, .dy = dy, .flags = move | absolute | virtualdesk, .extra = CLICK_TAG}},
        {.mouse = {.flags = down, .extra = CLICK_TAG}},
        {.mouse = {.flags = up, .extra = CLICK_TAG}}
    };
    UINT sent = SendInput(3, events, sizeof(INPUT));
    if (sent == 2)
        SendInput(1, events + 2, sizeof(INPUT));
    return sent == 3;
}
