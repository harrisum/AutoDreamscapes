#include "desktop.h"
#include "lessons.h"
#include "timing.h"

enum {
    TEACH = 101,
    SOLVE,
    STOP,
    GAP_MIN,
    GAP_MAX,
    JITTER,
    NEW_SCENE
};
enum {
    IDLE,
    PICK_LABEL,
    PICK_OBJECT,
    WAIT_GAME,
    RUNNING
};
enum {
    WM_DESTROY = 2,
    WM_CLOSE = 0x10,
    WM_QUIT = 0x12,
    WM_SETFONT = 0x30,
    WM_COMMAND = 0x111,
    WM_HOTKEY = 0x312,
    WM_DPICHANGED = 0x02e0
};
enum {
    FRAME_FRESH_MS = 40,
    RETRY_MS = 25,
    QUIET_MS = 1000
};

static struct {
    HINSTANCE instance;
    HWND panel, controls[6], status, count, captions[4], new_scene;
    HFONT font;
    HHOOK hook;
    UINT dpi;
    TargetWindow game, teaching;
    Grabber screen, patch;
    Frame frame;
    Lesson lessons[MAX_LESSONS];
    Pattern lesson;
    Hit hits[MAX_LESSONS];
    unsigned char clicked[MAX_LESSONS];
    Rect scanned;
    int mode, taught, label_y, top_label, sent, choice;
    int pending_mouse, cancel, found, dirty;
    Point mouse;
    Timing timing;
    int64_t armed_until, next_scan, saw_at, quiet_at;
} a;
static SavedLessons saved;

static int min_int(int x, int y)
{
    return x < y ? x : y;
}

static int max_int(int x, int y)
{
    return x > y ? x : y;
}

static int px(int x)
{
    return (x * (int)a.dpi + 48) / 96;
}

static void status(const W *message)
{
    SetWindowTextW(a.status, message);
}

static void count_text(void)
{
    W value[80];
    wsprintfW(value, WSTR("%u taught    %u clicked"), (unsigned)a.taught, (unsigned)a.sent);
    SetWindowTextW(a.count, value);
}

static void stop(const W *message)
{
    a.mode = IDLE;
    a.pending_mouse = a.cancel = 0;
    status(message);
    count_text();
}

static HWND root_at(Point p)
{
    HWND w = WindowFromPoint((POINT){p.x, p.y});
    return w ? GetAncestor(w, 2) : 0;
}

static int persist(void)
{
    saved.window = (uint64_t)(uintptr_t)a.game.window;
    saved.process = a.game.process;
    saved.width = a.game.view.w;
    saved.height = a.game.view.h;
    saved.top_label = a.top_label;
    saved.count = a.taught;
    for (int i = 0; i < a.taught; ++i)
        saved.items[i] = a.lessons[i];
    a.dirty = !save_lessons(&saved, LESSONS_ACTIVE);
    return !a.dirty;
}

static int restore(void)
{
    int active = load_lessons(&saved, LESSONS_ACTIVE);
    if (active != LESSONS_OK) {
        int recovered = load_lessons(&saved, LESSONS_RECOVERED);
        if (recovered != LESSONS_OK)
            return active == LESSONS_ERROR || recovered == LESSONS_ERROR ? LESSONS_ERROR : LESSONS_MISSING;
    }
    a.taught = saved.count;
    a.top_label = saved.top_label;
    a.game = (TargetWindow){(HWND)(uintptr_t)saved.window, saved.process, {0, 0, saved.width, saved.height}};
    for (int i = 0; i < a.taught; ++i)
        a.lessons[i] = saved.items[i];
    return LESSONS_OK;
}

static int read_number(int id, int lo, int hi, int standard)
{
    W chars[8];
    HWND edit = a.controls[id - TEACH];
    int n = GetWindowTextW(edit, chars, 8);
    unsigned value = n ? 0 : (unsigned)standard;
    for (int i = 0; i < n; ++i) {
        if (chars[i] < '0' || chars[i] > '9') {
            value = (unsigned)standard;
            break;
        }
        value = value * 10 + chars[i] - '0';
    }
    return max_int(lo, min_int(hi, (int)value));
}

