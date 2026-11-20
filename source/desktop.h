#ifndef AUTODS_DESKTOP_H
#define AUTODS_DESKTOP_H

#include "matcher.h"
#include "win32.h"

#define CLICK_TAG ((ULONG_PTR)UINT64_C(0x4155544f))

typedef struct {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    uint32_t *pixels;
    int w, h;
} Grabber;

typedef struct {
    HWND window;
    DWORD process;
    Rect view;
} TargetWindow;

int64_t now_ms(void);
void init_clock(void);
HWND foreground_root(void);
int viewport(HWND window, Rect *out);
int target_geometry(const TargetWindow *t, Rect *view);
int over_window(HWND window, Point screen);
int screen_holds(Rect view);
int capture_rect(Grabber *g, Rect source);
void release_capture(Grabber *g);
int dispatch_click(Point p);
int mouse_down(void);

#endif
