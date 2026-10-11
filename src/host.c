/*
 * host.c - the modern machine's side: a Win32 window (GDI, nearest-neighbour,
 * 4:3), waveOut audio, keyboard as raw PC scancodes -- or, with --headless,
 * none of that: frames are composed offscreen and, with --record, piped to
 * ffmpeg with the audio muxed in at the end. Headless never touches the
 * desktop, so it works over RDP and from CI (repo rules, section 13).
 *
 * Scripted input (--keys "ms:KEY,...") presses and releases keys at fixed
 * points in emulated time, which is how the harness walks the menus.
 */
#include "machine.h"
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#define FRAME_MAX (3840 * 1200)                 /* 640x200 (21:9 and wider) at --hires 6 */
static uint32_t frame[FRAME_MAX];
static uint32_t big[FRAME_MAX];
static int fw = 320, fh = 200;
static int scan_rows = 200;                     /* the CRT's rows behind the frame */
static int hires_on;
int hires_compose(uint32_t *out, int S, int *w, int *h);
void hires_set_wide(double aspect);
int hires_wide_cols(void);
int hires_set_dlss(int m);
int hires_dlss_mode(void);
int hires_dlss_parse(const char *s);
const char *hires_dlss_name(int m);
void dlss_shutdown(void);
int hires_art_available(void);
int hires_art_mode(void);
void hires_set_art(int on);
static double wide_aspect, frame_aspect = 4.0 / 3;   /* the chosen widescreen; this frame's shape */
static int wide_on;

/* What is on screen now: the hi-res renderer's frame when it is on and the
 * game is in its 3D mode, the plain VGA picture otherwise. */
static void compose(void)
{
    if (hires_on && g_opt.hires > 0 && hires_compose(frame, g_opt.hires, &fw, &fh)) {
        scan_rows = 200;
        frame_aspect = 4.0 / 3 * fw / (320.0 * g_opt.hires);   /* 320 columns show at 4:3 */
        return;
    }
    vga_compose(frame, &fw, &fh);
    scan_rows = fh;
    frame_aspect = 4.0 / 3;
}

/* ---- recording ------------------------------------------------------------ */

#define REC_FPS 35
static FILE *rec_video, *rec_audio;
static uint64_t rec_frames, rec_samples;
static char rec_vpath[512], rec_apath[512];

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4, riff = 36 + data, rate = AUDIO_RATE, br = AUDIO_RATE * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static int rec_w = 640, rec_h = 400;

static void rec_open(void)
{
    if (g_opt.hires > 0) { rec_w = (320 + 2 * hires_wide_cols()) * g_opt.hires; rec_h = 200 * g_opt.hires; }
    snprintf(rec_vpath, sizeof rec_vpath, "%s.video.mp4", g_opt.record);
    snprintf(rec_apath, sizeof rec_apath, "%s.audio.wav", g_opt.record);
    char cmd[1200];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -y -loglevel error -f rawvideo -pixel_format bgra -video_size %dx%d "
             "-framerate %d -i - -vf scale=%d:%d:flags=neighbor -c:v libx264 -pix_fmt yuv420p "
             "-crf 18 \"%s\"", rec_w, rec_h, REC_FPS, rec_w < 1280 ? 1280 : rec_w,
             (rec_w < 1280 ? 1280 : rec_w) * 3 / 4, rec_vpath);
#ifdef _WIN32
    rec_video = _popen(cmd, "wb");
#endif
    if (!rec_video) fatal("cannot start ffmpeg (is it on PATH?)");
    rec_audio = fopen(rec_apath, "wb");
    if (rec_audio) wav_header(rec_audio, 0);
}

static void rec_close(void)
{
    if (!rec_video) return;
#ifdef _WIN32
    _pclose(rec_video);
#endif
    rec_video = NULL;
    if (rec_audio) { wav_header(rec_audio, (uint32_t)rec_samples); fclose(rec_audio); rec_audio = NULL; }
    char cmd[1600];
    snprintf(cmd, sizeof cmd, "ffmpeg -y -loglevel error -i \"%s\" -i \"%s\" -c:v copy -c:a aac -b:a 192k -shortest \"%s\"",
             rec_vpath, rec_apath, g_opt.record);
    if (system(cmd) == 0) { remove(rec_vpath); remove(rec_apath); }
    fprintf(stderr, "[rec] %s: %llu frames\n", g_opt.record, (unsigned long long)rec_frames);
}

/* Every frame to the encoder at one size, whatever mode the game is in */
static const uint32_t *as_rec(void)
{
    if (fw == rec_w && fh == rec_h) return frame;
    for (int y = 0; y < rec_h; y++)
        for (int x = 0; x < rec_w; x++)
            big[y * rec_w + x] = frame[(y * fh / rec_h) * fw + x * fw / rec_w];
    return big;
}

