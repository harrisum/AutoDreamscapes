#ifndef AUTODS_WIN32_H
#define AUTODS_WIN32_H

#include <stddef.h>
#include <stdint.h>

/* Minimal Win64 declarations so the portable build does not need an SDK. */
typedef uint16_t W;
typedef uint32_t DWORD, UINT, COLORREF;
typedef int32_t BOOL, LONG;
typedef uintptr_t WPARAM, ULONG_PTR;
typedef intptr_t LPARAM, LRESULT;
typedef void *HANDLE, *HWND, *HDC, *HGDIOBJ, *HBITMAP, *HFONT, *HBRUSH, *HHOOK;
typedef void *HINSTANCE, *HMODULE, *HICON, *HCURSOR, *HMENU;
typedef struct {
    LONG x, y;
} POINT;
typedef struct {
    LONG left, top, right, bottom;
} RECT;
typedef struct {
    DWORD low, high;
} FILETIME;
typedef struct {
    W year, month, weekday, day, hour, minute, second, ms;
} SYSTEMTIME;
typedef struct {
    HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam;
    DWORD time; POINT pt; DWORD private_;
} MSG;
typedef struct {
    HDC dc;
    BOOL erase;
    RECT rect;
    BOOL restore, update;
    uint8_t reserved[32];
} PAINTSTRUCT;
typedef LRESULT (*WindowProc)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (*HookProc)(int, WPARAM, LPARAM);
typedef struct {
    UINT cbSize, style;
    WindowProc proc;
    int clsExtra, wndExtra;
    HINSTANCE instance;
    HICON icon;
    HCURSOR cursor;
    HBRUSH background;
    const W *menuName, *className;
    HICON iconSmall;
} WNDCLASSEX;
typedef struct {
    POINT pt;
    DWORD mouseData, flags, time;
    ULONG_PTR extra;
} MOUSEHOOK;
typedef struct {
    LONG dx, dy;
    DWORD data, flags, time;
    ULONG_PTR extra;
} MOUSEINPUT;
typedef struct {
    DWORD type;
    MOUSEINPUT mouse;
} INPUT;
typedef struct {
    DWORD size; LONG width, height;
    W planes, bitCount;
    DWORD compression, sizeImage;
    LONG xPelsPerMeter, yPelsPerMeter;
    DWORD clrUsed, clrImportant, colors;
} BITMAPINFO;
typedef struct {
    DWORD attributes;
    FILETIME created, access, written;
    DWORD sizeHigh, sizeLow, reserved0, reserved1;
    W name[260], alternateName[14];
} FINDDATA;

_Static_assert(sizeof(WNDCLASSEX) == 80, "Win64 window class layout");
_Static_assert(sizeof(MSG) == 48, "Win64 message layout");
_Static_assert(sizeof(INPUT) == 40 && offsetof(INPUT, mouse) == 8, "Win64 mouse input layout");
_Static_assert(sizeof(MOUSEHOOK) == 32 && sizeof(FINDDATA) == 592, "Win64 data layout");
_Static_assert(sizeof(BITMAPINFO) == 44 && sizeof(PAINTSTRUCT) == 72, "Win64 GDI layout");