static void set_number(int id, int n)
{
    W text[8];
    wsprintfW(text, WSTR("%u"), (unsigned)n);
    SetWindowTextW(a.controls[id - TEACH], text);
}

static void solve(void)
{
    if (!a.taught) {
        stop(WSTR("Teach an item first."));
        return;
    }
    if (!a.hook) {
        stop(WSTR("Couldn't start the mouse hook. Close and reopen AutoDreamscapes."));
        return;
    }
    HWND foreground = foreground_root();
    DWORD previous_process = 0;
    if (!IsWindow(a.game.window) || !GetWindowThreadProcessId(a.game.window, &previous_process) ||
        previous_process != a.game.process) {
        Rect view;
        DWORD pid = 0;
        if (foreground == a.panel || !viewport(foreground, &view) ||
            view.w != a.game.view.w || view.h != a.game.view.h ||
            !GetWindowThreadProcessId(foreground, &pid)) {
            stop(WSTR("Open the same scene at the original window size, then press Ctrl+Alt+S."));
            return;
        }
        a.game = (TargetWindow){foreground, pid, view};
    }
    if (foreground != a.game.window) {
        a.mode = WAIT_GAME;
        a.armed_until = now_ms() + 12000;
        status(WSTR("Switch back to the game within 12 seconds."));
        return;
    }
    Rect view;
    if (!target_geometry(&a.game, &view)) {
        stop(WSTR("Put the game window back to the size you taught it at."));
        return;
    }
    int low = read_number(GAP_MIN, 10, 2000, 65);
    int high = read_number(GAP_MAX, 10, 2000, 85);
    if (low > high) {
        int swap = low;
        low = high;
        high = swap;
    }
    int jitter = read_number(JITTER, 0, 20, 2);
    set_number(GAP_MIN, low);
    set_number(GAP_MAX, high);
    set_number(JITTER, jitter);

    int64_t now = now_ms(), seed = 0;
    QueryPerformanceCounter(&seed);
    timing_start(&a.timing, (uint32_t)seed ^ (uint32_t)(seed >> 32), low, high, jitter, now);
    for (int i = 0; i < a.taught; ++i)
        a.clicked[i] = 0;
    a.choice = -1;
    a.found = a.cancel = a.sent = a.pending_mouse = 0;
    a.saw_at = -10000;
    a.next_scan = 0;
    a.quiet_at = -1;
    a.mode = RUNNING;
    count_text();
    status(WSTR("Solving. Click the mouse or press Ctrl+Alt+X to stop."));
}

static void command(int id)
{
    if (id == STOP) {
        stop(WSTR("Stopped."));
        return;
    }
    if (id == NEW_SCENE) {
        stop(WSTR("Stopped."));
        if (!a.taught || MessageBoxW(a.panel, WSTR("Start a new scene and clear your current lessons?"),
                                   WSTR("AutoDreamscapes"), 0x124) != 6)
            return;
        int previous = a.taught;
        a.taught = 0;
        if (!persist()) {
            a.taught = previous;
            stop(WSTR("Couldn't start a new scene. Your lessons are still loaded."));
        } else {
            a.game = (TargetWindow){0};
            a.sent = 0;
            stop(WSTR("Press Teach to add the first item."));
        }
        return;
    }
    if (id == TEACH) {
        a.pending_mouse = a.cancel = 0;
        if (!a.hook) {
            stop(WSTR("Couldn't start the mouse hook. Close and reopen AutoDreamscapes."));
            return;
        }
        a.mode = PICK_LABEL;
        status(WSTR("Click the center of the label."));
    } else if (id == SOLVE) {
        solve();
    }
}

static LRESULT mouse_hook(int code, WPARAM message, LPARAM data)
{
    if (code < 0 || (message != 0x0201 && message != 0x0204 &&
                     message != 0x0207 && message != 0x020b))
        return CallNextHookEx(a.hook, code, message, data);
    const MOUSEHOOK *event = (const MOUSEHOOK *)data;
    /* Windows' injected flags and our own marker are independent signals. */
    if ((event->flags & 3) || event->extra == CLICK_TAG)
        return CallNextHookEx(a.hook, code, message, data);

    if (a.mode == RUNNING) {
        a.cancel = 1;
    } else if ((a.mode == PICK_LABEL || a.mode == PICK_OBJECT) &&
               message == 0x0201 && !a.pending_mouse) {
        a.mouse = (Point){event->pt.x, event->pt.y};
        a.pending_mouse = 1;
    }
    return CallNextHookEx(a.hook, code, message, data);
}