static FILE *wav_out;
static uint64_t wav_samples;

void host_snapshot(const char *path);

/* --shot-at "ms:file,...": frames at fixed points in emulated time, for the
 * conformance harness (tools/conformance.py) */
static void shots_due(uint64_t now)
{
    static const char *p;
    static int init;
    if (!init) { init = 1; p = g_opt.shots; }
    while (p && *p) {
        double ms; char path[260]; int n = 0;
        if (sscanf(p, "%lf:%259[^,]%n", &ms, path, &n) < 2) { p = NULL; return; }
        if (now < (uint64_t)(ms * PIT_HZ / 1000.0)) return;
        host_snapshot(path);
        p += n; if (*p == ',') p++;
    }
}

/* ---- mouse aim -----------------------------------------------------------------
 *
 * The game turns with the mouse's horizontal motion and walks with its
 * vertical motion (when the CONFIG has the mouse enabled; Steam's does). Mouse
 * aim, on unless --mouse-aim off (F7 toggles): horizontal turns, scaled by
 * --mouse-sens with the fractions carried so slow movement still turns;
 * vertical is dropped, so walking stays on the keys. In a window the mouse is
 * captured whenever the window has focus. */
static int aim_on = 1;
static double aim_sens = 1.0, aim_frac;

static void aim_motion(int dx, int dy, int buttons)
{
    if (!aim_on) { mouse_host(dx, dy, buttons); return; }
    double v = dx * aim_sens + aim_frac;
    int i = (int)v;
    aim_frac = v - i;
    mouse_host(i, 0, buttons);
}

/* Scripted mouse sweeps for tests: "ms:MX<dx>+<over ms>" in --keys, and
 * "ms:MY<dy>+<over ms>" (which mouse aim drops). */
typedef struct { uint64_t at, dur; int dx, dy, sent_x, sent_y; } MouseEv;
static MouseEv mev[64];
static int nmev;

static void mouse_due(void)
{
    uint64_t now = pit_now();
    for (int i = 0; i < nmev; i++) {
        MouseEv *m = &mev[i];
        if (now < m->at || (m->sent_x == m->dx && m->sent_y == m->dy)) continue;
        double f = m->dur ? (double)(now - m->at) / m->dur : 1;
        if (f > 1) f = 1;
        int tx = (int)lround(m->dx * f), ty = (int)lround(m->dy * f);
        if (tx != m->sent_x || ty != m->sent_y) {
            aim_motion(tx - m->sent_x, ty - m->sent_y, 0);
            m->sent_x = tx; m->sent_y = ty;
        }
    }
}

/* ---- scripted keys ----------------------------------------------------------- */

typedef struct { uint64_t at; uint8_t sc; uint8_t up; } KeyEv;
static KeyEv kev[512];
static int nkev, kev_i;

static int key_by_name(const char *s)
{
    static const struct { const char *n; int sc; } t[] = {
        {"ESC",1},{"ENTER",0x1C},{"SPACE",0x39},{"UP",0x48|0x100},{"DOWN",0x50|0x100},
        {"LEFT",0x4B|0x100},{"RIGHT",0x4D|0x100},{"CTRL",0x1D},{"ALT",0x38},{"SHIFT",0x2A},
        {"TAB",0x0F},{"BKSP",0x0E},{"Y",0x15},{"N",0x31},{"F1",0x3B},{"F2",0x3C},{"F3",0x3D},
        {"F10",0x44},{"W",0x11},{"A",0x1E},{"S",0x1F},{"D",0x20},{"1",2},{"2",3},{"3",4},{"4",5},{"5",6},{"6",7},{NULL,0}};
    for (int i = 0; t[i].n; i++) if (!_stricmp(s, t[i].n)) return t[i].sc;
    return (int)strtol(s, NULL, 16);
}