/* The final field on each line identifies the system import library. */
#define API(result, name, args, dll) extern result name args;
API(HMODULE, GetModuleHandleW, (const W *), kernel32)
API(BOOL, QueryPerformanceCounter, (int64_t *), kernel32)
API(BOOL, QueryPerformanceFrequency, (int64_t *), kernel32)
API(void, ExitProcess, (UINT), kernel32)
API(BOOL, CloseHandle, (HANDLE), kernel32)
API(HANDLE, CreateWaitableTimerExW, (void *, const W *, DWORD, DWORD), kernel32)
API(BOOL, SetWaitableTimer, (HANDLE, const int64_t *, LONG, void *, void *, BOOL), kernel32)
API(void *, VirtualAlloc, (void *, size_t, DWORD, DWORD), kernel32)
API(BOOL, VirtualFree, (void *, size_t, DWORD), kernel32)
API(BOOL, SetProcessDpiAwarenessContext, (HANDLE), user32)
API(W, RegisterClassExW, (const WNDCLASSEX *), user32)
API(HWND, CreateWindowExW, (DWORD, const W *, const W *, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, void *), user32)
API(LRESULT, DefWindowProcW, (HWND, UINT, WPARAM, LPARAM), user32)
API(BOOL, DestroyWindow, (HWND), user32)
API(BOOL, ShowWindow, (HWND, int), user32)
API(void, PostQuitMessage, (int), user32)
API(BOOL, PeekMessageW, (MSG *, HWND, UINT, UINT, UINT), user32)
API(BOOL, TranslateMessage, (const MSG *), user32)
API(LRESULT, DispatchMessageW, (const MSG *), user32)
API(DWORD, MsgWaitForMultipleObjectsEx, (DWORD, const HANDLE *, DWORD, DWORD, DWORD), user32)
API(BOOL, IsDialogMessageW, (HWND, MSG *), user32)
API(BOOL, RegisterHotKey, (HWND, int, UINT, UINT), user32)
API(BOOL, UnregisterHotKey, (HWND, int), user32)
API(HHOOK, SetWindowsHookExW, (int, HookProc, HINSTANCE, DWORD), user32)
API(BOOL, UnhookWindowsHookEx, (HHOOK), user32)
API(LRESULT, CallNextHookEx, (HHOOK, int, WPARAM, LPARAM), user32)
API(short, GetAsyncKeyState, (int), user32)
API(HWND, GetForegroundWindow, (void), user32)
API(HWND, GetAncestor, (HWND, UINT), user32)
API(HWND, WindowFromPoint, (POINT), user32)
API(DWORD, GetWindowThreadProcessId, (HWND, DWORD *), user32)
API(BOOL, IsWindow, (HWND), user32)
API(BOOL, IsWindowVisible, (HWND), user32)
API(BOOL, IsIconic, (HWND), user32)
API(int, GetSystemMetrics, (int), user32)
API(BOOL, GetClientRect, (HWND, RECT *), user32)
API(BOOL, ClientToScreen, (HWND, POINT *), user32)
API(UINT, GetDpiForWindow, (HWND), user32)
API(BOOL, MoveWindow, (HWND, int, int, int, int, BOOL), user32)
API(BOOL, SetWindowPos, (HWND, HWND, int, int, int, int, UINT), user32)
API(HCURSOR, LoadCursorW, (HINSTANCE, const W *), user32)
API(HICON, LoadIconW, (HINSTANCE, const W *), user32)
API(BOOL, SetWindowTextW, (HWND, const W *), user32)
API(int, GetWindowTextW, (HWND, W *, int), user32)
API(LRESULT, SendMessageW, (HWND, UINT, WPARAM, LPARAM), user32)
API(int, wsprintfW, (W *, const W *, ...), user32)
API(int, MessageBoxW, (HWND, const W *, const W *, UINT), user32)
API(HDC, GetDC, (HWND), user32)
API(int, ReleaseDC, (HWND, HDC), user32)
API(UINT, SendInput, (UINT, const INPUT *, int), user32)
API(HFONT, CreateFontW, (int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, const W *), gdi32)
API(BOOL, DeleteObject, (HGDIOBJ), gdi32)
API(HGDIOBJ, SelectObject, (HDC, HGDIOBJ), gdi32)
API(HDC, CreateCompatibleDC, (HDC), gdi32)
API(BOOL, DeleteDC, (HDC), gdi32)
API(HBITMAP, CreateDIBSection, (HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD), gdi32)
API(BOOL, GdiFlush, (void), gdi32)
API(BOOL, BitBlt, (HDC, int, int, int, int, HDC, int, int, DWORD), gdi32)
API(DWORD, GetCurrentProcessId, (void), kernel32)
API(DWORD, GetModuleFileNameW, (HMODULE, W *, DWORD), kernel32)
API(DWORD, GetLastError, (void), kernel32)
API(HANDLE, CreateFileW, (const W *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE), kernel32)
API(BOOL, ReadFile, (HANDLE, void *, DWORD, DWORD *, void *), kernel32)
API(BOOL, WriteFile, (HANDLE, const void *, DWORD, DWORD *, void *), kernel32)
API(BOOL, GetFileSizeEx, (HANDLE, int64_t *), kernel32)
API(BOOL, FlushFileBuffers, (HANDLE), kernel32)
API(BOOL, MoveFileExW, (const W *, const W *, DWORD), kernel32)
API(BOOL, DeleteFileW, (const W *), kernel32)
#undef API

#define WSTR(s) ((const W *)u##s)
#define RGB(r, g, b) ((COLORREF)((r) | ((g) << 8) | ((b) << 16)))
#define INVALID_HANDLE ((HANDLE)(intptr_t)-1)
#define SRCCOPY 0x00CC0020u

#endif