static void choose_label(Point p, HWND w)
{
    Rect view;
    DWORD process = 0;
    if (!viewport(w, &view) || !point_in(view, p) || !GetWindowThreadProcessId(w, &process))
        return;
    Rect sub = {
        max_int(view.x, p.x - PATCH_W / 2),
        max_int(view.y, p.y - PATCH_H / 2),
        0, 0
    };
    sub.w = min_int(view.x + view.w, p.x + PATCH_W / 2) - sub.x;
    sub.h = min_int(view.y + view.h, p.y + PATCH_H / 2 + 1) - sub.y;
    if (!capture_rect(&a.patch, sub)) {
        status(WSTR("The label crop could not be captured. Check that the game is on-screen."));
        return;
    }
    if (!make_pattern(a.patch.pixels, sub.w, sub.h, (Point){p.x - sub.x, p.y - sub.y}, &a.lesson)) {
        status(WSTR("Couldn't read the label. Click directly on the visible text."));
        return;
    }
    a.teaching = (TargetWindow){w, process, view};
    a.label_y = p.y - view.y;
    a.mode = PICK_OBJECT;
    status(WSTR("Now click the object."));
}

static void choose_object(Point p, HWND w)
{
    Rect view;
    if (w != a.teaching.window || !viewport(w, &view) ||
        view.w != a.teaching.view.w || view.h != a.teaching.view.h || !point_in(view, p)) {
        status(WSTR("Click the object in the same game window."));
        return;
    }
    int reset = a.game.window != w || a.game.process != a.teaching.process ||
                a.game.view.w != view.w || a.game.view.h != view.h;
    if (reset && a.taught) {
        stop(WSTR("These lessons belong to the other window. Use it again or choose New scene."));
        return;
    }
    Point target = {p.x - view.x, p.y - view.y};
    int key = 0;
    if (!reset) {
        for (; key < a.taught; ++key) {
            const Lesson *previous = a.lessons + key;
            Rect near = {previous->target.x - 14, previous->target.y - 14, 29, 29};
            if (same_pattern(&a.lesson, &previous->pattern) ||
                (point_in(near, target) && close_pattern(&a.lesson, &previous->pattern)))
                break;
        }
        if (key >= MAX_LESSONS) {
            stop(WSTR("Reached the 128-item lesson limit."));
            return;
        }
    } else {
        a.taught = 0;
        a.top_label = a.label_y;
    }
    a.top_label = min_int(a.top_label, a.label_y);
    a.lessons[key] = (Lesson){a.lesson, target};
    a.game = a.teaching;
    if (key == a.taught)
        ++a.taught;
    a.sent = 0;
    stop(persist() ? WSTR("Saved. Teach another item or press Solve.")
                   : WSTR("Couldn't save the lesson. It's still in memory, so keep the app open."));
}

static void take_mouse(void)
{
    if (!a.pending_mouse)
        return;
    a.pending_mouse = 0;
    HWND window = root_at(a.mouse);
    if (!window || window == a.panel)
        return;
    if (a.mode == PICK_LABEL)
        choose_label(a.mouse, window);
    else if (a.mode == PICK_OBJECT) choose_object(a.mouse, window);
}

static int frame_buffer(int width, int height)
{
    size_t words = (size_t)((width + 63) / 64 + 1) * height;
    size_t capacity = (size_t)width * height;
    Frame *f = &a.frame;
    if (height > 2048)
        return 0;
    if (f->words >= words && f->capacity >= capacity)
        return 1;
    if (f->rows)
        VirtualFree(f->rows, 0, 0x8000);
    if (f->anchors)
        VirtualFree(f->anchors, 0, 0x8000);
    *f = (Frame){0};
    f->rows = VirtualAlloc(0, words * sizeof(*f->rows), 0x3000, 4);
    f->anchors = VirtualAlloc(0, capacity * sizeof(*f->anchors), 0x3000, 4);
    if (!f->rows || !f->anchors)
        return 0;
    f->words = words;
    f->capacity = capacity;
    return 1;
}