static void keys_parse(void)
{
    const char *p = g_opt.keys;
    while (p && *p && nkev < 500) {
        char name[32]; double ms; int hold = 120, n = 0;
        if (sscanf(p, "%lf:%31[^,]%n", &ms, name, &n) < 2) break;
        char *h = strchr(name, '+');            /* KEY+holdms */
        if (h) { *h = 0; hold = atoi(h + 1); }
        uint64_t at = (uint64_t)(ms * PIT_HZ / 1000.0);
        if ((name[0] == 'M' || name[0] == 'm') && (toupper(name[1]) == 'X' || toupper(name[1]) == 'Y') && nmev < 64) {
            int v = atoi(name + 2);                 /* MX<dx>+<ms>, MY<dy>+<ms> */
            mev[nmev++] = (MouseEv){ at, (uint64_t)((h ? hold : 0) * PIT_HZ / 1000.0),
                                     toupper(name[1]) == 'X' ? v : 0, toupper(name[1]) == 'Y' ? v : 0, 0, 0 };
            p += n; if (*p == ',') p++;
            continue;
        }
        int sc = key_by_name(name);
        kev[nkev++] = (KeyEv){ at, (uint8_t)sc, 0 };
        kev[nkev++] = (KeyEv){ at + (uint64_t)(hold * PIT_HZ / 1000.0), (uint8_t)sc, 1 };
        p += n; if (*p == ',') p++;
    }
    /* sort by time (insertion; small) */
    for (int i = 1; i < nkev; i++)
        for (int j = i; j > 0 && kev[j].at < kev[j - 1].at; j--) { KeyEv t = kev[j]; kev[j] = kev[j - 1]; kev[j - 1] = t; }
}

static int ext_of(int sc) { return sc == 0x48 || sc == 0x50 || sc == 0x4B || sc == 0x4D; }