static Rect label_strip(Rect view)
{
    /* Preserve the bottom 18% and accommodate a higher label row if taught. */
    int top = max_int(0, min_int(view.h * 82 / 100, a.top_label - PATCH_H));
    return (Rect){view.x, view.y + top, view.w, view.h - top};
}

static int find_ready(Rect view)
{
    int64_t began = now_ms();
    Rect region = label_strip(view);
    if (!frame_buffer(region.w, region.h) ||
        !capture_rect(&a.screen, region) ||
        !frame_prepare(&a.frame, a.screen.pixels, region.w, region.h)) {
        stop(WSTR("Couldn't capture the game. Make sure the whole window is visible."));
        return -1;
    }
    a.scanned = region;
    a.saw_at = began;
    /* A bright transition panel can contain orders of magnitude more white
       pixels than the label strip. Wait for the rendered puzzle to return. */
    if (a.frame.count > region.w * region.h / 3)
        return -1;
    for (int i = 0; i < a.taught; ++i) {
        a.hits[i] = find_pattern(&a.frame, &a.lessons[i].pattern);
    }
    int best = -1;
    for (int i = 0; i < a.taught; ++i) {
        Hit hit = a.hits[i];
        if (a.clicked[i] || hit.score < SCORE_MIN)
            continue;
        int uncertain = 0;
        for (int j = 0; j < a.taught; ++j) {
            if (i != j && a.hits[j].score >= SCORE_MIN &&
                a.hits[j].score >= hit.score - 40 && same_slot(hit, a.hits[j])) {
                uncertain = 1;
                break;
            }
        }
        if (uncertain)
            continue;
        if (best < 0 || hit.score > a.hits[best].score ||
            (hit.score == a.hits[best].score && hit.box.x < a.hits[best].box.x))
            best = i;
    }
    return best;
}

static int label_still_visible(Rect view, int key)
{
    Rect box = a.hits[key].box;
    Rect region = label_strip(view);
    box.x += region.x;
    box.y += region.y;
    if (!rect_in(region, box) || !capture_rect(&a.patch, box))
        return 0;
    return score_patch(&a.lessons[key].pattern, a.patch.pixels) >= SCORE_MIN;
}

static void mark_clicked(int item)
{
    Point p = a.lessons[item].target;
    for (int i = 0; i < a.taught; ++i) {
        Point q = a.lessons[i].target;
        if (q.x == p.x && q.y == p.y)
            a.clicked[i] = 1;
    }
}

static int remaining(void)
{
    for (int i = 0; i < a.taught; ++i)
        if (!a.clicked[i])
            return 1;
    return 0;
}

static void pulse(void)
{
    if (a.cancel) {
        stop(WSTR("Stopped by mouse click."));
        return;
    }
    take_mouse();
    int64_t now = now_ms();
    if (a.mode == WAIT_GAME) {
        if (foreground_root() == a.game.window)
            solve();
        else if (now >= a.armed_until) stop(WSTR("Switch back to the game and press Solve again."));
        return;
    }
    if (a.mode != RUNNING)
        return;

    Rect view;
    if (!target_geometry(&a.game, &view)) {
        stop(WSTR("The game window lost focus or changed size."));
        return;
    }
    if (now >= a.next_scan && now + 10 >= a.timing.due &&
        (a.choice < 0 || now - a.saw_at > FRAME_FRESH_MS)) {
        a.choice = find_ready(view);
        if (a.mode != RUNNING)
            return;
        now = now_ms();
        a.next_scan = now + RETRY_MS;
        if (a.choice >= 0) {
            a.found = 1;
        } else if (a.quiet_at < 0) {
            a.quiet_at = now;
        } else if (now - a.quiet_at >= QUIET_MS) {
            stop(a.found ? WSTR("No more taught labels on screen.")
                         : WSTR("No saved labels matched. Try teaching the visible label again."));
            return;
        }
    }
    if (a.choice < 0 || now < a.timing.due || mouse_down() ||
        (GetAsyncKeyState(0x11) & 0x8000) || (GetAsyncKeyState(0x12) & 0x8000))
        return;
    if (a.scanned.x != view.x || a.scanned.y != label_strip(view).y ||
        !label_still_visible(view, a.choice)) {
        a.choice = -1;
        a.next_scan = now + 5;
        if (a.quiet_at < 0)
            a.quiet_at = now;
        else if (now - a.quiet_at >= QUIET_MS)
            stop(WSTR("The labels moved. Press Solve when they settle."));
        return;
    }

    Point p = a.lessons[a.choice].target;
    p.x += view.x;
    p.y += view.y;
    p = timing_point(&a.timing, p, view);
    MSG pending;
    PeekMessageW(&pending, 0, 0, 0, 0);
    if (a.cancel || mouse_down())
        return;
    Rect latest;
    if (!target_geometry(&a.game, &latest) || view.x != latest.x || view.y != latest.y ||
        !over_window(a.game.window, p)) {
        stop(WSTR("The game window moved or the click spot is covered."));
        return;
    }
    int64_t when = now_ms();
    if (!dispatch_click(p)) {
        stop(WSTR("Windows didn't send the click. Run both apps at the same permission level."));
        return;
    }
    mark_clicked(a.choice);
    ++a.sent;
    a.choice = -1;
    a.quiet_at = -1;
    timing_clicked(&a.timing, when);
    count_text();
    if (!remaining())
        stop(WSTR("Done. Clicked each saved spot once."));
}