static void keys_due(void)
{
    while (kev_i < nkev && kev[kev_i].at <= pit_now()) {
        int sc = kev[kev_i].sc;
        if (ext_of(sc)) kbd_scancode(0xE0);
        kbd_scancode((uint8_t)(sc | (kev[kev_i].up ? 0x80 : 0)));
        kev_i++;
    }
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- autopilot (--route) ------------------------------------------------------
 *
 * Key timings are fine for menus but drift on a walk across a map, so a test
 * run can steer instead: from a start time it walks the player to waypoints,
 * reading where the player is each frame and pressing the keys a player would.
 * Headless time is deterministic, so a route lands the same way every run.
 *
 *   --route "47000;go 37.5,48;fire 3000;go 37.5,42.5;face 180;use;wait 1000"
 *
 *   go X,Y    turn toward the point (tiles) and walk until within a quarter tile
 *   face A    turn to angle A (degrees: 0 east, 90 north)
 *   use       press SPACE          fire MS   hold CTRL for MS
 *   key K     press key K          wait MS   do nothing for MS
 *
 * Each finished step is logged as "[route] ..."; a step that makes no progress
 * for 4 s is logged as stuck and skipped. */
int hires_player(double *x, double *y, int *angle);

typedef struct { char op; double a, b; int k; } RouteStep;
static RouteStep route[128];
static int nroute, route_i;
static uint64_t route_at, step_at;
static double step_best;
static uint64_t step_best_at;
static uint8_t route_down[0x200];

static void route_key(int sc, int down)
{
    if (route_down[sc & 0x1FF] == down) return;
    route_down[sc & 0x1FF] = (uint8_t)down;
    if (ext_of(sc)) kbd_scancode(0xE0);
    kbd_scancode((uint8_t)(sc | (down ? 0 : 0x80)));
}

static void route_release(void)
{
    for (int sc = 0; sc < 0x200; sc++) if (route_down[sc]) route_key(sc, 0);
}

static void route_parse(void)
{
    const char *p = g_opt.route;
    if (!p) return;
    route_at = (uint64_t)(atof(p) * PIT_HZ / 1000.0);
    while ((p = strchr(p, ';')) != NULL && nroute < 128) {
        p++;
        char op[16] = {0}, arg[32] = {0};
        RouteStep s = {0};
        sscanf(p, "%15[^ ;] %31[^;]", op, arg);
        if (!strcmp(op, "go"))        { s.op = 'g'; sscanf(arg, "%lf,%lf", &s.a, &s.b); }
        else if (!strcmp(op, "face")) { s.op = 'f'; s.a = atof(arg); }
        else if (!strcmp(op, "use"))  { s.op = 'k'; s.k = key_by_name("SPACE"); s.a = 150; }
        else if (!strcmp(op, "fire")) { s.op = 'k'; s.k = key_by_name("CTRL"); s.a = atof(arg); }
        else if (!strcmp(op, "key"))  { s.op = 'k'; s.k = key_by_name(arg); s.a = 150; }
        else if (!strcmp(op, "wait")) { s.op = 'w'; s.a = atof(arg); }
        else { fprintf(stderr, "[route] unknown step '%s'\n", op); continue; }
        route[nroute++] = s;
    }
}

static void route_next(const char *how, double x, double y, int ang)
{
    route_release();
    fprintf(stderr, "[route] step %d %s at %.2f,%.2f angle %d, t=%llu\n", route_i + 1, how, x, y, ang,
            (unsigned long long)(pit_now() * 1000 / PIT_HZ));
    route_i++;
    step_at = pit_now();
    step_best = 1e9;
    step_best_at = step_at;
    if (route_i == nroute) fprintf(stderr, "[route] done\n");
}

static void route_due(void)
{
    uint64_t now = pit_now();
    if (route_i >= nroute || now < route_at) return;
    double x, y; int ang;
    if (!hires_player(&x, &y, &ang)) return;
    if (!step_at) { step_at = now; step_best = 1e9; step_best_at = now; }
    const RouteStep *s = &route[route_i];
    double ms = (double)(now - step_at) * 1000 / PIT_HZ;
    int left = key_by_name("LEFT"), right = key_by_name("RIGHT"), up = key_by_name("UP");
    if (s->op == 'w') { if (ms >= s->a) route_next("waited", x, y, ang); return; }
    if (s->op == 'k') {
        route_key(s->k, ms < s->a);
        if (ms >= s->a + 100) route_next("pressed", x, y, ang);   /* and let go */
        return;
    }
    /* turning toward a heading: y grows southward, angles counterclockwise */
    double want = s->op == 'f' ? s->a : atan2(y - s->b, s->a - x) * 180 / M_PI;
    double err = fmod(want - ang + 540.0, 360.0) - 180.0;
    double dist = s->op == 'g' ? hypot(s->a - x, s->b - y) : fabs(err);
    double thr = s->op == 'f' ? 2 : 4;                 /* walking tolerates a little wander */
    if (s->op == 'g' ? dist < 0.25 : fabs(err) <= thr) { route_next(s->op == 'g' ? "reached" : "faced", x, y, ang); return; }
    if (dist < step_best - 0.05) { step_best = dist; step_best_at = now; }
    double stalled = (double)(now - step_best_at) * 1000 / PIT_HZ;
    if (stalled > 4000) { route_next("STUCK", x, y, ang); return; }
    /* a walk that stalls is usually at a closed door: try it, as a player would */
    route_key(key_by_name("SPACE"), s->op == 'g' && stalled > 800 && stalled < 950);
    route_key(left, err > thr);
    route_key(right, err < -thr);
    route_key(up, s->op == 'g' && fabs(err) < 30);
}

/* ---- window ---------------------------------------------------------------- */

#ifdef _WIN32
static HWND wnd;
static int quit_req;
static int captured;

static int mouse_buttons_now(void)
{
    return ((GetKeyState(VK_LBUTTON) < 0) ? 1 : 0) | ((GetKeyState(VK_RBUTTON) < 0) ? 2 : 0)
         | ((GetKeyState(VK_MBUTTON) < 0) ? 4 : 0);
}

/* Take the mouse (raw input, the cursor hidden and kept in the client area),
 * or give it back. Called again after a move or resize to re-clip. */
static void capture_mouse(int on)
{
    if (on && wnd && GetForegroundWindow() == wnd) {
        RECT r; GetClientRect(wnd, &r); MapWindowPoints(wnd, NULL, (POINT *)&r, 2);
        if (!captured) {
            RAWINPUTDEVICE rid = { 1, 2, RIDEV_INPUTSINK, wnd };
            RegisterRawInputDevices(&rid, 1, sizeof rid);
            ShowCursor(FALSE);
            captured = 1;
        }
        ClipCursor(&r);
    } else if (!on && captured) {
        ClipCursor(NULL); ShowCursor(TRUE); captured = 0;
    }
}

void host_snapshot(const char *path);

/* F12: the current frame to screenshots\<game>-NNN.bmp */
static void screenshot_key(void)
{
    char p[256];
    CreateDirectoryA("screenshots", NULL);
    for (int n = 0; n < 1000; n++) {
        snprintf(p, sizeof p, "screenshots/%s-%03d.bmp", g_game_id, n);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) { host_snapshot(p); return; }
    }
}
static BITMAPINFO bmi;

static void toggle_fullscreen(void)
{
    static WINDOWPLACEMENT prev = { sizeof prev };
    DWORD st = GetWindowLong(wnd, GWL_STYLE);
    if (st & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(wnd, &prev);
        GetMonitorInfo(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLong(wnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLong(wnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(wnd, &prev);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

/* Scaling (F12 cycles, --scale picks), the family's presenter shader
 * (src/present_d3d.c):
 *   sharp    sharp-bilinear at 4:3 (or the widescreen's shape): crisp texels
 *            with soft edges, no shimmer at odd scales (the default)
 *   smooth   bilinear
 *   crt      scanlines and an aperture grille
 *   nearest  plain pixels at 4:3
 *   integer  nearest, the largest whole multiple: square pixels (16:10)
 * F9 switches the colour look, natural or vivid. Without Direct3D 11 the
 * picture still shows, through GDI, nearest-neighbour. */
static const char *scale_names[] = { "sharp", "smooth", "crt", "nearest", "integer" };
static int scale_mode, look;
int d3d_present(HWND hw, const uint32_t *frame, int w, int h, const RECT *r, int cw, int ch, int mode, int look);

static void paint(HDC dc)
{
    RECT r; GetClientRect(wnd, &r);
    int cw = r.right, ch = r.bottom, w, h;
    if (scale_mode == 4) {                            /* integer: whole multiples */
        int k = cw / fw < ch / fh ? cw / fw : ch / fh;
        if (k < 1) k = 1;
        w = fw * k; h = fh * k;
    } else {                                          /* 4:3, or the widescreen's shape */
        w = cw; h = (int)(cw / frame_aspect);
        if (h > ch) { h = ch; w = (int)(ch * frame_aspect); }
    }
    int x = (cw - w) / 2, y = (ch - h) / 2;
    RECT pic = { x, y, x + w, y + h };
    if (!g_opt.gdi && d3d_present(wnd, frame, fw, fh, &pic, cw, ch, scale_mode == 4 ? 3 : scale_mode, look)) return;
    PatBlt(dc, 0, 0, cw, y, BLACKNESS); PatBlt(dc, 0, y + h, cw, ch - y - h, BLACKNESS);
    PatBlt(dc, 0, 0, x, ch, BLACKNESS); PatBlt(dc, x + w, 0, cw - x - w, ch, BLACKNESS);
    bmi.bmiHeader.biWidth = fw; bmi.bmiHeader.biHeight = -fh;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, w, h, 0, 0, fw, fh, frame, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

/* The presenter's choices and the window's place, kept beside the exe in
 * wolf3d.ini (as Red Alert 2 keeps ra2.ini); the command line wins. */
static char ini_path[MAX_PATH];

static void ini_load(void)
{
    GetModuleFileNameA(NULL, ini_path, sizeof ini_path);
    char *slash = strrchr(ini_path, '\\');
    strcpy(slash ? slash + 1 : ini_path, "wolf3d.ini");
    char v[64];
    GetPrivateProfileStringA("present", "scale", "sharp", v, sizeof v, ini_path);
    for (int i = 0; i < 5; i++) if (!strcmp(v, scale_names[i])) scale_mode = i;
    look = GetPrivateProfileIntA("present", "look", 0, ini_path) != 0;
    aim_on = GetPrivateProfileIntA("mouse", "aim", 1, ini_path) != 0;
    GetPrivateProfileStringA("mouse", "sens", "1", v, sizeof v, ini_path);
    aim_sens = atof(v) > 0 ? atof(v) : 1;
    if (!g_opt.fullscreen) g_opt.fullscreen = GetPrivateProfileIntA("present", "fullscreen", 0, ini_path);
}

static void ini_save(void)
{
    if (!ini_path[0]) return;
    WritePrivateProfileStringA("present", "scale", scale_names[scale_mode], ini_path);
    WritePrivateProfileStringA("present", "look", look ? "1" : "0", ini_path);
    WritePrivateProfileStringA("present", "fullscreen", (GetWindowLong(wnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) ? "0" : "1", ini_path);
    WritePrivateProfileStringA("present", "hires", hires_on ? (wide_on ? "wide" : "on") : "off", ini_path);
    WritePrivateProfileStringA("present", "dlss", hires_dlss_name(hires_dlss_mode()), ini_path);
    char sens[32]; snprintf(sens, sizeof sens, "%g", aim_sens);
    WritePrivateProfileStringA("present", "art", hires_art_mode() ? "1" : "0", ini_path);
    WritePrivateProfileStringA("mouse", "aim", aim_on ? "1" : "0", ini_path);
    WritePrivateProfileStringA("mouse", "sens", sens, ini_path);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CLOSE: quit_req = 1; return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SYSKEYDOWN: case WM_KEYDOWN: case WM_SYSKEYUP: case WM_KEYUP: {
        int up = (m == WM_KEYUP || m == WM_SYSKEYUP);
        if (wp == VK_RETURN && (lp & (1 << 29))) { if (!up) toggle_fullscreen(); return 0; }
        if (wp == VK_F4 && (lp & (1 << 29))) { quit_req = 1; return 0; }
        if (wp == VK_F12) {                           /* scaling; Ctrl+F12 a screenshot */
            if (!up) {
                if (GetKeyState(VK_CONTROL) < 0) screenshot_key();
                else { scale_mode = (scale_mode + 1) % 5; fprintf(stderr, "[present] scale %s\n", scale_names[scale_mode]); }
            }
            return 0;
        }
        if (wp == VK_F9) { if (!up) look ^= 1; return 0; }
        if (wp == VK_F6) {                            /* the upscaled art on/off */
            if (!up) {
                if (!hires_art_available()) fprintf(stderr, "[art] none: py tools\\upscale_art.py makes it\n");
                else { hires_set_art(!hires_art_mode()); fprintf(stderr, "[art] %s\n", hires_art_mode() ? "on" : "off"); }
            }
            return 0;
        }
        if (wp == VK_F7) {                            /* mouse aim on/off; off also lets the mouse go */
            if (!up) { aim_on ^= 1; capture_mouse(aim_on); fprintf(stderr, "[mouse] aim %s\n", aim_on ? "on" : "off"); }
            return 0;
        }
        if (wp == VK_F8) {                            /* DLSS: off -> DLAA -> quality -> balanced -> performance -> */
            if (!up) {
                int m = (hires_dlss_mode() + 1) % 5;
                if (!hires_set_dlss(m)) fprintf(stderr, "[dlss] not available in this build or on this GPU\n");
                else fprintf(stderr, "[dlss] %s\n", hires_dlss_name(m));
            }
            return 0;
        }
        if (wp == VK_F10) {                           /* widescreen -> original -> hi-res 4:3 -> */
            if (!up) {
                if (hires_on && wide_on) { hires_on = 0; wide_on = 0; }
                else if (!hires_on) hires_on = 1;
                else wide_on = 1;
                hires_set_wide(wide_on ? wide_aspect : 0);
                InvalidateRect(h, NULL, TRUE);
            }
            return 0;
        }
        if (wp == VK_F11) { if (!up) toggle_fullscreen(); return 0; }
        if (!up && (lp & (1 << 30))) return 0;      /* the game does its own repeat */
        int sc = (lp >> 16) & 0xFF;
        if (lp & (1 << 24)) kbd_scancode(0xE0);
        kbd_scancode((uint8_t)(sc | (up ? 0x80 : 0)));
        return 0;
    }
    case WM_SYSCHAR: return 0;                       /* no menu beep on Alt */

    /* Mouse: captured whenever the window has focus while mouse aim is on
     * (F7), otherwise by a click; losing focus releases it. Raw input, so the
     * motion is the device's, not the cursor's. */
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        if (!captured) { capture_mouse(1); return 0; }
        /* fall through: a click while captured is a button for the game */
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        if (captured) mouse_host(0, 0, (int)(((wp & MK_LBUTTON) ? 1 : 0) | ((wp & MK_RBUTTON) ? 2 : 0) | ((wp & MK_MBUTTON) ? 4 : 0)));
        return 0;
    case WM_INPUT: {
        RAWINPUT ri; UINT sz = sizeof ri;
        if (captured && GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1
            && ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            aim_motion(ri.data.mouse.lLastX, ri.data.mouse.lLastY, mouse_buttons_now());
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && aim_on && !HIWORD(wp)) capture_mouse(1);
        break;
    case WM_SIZE: case WM_MOVE:                       /* fullscreen and back: clip to the new client area */
        if (captured) capture_mouse(1);
        break;
    case WM_KILLFOCUS:
        capture_mouse(0);
        break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

/* waveOut: a ring the main thread fills and a few buffers cycling under it */
#define WBUF 8
#define WLEN 1024
static HWAVEOUT wout;
static WAVEHDR whdr[WBUF];
static int16_t wdata[WBUF][WLEN * 2];
static int wfill, wcur;

static void wave_open(void)
{
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, AUDIO_RATE, AUDIO_RATE * 4, 4, 16, 0 };
    if (waveOutOpen(&wout, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wout = NULL; return; }
    for (int i = 0; i < WBUF; i++) {
        whdr[i].lpData = (LPSTR)wdata[i]; whdr[i].dwBufferLength = WLEN * 4;
        waveOutPrepareHeader(wout, &whdr[i], sizeof whdr[i]);
        whdr[i].dwFlags |= WHDR_DONE;
    }
}

static void wave_push(const int16_t *s, int frames)
{
    while (frames > 0) {
        WAVEHDR *h = &whdr[wcur];
        if (!(h->dwFlags & WHDR_DONE)) return;       /* all queued: drop rather than block */
        int n = WLEN - wfill; if (n > frames) n = frames;
        memcpy(&wdata[wcur][wfill * 2], s, n * 4);
        wfill += n; s += n * 2; frames -= n;
        if (wfill == WLEN) {
            h->dwFlags &= ~WHDR_DONE;
            waveOutWrite(wout, h, sizeof *h);
            wfill = 0; wcur = (wcur + 1) % WBUF;
        }
    }
}
#endif

/* Gamepad: an XInput pad pressed as the keys the game already knows, so it
 * works in the menus and in play with no joystick calibration. The game's
 * own joystick support (port 201h) would want calibrating per session.
 *   stick / d-pad  arrows + WSAD RT  Ctrl (fire)     LT  Alt (strafe)
 *   A  Space (use)  X  Enter     RB  Right Shift (run)
 *   B / Start  Esc               Back  Tab                               */
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, void *);

static void pad_poll(void)
{
    static XInputGetState_t get;
    static int tried;
    static uint32_t held;
    /* each direction is an arrow (the menus) and W/S/A/D (play: the Steam
     * and GOG CONFIG's movement keys) */
    static const struct { uint32_t bit; uint8_t sc; uint8_t ext; } map[] = {
        {1 << 0, 0x48, 1}, {1 << 1, 0x50, 1}, {1 << 2, 0x4B, 1}, {1 << 3, 0x4D, 1},
        {1 << 0, 0x11, 0}, {1 << 1, 0x1F, 0}, {1 << 2, 0x1E, 0}, {1 << 3, 0x20, 0},
        {1 << 4, 0x1D, 0}, {1 << 5, 0x38, 0}, {1 << 6, 0x39, 0}, {1 << 7, 0x1C, 0},
        {1 << 8, 0x36, 0}, {1 << 9, 0x01, 0}, {1 << 10, 0x0F, 0}};
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } s;

    if (!tried) {
        HMODULE m;
        tried = 1;
        m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) get = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
    }
    if (!get || get(0, &s) != ERROR_SUCCESS) return;

    uint32_t now = 0;
    WORD b = s.buttons;
    if ((b & 0x0001) || s.ly >  12000) now |= 1 << 0;   /* up */
    if ((b & 0x0002) || s.ly < -12000) now |= 1 << 1;   /* down */
    if ((b & 0x0004) || s.lx < -12000) now |= 1 << 2;   /* left */
    if ((b & 0x0008) || s.lx >  12000) now |= 1 << 3;   /* right */
    if (s.rt > 64) now |= 1 << 4;
    if (s.lt > 64) now |= 1 << 5;
    if (b & 0x1000) now |= 1 << 6;                       /* A */
    if (b & 0x4000) now |= 1 << 7;                       /* X */
    if (b & 0x0200) now |= 1 << 8;                       /* RB */
    if (b & (0x2000 | 0x0010)) now |= 1 << 9;            /* B, Start */
    if (b & 0x0020) now |= 1 << 10;                      /* Back */

    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        uint32_t m = map[i].bit;
        if ((now ^ held) & m) {
            if (map[i].ext) kbd_scancode(0xE0);
            kbd_scancode((uint8_t)(map[i].sc | ((now & m) ? 0 : 0x80)));
        }
    }
    held = now;
}

void host_audio(const int16_t *s, int frames)
{
    if (rec_audio) { fwrite(s, 4, frames, rec_audio); rec_samples += frames; }
    if (wav_out) { fwrite(s, 4, frames, wav_out); wav_samples += frames; }
#ifdef _WIN32
    if (wout) wave_push(s, frames);
#endif
}

void host_init(void)
{
    char ini_hires[16] = "wide";                         /* F10's last choice, from wolf3d.ini */
    char ini_dlss[16] = "dlaa";                          /* F8's */
#ifdef _WIN32
    if (!g_opt.headless) {
        ini_load();
        GetPrivateProfileStringA("present", "hires", "wide", ini_hires, sizeof ini_hires, ini_path);
        GetPrivateProfileStringA("present", "dlss", "dlaa", ini_dlss, sizeof ini_dlss, ini_path);
    }
#endif
    {   /* DLSS: DLAA in a window when the build and the GPU have it, off
         * headless (deterministic runs) unless asked for; docs/dlss.md */
        const char *dl = g_opt.dlss ? g_opt.dlss : g_opt.headless ? "off" : ini_dlss;
        int m = hires_dlss_parse(dl);
        if (m < 0) fprintf(stderr, "[dlss] unknown mode '%s' (off, dlaa, quality, balanced, performance, ultra)\n", dl);
        else if (m && !hires_set_dlss(m) && g_opt.dlss)
            fprintf(stderr, "[dlss] not available (a build without the SDK, or not an RTX GPU): running without\n");
    }
    {   /* the upscaled art: on when it exists, unless --art off or F6 turned it off */
        int on = 1;
#ifdef _WIN32
        if (!g_opt.headless) on = GetPrivateProfileIntA("present", "art", 1, ini_path) != 0;
#endif
        if (g_opt.art) on = strcmp(g_opt.art, "off") != 0;
        hires_set_art(on);
    }
    if (g_opt.mouse_aim) aim_on = strcmp(g_opt.mouse_aim, "off") != 0;    /* the command line wins */
    if (g_opt.mouse_sens > 0) aim_sens = g_opt.mouse_sens;
    int hires_cli = g_opt.hires >= 0;
    if (g_opt.hires < 0) g_opt.hires = g_opt.headless ? 0 : 4;   /* on in a window */
    hires_on = g_opt.hires > 0 && (hires_cli || strcmp(ini_hires, "off"));
    /* widescreen: on in a window unless --widescreen off; "W:H" or a ratio */
    const char *ws = g_opt.wide ? g_opt.wide : g_opt.headless || strcmp(ini_hires, "wide") ? "off" : "16:9";
    double aw = 0, ah = 1;
    if (sscanf(ws, "%lf:%lf", &aw, &ah) >= 1 && ah > 0) wide_aspect = aw / ah;
    if (wide_aspect <= 4.0 / 3) wide_aspect = 0;
    wide_on = wide_aspect > 0;
    if (!wide_aspect) wide_aspect = 16.0 / 9;           /* what F10 turns on */
    hires_set_wide(wide_on ? wide_aspect : 0);
    keys_parse();
    route_parse();
    if (g_opt.record) rec_open();
    if (g_opt.wav && (wav_out = fopen(g_opt.wav, "wb")) != NULL) wav_header(wav_out, 0);
#ifdef _WIN32
    if (g_opt.headless) return;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "wolf3d";
    wc.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
    RegisterClassA(&wc);
    int s = g_opt.window ? g_opt.window : 3;
    RECT r = { 0, 0, wide_on && hires_on ? (int)(240 * s * wide_aspect) : 320 * s, 240 * s };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wnd = CreateWindowA("wolf3d", "Wolfenstein 3-D", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                        r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    if (g_opt.fullscreen) toggle_fullscreen();
    if (aim_on) capture_mouse(1);
    for (int i = 0; i < 5; i++)
        if (g_opt.scale_name && !strcmp(g_opt.scale_name, scale_names[i])) scale_mode = i;
    if (!g_opt.mute) wave_open();
#endif
}

void host_frame(void)
{
    static uint64_t last_present;
    keys_due();
    mouse_due();
    route_due();
    uint64_t now = pit_now();
    shots_due(now);
    { void hires_debug_pos(unsigned); hires_debug_pos((unsigned)(now * 1000 / PIT_HZ)); }
    if (g_opt.seconds > 0 && now >= (uint64_t)(g_opt.seconds * PIT_HZ)) dos_exit(0);
    if (rec_video) {
        uint64_t per = (uint64_t)(PIT_HZ / REC_FPS);
        if ((rec_frames + 1) * per <= now) {
            compose();
            const uint32_t *f = as_rec();
            while ((rec_frames + 1) * per <= now) { fwrite(f, 4, (size_t)rec_w * rec_h, rec_video); rec_frames++; }
        }
    }
#ifdef _WIN32
    if (!wnd) return;
    if (now - last_present < (uint64_t)(PIT_HZ / 70)) return;
    last_present = now;
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (GetForegroundWindow() == wnd) pad_poll();
    if (quit_req) dos_exit(0);
    compose();
    HDC dc = GetDC(wnd); paint(dc); ReleaseDC(wnd, dc);
#endif
}

/* The last thing on screen, as a BMP -- a run nobody watched still says where
 * it got to. */
void host_snapshot(const char *path)
{
    compose();
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t sz = 54 + fw * fh * 4, off = 54, hs = 40, zero = 0, img = fw * fh * 4;
    int32_t w = fw, h = -fh; uint16_t planes = 1, bpp = 32;
    fwrite("BM", 1, 2, f); fwrite(&sz, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&off, 4, 1, f);
    fwrite(&hs, 4, 1, f); fwrite(&w, 4, 1, f); fwrite(&h, 4, 1, f); fwrite(&planes, 2, 1, f);
    fwrite(&bpp, 2, 1, f); fwrite(&zero, 4, 1, f); fwrite(&img, 4, 1, f);
    fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&zero, 4, 1, f);
    fwrite(frame, 4, fw * fh, f);
    fclose(f);
}

void host_shutdown(void)
{
    rec_close();
    if (wav_out) { wav_header(wav_out, (uint32_t)wav_samples); fclose(wav_out); wav_out = NULL; }
#ifdef _WIN32
    if (wout) { waveOutReset(wout); waveOutClose(wout); }
    if (wnd) { ini_save(); DestroyWindow(wnd); }
    dlss_shutdown();
#endif
}