static HWND control(const W *kind, const W *value, DWORD flags, int id)
{
    return CreateWindowExW(flags & 0x2000 ? 0x200u : 0, kind, value, 0x50000000u | flags,
                           0, 0, 0, 0, a.panel, (HMENU)(intptr_t)id, a.instance, 0);
}

static void place(HWND h, int x, int y, int w, int height)
{
    SendMessageW(h, WM_SETFONT, (WPARAM)a.font, 1);
    MoveWindow(h, px(x), px(y), px(w), px(height), 1);
}

static void layout(void)
{
    HFONT old = a.font;
    a.font = CreateFontW(-px(15), 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0, WSTR("Segoe UI"));
    for (int i = 0; i < 3; ++i)
        place(a.controls[i], 17 + 137 * i, 14, 124, 33);
    place(a.captions[0], 17, 58, 122, 22);
    place(a.controls[3], 140, 53, 53, 27);
    place(a.captions[1], 200, 58, 18, 22);
    place(a.controls[4], 224, 53, 53, 27);
    place(a.captions[2], 296, 58, 68, 22);
    place(a.controls[5], 370, 53, 44, 27);
    place(a.status, 17, 95, 396, 48);
    place(a.count, 17, 146, 396, 22);
    place(a.captions[3], 17, 177, 396, 22);
    place(a.new_scene, 312, 146, 103, 25);
    if (old)
        DeleteObject(old);
}

static LRESULT wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if ((msg == WM_COMMAND && (wp >> 16) == 0) || msg == WM_HOTKEY) {
        command((int)(wp & 0xffff));
        return 0;
    }
    if (msg == WM_DPICHANGED) {
        a.dpi = (UINT)(wp & 0xffff);
        const RECT *r = (const RECT *)lp;
        SetWindowPos(w, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, 0x14);
        layout();
        return 0;
    }
    if (msg == WM_CLOSE) {
        a.mode = IDLE;
        if (a.dirty && !persist() &&
            MessageBoxW(a.panel, WSTR("Couldn't save the latest lessons. Close anyway?"),
                        WSTR("AutoDreamscapes"), 0x124) != 6) return 0;
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static int open_panel(void)
{
    const W *name = WSTR("AutoDS.Simple.2");
    WNDCLASSEX cls = {sizeof(cls), 0, wndproc, 0, 0, a.instance,
                      LoadIconW(0, (const W *)32512), LoadCursorW(0, (const W *)32512),
                      (HBRUSH)16, 0, name, 0};
    if (!RegisterClassExW(&cls))
        return 0;
    a.panel = CreateWindowExW(0, name, WSTR("AutoDreamscapes"), 0x00ca0000, 18, 28, 447, 249,
                              0, 0, a.instance, 0);
    if (!a.panel)
        return 0;
    a.dpi = GetDpiForWindow(a.panel);
    if (a.dpi < 72)
        a.dpi = 96;
    MoveWindow(a.panel, 18, 28, px(447), px(249), 0);

    a.controls[0] = control(WSTR("BUTTON"), WSTR("Teach"), 0x10000, TEACH);
    a.controls[1] = control(WSTR("BUTTON"), WSTR("Solve"), 0x10001, SOLVE);
    a.controls[2] = control(WSTR("BUTTON"), WSTR("Stop"), 0x10000, STOP);
    a.controls[3] = control(WSTR("EDIT"), WSTR("65"), 0x12080, GAP_MIN);
    a.controls[4] = control(WSTR("EDIT"), WSTR("85"), 0x12080, GAP_MAX);
    a.controls[5] = control(WSTR("EDIT"), WSTR("2"), 0x12080, JITTER);
    for (int i = 3; i < 6; ++i)
        SendMessageW(a.controls[i], 0x00c5, 4, 0);
    a.captions[0] = control(WSTR("STATIC"), WSTR("Click gap (ms)"), 0, 0);
    a.captions[1] = control(WSTR("STATIC"), WSTR("to"), 0, 0);
    a.captions[2] = control(WSTR("STATIC"), WSTR("Jitter (px)"), 0, 0);
    a.captions[3] = control(WSTR("STATIC"), WSTR("Ctrl+Alt+T / S / X    Teach / Solve / Stop"), 0, 0);
    a.status = control(WSTR("STATIC"), WSTR(""), 0, 0);
    a.count = control(WSTR("STATIC"), WSTR(""), 0, 0);
    a.new_scene = control(WSTR("BUTTON"), WSTR("New scene"), 0x10000, NEW_SCENE);
    layout();
    return 1;
}

static int pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, 0, 0, 0, 1)) {
        if (msg.message == WM_QUIT)
            return 0;
        if (!IsDialogMessageW(a.panel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 1;
}

void ad_entry(void)
{
    init_clock();
    SetProcessDpiAwarenessContext((HANDLE)(intptr_t)-4);
    a.instance = GetModuleHandleW(0);
    a.dpi = 96;
    if (!open_panel()) {
        MessageBoxW(0, WSTR("Couldn't open AutoDreamscapes."), WSTR("AutoDreamscapes"), 0x10);
        ExitProcess(1);
        return;
    }
    a.hook = SetWindowsHookExW(14, mouse_hook, a.instance, 0);
    RegisterHotKey(a.panel, TEACH, 0x4003, 'T');
    RegisterHotKey(a.panel, SOLVE, 0x4003, 'S');
    RegisterHotKey(a.panel, STOP, 0x4003, 'X');
    int loaded = restore();
    if (!a.hook)
        stop(WSTR("Couldn't start the mouse hook. Close and reopen AutoDreamscapes."));
    else if (a.taught) stop(WSTR("Lessons loaded. Open the same scene and press Solve."));
    else if (loaded == LESSONS_ERROR) stop(WSTR("Couldn't read the lesson file. Keep a copy of your .dat files."));
    else stop(WSTR("Press Teach, then click a label and its object."));
    ShowWindow(a.panel, 1);

    HANDLE timer = CreateWaitableTimerExW(0, 0, 2, 0x1f0003);
    while (pump()) {
        pulse();
        int delay = a.mode == RUNNING ? 3 : (a.mode == IDLE ? 60 : 10);
        int64_t due = -(int64_t)delay * 10000;
        if (timer && SetWaitableTimer(timer, &due, 0, 0, 0, 0))
            MsgWaitForMultipleObjectsEx(1, &timer, (DWORD)delay + 20, 0x04ff, 0x0004);
        else
            MsgWaitForMultipleObjectsEx(0, 0, (DWORD)delay, 0x04ff, 0x0004);
    }
    if (timer)
        CloseHandle(timer);
    if (a.hook)
        UnhookWindowsHookEx(a.hook);
    UnregisterHotKey(a.panel, TEACH);
    UnregisterHotKey(a.panel, SOLVE);
    UnregisterHotKey(a.panel, STOP);
    if (a.font)
        DeleteObject(a.font);
    release_capture(&a.patch);
    release_capture(&a.screen);
    if (a.frame.rows)
        VirtualFree(a.frame.rows, 0, 0x8000);
    if (a.frame.anchors)
        VirtualFree(a.frame.anchors, 0, 0x8000);
    ExitProcess(0);
}
