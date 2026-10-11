/*
 * hires.c - the 3D view redrawn at high resolution, from the game's own art.
 *
 * The original keeps running and drawing exactly as it did: game logic, HUD
 * and the low-resolution 3D view are all still the lifted 1992 code. Hooks
 * tools/lift.py wraps around it supply what this needs:
 *
 *   hires_hit()    after each of the raycaster's Hit* routines: which texture
 *                  page and column this view column shows, the wall height,
 *                  and the exact intercept -- the game's own answer to "what
 *                  is in this column", including doors and pushwalls;
 *   hires_shape()  entering ScaleShape / SimpleScaleShape: the sprite's height;
 *   hires_sline()  each sprite column the line scaler sends through a
 *                  compiled scaler: the shape, its post list, the screen column;
 *   g_draw_tag     set while the walls, the floor/ceiling clear and the
 *                  sprite columns run, so vga.c knows who wrote every pixel.
 *
 * At present time the compositor upsamples: each output column sits between
 * two of the game's columns, and where both hit the same texture page its
 * texture coordinate and height are interpolated, so walls are sampled from
 * the 64x64 textures at full output resolution. Sprites are fitted back to
 * their centre and drawn from their own posts. Layering is by owner: hi-res
 * walls over the flat floor and ceiling, sprites over walls, and whatever the
 * game drew on top (text, the fizzle) stays.
 *
 * Every DGROUP offset used here is read out of the game's own code at lift
 * time (find_renderer in tools/lift.py). See docs/renderer.md.
 */
#include "machine.h"
#include "pool.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int g_draw_tag;

typedef struct {
    uint16_t seg, off;          /* the texture column: postsource */
    uint16_t wh;                /* wallheight[]: 8x the half-height in pixels */
    uint16_t yi, xi;            /* the intercepts' fractions, for sub-texel */
    double hx, hy;              /* the hit point, in tiles: DLSS's depth scale (dlss_view) */
    uint8_t ok;
} Col;

static Col cur[320], shown[320];
#define WALLH_MAX ((320 + 2 * 160) * 6)
static double wallh[WALLH_MAX];          /* hi-res wall height (wallheight units) per output column */
static int dirty, have;

static inline uint16_t rd16(uint16_t seg, uint16_t off) { return mem_read16(&g_cpu, seg, off); }

/* Widescreen (see hires_side): the game's own frame is pass 0; passes 1 and 2
 * are the view rotated left and right, cast by the game's code again. */
static int pass;
static uint16_t dgroup;                  /* DGROUP, as the raycaster saw it */
static Col side_cur[2][320], side_shown[2][320];
static int wide_cols;                    /* game columns added each side; 0 off */
static int side_ran, side_shown_ok;      /* the side passes ran for this frame */
static double side_rot;                  /* their rotation, radians toward column 0 */
static int16_t pa_tab[320];              /* pixelangle[]: each column's ray */

void hires_set_wide(double aspect)
{
    int e = aspect > 0 ? (int)lround(320 * (aspect * 0.75 - 1) / 2) : 0;   /* 320x200 shows at 4:3 */
    wide_cols = e < 0 ? 0 : e > 160 ? 160 : e;
}
int hires_wide_cols(void) { return wide_cols; }

void hires_hit(CPU *c)
{
    const HiresVars *h = &g_hires;
    if (!h->ok) return;
    uint16_t ds = c->ds, x = rd16(ds, h->pixx);
    if (x >= 320) return;
    Col *k = pass ? &side_cur[pass - 1][x] : &cur[x];
    k->seg = rd16(ds, h->postseg);
    k->off = rd16(ds, h->postoff);
    k->wh = rd16(ds, (uint16_t)(h->wallheight + 2 * x));
    k->yi = rd16(ds, h->yint);
    k->xi = rd16(ds, h->xint);
    k->hx = (int32_t)(rd16(ds, h->xint) | (uint32_t)rd16(ds, (uint16_t)(h->xint + 2)) << 16) / 65536.0;
    k->hy = (int32_t)(rd16(ds, h->yint) | (uint32_t)rd16(ds, (uint16_t)(h->yint + 2)) << 16) / 65536.0;
    k->ok = 1;
    if (!pass) { dirty = 1; dgroup = ds; }
}

/* ---- sprites ---------------------------------------------------------------
 *
 * ScaleShape(xcenter, shapenum, height) draws an actor or object, height in
 * wallheight units; SimpleScaleShape the weapon, height in pixels. Both hand
 * each run of screen columns showing one texel column to the line scaler,
 * which walks that column's post list -- {end*2, source, start*2} words,
 * terminated by 0 -- in the shape's page, whose t_compshape header
 * {leftpix, rightpix, dataofs[]} says which texel column that list belongs
 * to. Capturing each column gives the sprite back: its page, its height and,
 * by fitting the texel column of every captured screen column, its exact
 * centre. Then it can be drawn at any resolution from its own posts, occluded
 * by the hi-res walls the same way the game occludes it. */
typedef struct {
    uint32_t x;                 /* bufferofs * 4 + view column: resolved at the flip */
    double height;              /* full height in pixels */
    uint16_t seg, off;
    uint8_t w;                  /* screen columns this call covers */
    uint8_t pass, top;          /* top: the weapon, never behind walls */
} SCol;

typedef struct {
    uint16_t seg;
    double height;
    uint8_t pass, top;
    double xc;                  /* the centre, in screen pixels */
    int x0, x1;                 /* the screen columns the game drew */
} Sprite;

#define MAXSCOL 4096
#define MAXSPR 256
static SCol scols[MAXSCOL];
static int nscols;
static Sprite sprs[MAXSPR], sprs_shown[MAXSPR];
static int nsprs, nsprs_shown;
static double shape_h;                   /* the sprite being drawn */
static int shape_top;

void hires_shape(CPU *c, int weapon)
{
    uint16_t height = rd16(c->ss, (uint16_t)(c->sp + 8));   /* past the far return, xcenter, shapenum */
    shape_h = weapon ? height : height / 4.0;               /* wallheight units are 4 per pixel of height */
    shape_top = weapon;
}

void hires_sline(CPU *c)
{
    const HiresVars *h = &g_hires;
    if (!h->ok || nscols >= MAXSCOL) return;
    uint16_t ds = c->ds;
    SCol *k = &scols[nscols++];
    k->x = (uint32_t)rd16(ds, h->bufofs) * 4 + rd16(ds, h->sl_x);
    k->w = (uint8_t)rd16(ds, h->sl_w);
    if (!k->w) k->w = 1;
    k->seg = rd16(ds, h->sl_seg);
    k->off = rd16(ds, h->sl_cmd);
    k->height = shape_h;
    k->pass = (uint8_t)pass;
    k->top = (uint8_t)(!pass && shape_top);
}

static uint16_t seg_word(uint16_t seg, unsigned off) { return (uint16_t)(g_cpu.mem[(seg * 16u + off) & 0xFFFFF] | g_cpu.mem[(seg * 16u + off + 1) & 0xFFFFF] << 8); }

/* Which texel column of its shape a post list is. */
static int shape_column(uint16_t seg, uint16_t off)
{
    int left = seg_word(seg, 0), right = seg_word(seg, 2);
    if (left < 0 || right > 63 || left > right) return -1;
    for (int k = 0; k <= right - left; k++)
        if (seg_word(seg, 4 + 2 * k) == off) return left + k;
    return -1;
}

static int cmp_px(const void *a, const void *b) { return ((const int *)a)[0] - ((const int *)b)[0]; }

/* One sprite from columns already known to belong together: fit its centre.
 * A texel is height/64 pixels wide (the art is square), and pixel centre p
 * shows texel column c when xc + (c-32)k <= p < xc + (c-31)k. Every column
 * narrows the interval. */
static void fit_one(const int (*pc)[3], int n, const SCol *proto)
{
    if (nsprs >= MAXSPR || n <= 0) return;
    double k = proto->height / 64.0, lo = -1e9, hi = 1e9;
    for (int m = 0; m < n; m++) {
        double a = pc[m][0] + pc[m][2] - 0.5 - (pc[m][1] - 31) * k;
        double b = pc[m][0] + 0.5 - (pc[m][1] - 32) * k;
        if (a > lo) lo = a;
        if (b < hi) hi = b;
    }
    Sprite *sp = &sprs[nsprs++];
    sp->seg = proto->seg; sp->height = proto->height;
    sp->pass = proto->pass; sp->top = proto->top;
    sp->x0 = pc[0][0]; sp->x1 = pc[n - 1][0] + pc[n - 1][2] - 1;
    sp->xc = lo <= hi ? (lo + hi) / 2 : (sp->x0 + sp->x1 + 1) / 2.0;
    if (getenv("WOLF_SPRITE_DEBUG"))
        fprintf(stderr, "[spr] seg=%04X h=%.2f cols=%d x=%d..%d xc=%.2f [%g,%g]\n",
                sp->seg, sp->height, n, sp->x0, sp->x1, sp->xc, lo, hi);
}

/* Group the frame's columns into sprites: consecutive captures of the same
 * shape at the same height, split wherever the screen columns are not
 * contiguous (two copies of one object at one distance). */
static void fit_sprites(unsigned start)
{
    static int pc[MAXSCOL][3];
    nsprs = 0;
    for (int i = 0; i < nscols; ) {
        int j = i;
        while (j < nscols && scols[j].seg == scols[i].seg && scols[j].height == scols[i].height
               && scols[j].pass == scols[i].pass && scols[j].top == scols[i].top) j++;
        int n = 0;
        for (int m = i; m < j; m++) {
            int c = shape_column(scols[m].seg, scols[m].off);
            if (c < 0) continue;
            pc[n][0] = (int)(((scols[m].x - start * 4) & 0x3FFFF) % 320);
            pc[n][1] = c;
            pc[n][2] = scols[m].w;
            n++;
        }
        qsort(pc, n, sizeof pc[0], cmp_px);
        int a = 0;
        for (int m = 1; m <= n; m++)
            if (m == n || pc[m][0] > pc[m - 1][0] + pc[m - 1][2]) {
                fit_one((const int (*)[3])&pc[a], m - a, &scols[i]);
                a = m;
            }
        i = j;
    }
    nscols = 0;
}

/* ---- widescreen -------------------------------------------------------------
 *
 * The game casts its view's columns; a wider view needs rays beyond its edges.
 * It gets them from the game itself: right after DrawScaleds, WallRefresh and
 * DrawScaleds run twice more with the view turned left and then right about
 * the eye, and the hooks above capture those passes' columns and sprites as
 * they capture the real frame's. The passes must leave no trace, so the whole
 * of memory and the registers are put back after each (spotvis, the automap,
 * an actor's "seen" flag all change), video memory takes no writes, and no
 * interrupt runs or time passes -- the game, headless or not, cannot tell.
 *
 * WallRefresh derives the eye from player->x/y/angle (the eye sits
 * focallength behind the player), so a pass turns the angle and moves the
 * player round the eye by the same amount: the eye stays put. The turn is
 * whole degrees (the game's angles), just under the view's full width, so a
 * side strip falls near the edge of the turned view, where its columns are
 * densest, and overlaps the real view by a degree. */
static uint8_t side_snap[MEM_SIZE];
static uint64_t side_us, side_n;

static int32_t rd32(uint16_t seg, uint16_t off) { return (int32_t)(rd16(seg, off) | (uint32_t)rd16(seg, (uint16_t)(off + 2)) << 16); }
static void wr32(uint16_t seg, uint16_t off, int32_t v)
{
    mem_write16(&g_cpu, seg, off, (uint16_t)v);
    mem_write16(&g_cpu, seg, (uint16_t)(off + 2), (uint16_t)((uint32_t)v >> 16));
}

#define FINE (2 * M_PI / 3600)                   /* pixelangle[] units: FINEANGLES = 3600 */

/* The eye this frame was cast from (tiles; radians, 0 east, counter-clockwise),
 * for DLSS's motion vectors: captured here, shown at the flip. */
typedef struct { int ok; double ex, ey, a; } Cam;
static Cam cam_cur, cam_shown;

void hires_side(CPU *c)
{
    const HiresVars *h = &g_hires;
    if (pass || !h->ok || !h->ws_ok) return;
    uint16_t ds = c->ds, pl = rd16(ds, h->player);
    int pa0 = (int16_t)rd16(ds, h->pixelangle);
    for (int i = 0; i < 320; i++) pa_tab[i] = (int16_t)rd16(ds, (uint16_t)(h->pixelangle + 2 * i));
    if (!pl || !pa0) return;
    {
        int ang = rd16(ds, (uint16_t)(pl + h->p_angle));
        double f = rd32(ds, h->focal), a = ang * M_PI / 180;
        cam_cur.ok = 1; cam_cur.a = a;
        cam_cur.ex = (rd32(ds, (uint16_t)(pl + h->p_x)) - f * cos(a)) / 65536.0;
        cam_cur.ey = (rd32(ds, (uint16_t)(pl + h->p_y)) + f * sin(a)) / 65536.0;
    }
    if (!wide_cols) return;
    uint64_t t0 = host_us();
    CPU regs = *c;
    int budget = g_recomp_tick_budget;
    memcpy(side_snap, c->mem, MEM_SIZE);
    vga_hold(1);
    g_tick_hold = 1;

    int ang = rd16(ds, (uint16_t)(pl + h->p_angle));
    double f = rd32(ds, h->focal), a = ang * M_PI / 180;
    double ex = rd32(ds, (uint16_t)(pl + h->p_x)) - f * cos(a);
    double ey = rd32(ds, (uint16_t)(pl + h->p_y)) + f * sin(a);
    int delta = (int)(2 * abs(pa0) / 10.0) - 1;          /* degrees */
    for (int k = 0; k < 2; k++) {
        int turn = ((k == 0) == (pa0 > 0)) ? delta : -delta;   /* pass 1 toward column 0 */
        int a2 = ((ang + turn) % 360 + 360) % 360;
        double r = a2 * M_PI / 180;
        wr32(ds, (uint16_t)(pl + h->p_x), (int32_t)lround(ex + f * cos(r)));
        wr32(ds, (uint16_t)(pl + h->p_y), (int32_t)lround(ey - f * sin(r)));
        mem_write16(&g_cpu, ds, (uint16_t)(pl + h->p_angle), (uint16_t)a2);
        memset(side_cur[k], 0, sizeof side_cur[k]);
        pass = k + 1;
        hires_side_call(c, 0);
        hires_side_call(c, 1);
        pass = 0;
        memcpy(c->mem, side_snap, MEM_SIZE);
        *c = regs;
    }
    vga_hold(0);
    g_tick_hold = 0;
    g_recomp_tick_budget = budget;
    side_rot = delta * M_PI / 180;
    side_ran = 1;
    side_us += host_us() - t0; side_n++;
}

/* The player's position in tiles and angle in degrees (0 east, 90 north), for
 * the --route autopilot (host.c); 0 before the first 3D frame. */
int hires_player(double *x, double *y, int *angle)
{
    const HiresVars *h = &g_hires;
    if (!h->ws_ok || !dgroup) return 0;
    uint16_t pl = rd16(dgroup, h->player);
    if (!pl) return 0;
    *x = rd32(dgroup, (uint16_t)(pl + h->p_x)) / 65536.0;
    *y = rd32(dgroup, (uint16_t)(pl + h->p_y)) / 65536.0;
    *angle = rd16(dgroup, (uint16_t)(pl + h->p_angle));
    return 1;
}

/* WOLF_POS=1 (a development aid for writing scripted runs): every 250 ms of
 * game time the player's tile, fraction and angle go to stderr. */
void hires_debug_pos(unsigned ms)
{
    static int init, on;
    static unsigned next;
    const HiresVars *h = &g_hires;
    if (!init) { init = 1; on = getenv("WOLF_POS") != NULL; }
    if (!on || !h->ws_ok || !dgroup || ms < next) return;
    next = ms + 250;
    uint16_t pl = rd16(dgroup, h->player);
    if (!pl) return;
    /* WOLF_WARP=ms:x,y,angle (tiles, degrees): move the player there once,
     * to look around a map while writing a script; never used by a checked run */
    static int warped;
    const char *wp = getenv("WOLF_WARP");
    double wx, wy; int wa; unsigned wt;
    if (wp && !warped && sscanf(wp, "%u:%lf,%lf,%d", &wt, &wx, &wy, &wa) == 4 && ms >= wt) {
        warped = 1;
        wr32(dgroup, (uint16_t)(pl + h->p_x), (int32_t)(wx * 65536));
        wr32(dgroup, (uint16_t)(pl + h->p_y), (int32_t)(wy * 65536));
        mem_write16(&g_cpu, dgroup, (uint16_t)(pl + h->p_angle), (uint16_t)wa);
    }
    int32_t x = rd32(dgroup, (uint16_t)(pl + h->p_x)), y = rd32(dgroup, (uint16_t)(pl + h->p_y));
    fprintf(stderr, "[pos] t=%u tile=%d,%d at %.2f,%.2f angle=%d\n", ms, x >> 16, y >> 16,
            x / 65536.0, y / 65536.0, rd16(dgroup, (uint16_t)(pl + h->p_angle)));
}

/* The game flips pages by moving the CRTC start address once a frame is
 * drawn; the columns captured since the last flip belong to that frame. */
void hires_flip(void)
{
    if (!dirty && !nscols) return;
    memcpy(shown, cur, sizeof shown);
    memset(cur, 0, sizeof cur);
    cam_shown = cam_cur;
    cam_cur.ok = 0;
    memcpy(side_shown, side_cur, sizeof side_shown);
    side_shown_ok = side_ran;
    side_ran = 0;
    fit_sprites(vga_scan_start());
    memcpy(sprs_shown, sprs, sizeof(Sprite) * nsprs);
    nsprs_shown = nsprs;
    dirty = 0;
    have = 1;
}

/* A column's texture coordinate in texels from the start of its segment:
 * the column the game chose, plus where in that texel the ray landed. The
 * fraction comes from whichever intercept agrees with the chosen column
 * (vertical walls use y, horizontal x; textures facing the other way run
 * backwards); a door's texture slides, so nothing agrees and it gets the
 * texel's middle. */
static double texcoord(const Col *k)
{
    int c = (k->off >> 6) & 63;
    uint16_t src[2] = { k->yi, k->xi };
    for (int i = 0; i < 2; i++) {
        int cc = (src[i] >> 10) & 63;
        double f = (src[i] & 0x3FF) / 1024.0;
        if (cc == c) return (k->off >> 6) + f;
        if (63 - cc == c) return (k->off >> 6) + 1.0 - f;
    }
    return (k->off >> 6) + 0.5;
}

static int scale_of(int S) { return S < 1 ? 1 : S > 6 ? 6 : S; }

static int compose_inner(uint32_t *out, int S, int *w, int *h);
static void dlss_view(uint32_t *out);
static uint64_t c_us, c_n;
void hires_report(void)
{
    if (c_n) fprintf(stderr, "[hires] %llu frames, %.2f ms each\n", (unsigned long long)c_n, c_us / 1000.0 / c_n);
    if (side_n) fprintf(stderr, "[hires] %llu widescreen side passes, %.2f ms each\n",
                        (unsigned long long)side_n, side_us / 1000.0 / side_n);
}

/* The whole screen at S x 320 by S x 200, the 3D view redrawn. */

/* One column of the wide view: what the game chose there (or, in the side
 * strips, what a side pass did), ready to resample. */
typedef struct {
    uint16_t seg;
    int col;                    /* the texel column the game chose: the clamp */
    double tc, hh;              /* texture coordinate in texels; half height in px */
    uint8_t ok;
} VCol;

#define VMAX (320 + 2 * 160)
static VCol vcols[VMAX];

static VCol from_col(const Col *k)
{
    VCol v = {0};
    if (!k->ok) return v;
    v.seg = k->seg; v.col = k->off >> 6; v.tc = texcoord(k); v.hh = k->wh / 8.0; v.ok = 1;
    return v;
}

/* Side pass k's view at angle psi (radians toward column 0, from that view's
 * centre), between the two columns whose rays bracket it. */
static VCol side_col(int k, double psi)
{
    VCol v = {0};
    double s = pa_tab[0] > 0 ? FINE : -FINE;
    if (psi > pa_tab[0] * s || psi < pa_tab[319] * s) return v;
    int j = 0;
    while (j < 318 && pa_tab[j + 1] * s >= psi) j++;   /* ponytail: linear scan, 2 x E x 320 a frame */
    double pj = pa_tab[j] * s, pk = pa_tab[j + 1] * s, t = pj > pk ? (pj - psi) / (pj - pk) : 0;
    const Col *a = &side_shown[k][j], *b = &side_shown[k][j + 1];
    if (!a->ok) { a = b; t = 0; }
    if (!a->ok) return v;
    v = from_col(a);
    int lo = v.col, hi = v.col;
    if (b->ok && b != a) {
        VCol w = from_col(b);
        if (w.seg == v.seg && fabs(w.tc - v.tc) < 2.0) {
            v.tc += (w.tc - v.tc) * t; v.hh += (w.hh - v.hh) * t;
            lo = v.col < w.col ? v.col : w.col; hi = v.col < w.col ? w.col : v.col;
        } else if (t >= 0.5) {
            v = w; lo = hi = w.col;
        }
    }
    v.col = (int)floor(v.tc);
    if (v.col < lo) v.col = lo;
    if (v.col > hi) v.col = hi;
    return v;
}

/* The last composed frame's view, for the DLSS pass (dlss_view) to render
 * again: where it is in game pixels and in the output, and its geometry. */
static struct {
    int ok, S, OX, W;           /* scale; output x of game column 0; frame width */
    int art;                    /* the upscaled art was used */
    int g0, nv, vx0, vw, vy0, vh;   /* the (wide) view, in game columns and rows */
    double F, cx, yc;           /* focal length and centre, game pixels */
    uint32_t pal[256];
} V;
static uint8_t px[200][320], ow[200][320];
static uint8_t ceil_c, floor_c;

/* A wall at view position u (game columns from the wide view's left edge,
 * continuous): its texture column's address and its half height in game
 * pixels. 0 where nothing was hit. Each position lies between two of the
 * game's columns; where both show the same surface the texture coordinate and
 * height are interpolated, at an edge the nearer wins. The game's own column
 * choice is ground truth: the sub-texel fraction may move between the two
 * neighbours' columns but never past them -- at 63.99 + a fraction it would
 * read the next column in memory, a dashed line down the seam. */
static int wall_sample(double u, int nv, uint32_t *tex, double *hh_out, uint16_t *seg_out, double *tcol)
{
    int c0 = (int)floor(u);
    double t = u - c0;
    if (c0 < 0) { c0 = 0; t = 0; }
    if (c0 >= nv - 1) { c0 = nv - 1; t = 0; }
    const VCol *a = &vcols[c0], *b = &vcols[c0 + (c0 + 1 < nv)];
    if (!a->ok) return 0;
    double tc = a->tc, hh = a->hh;
    uint16_t seg = a->seg;
    if (b->ok && b != a) {
        if (b->seg == a->seg && fabs(b->tc - tc) < 2.0) {      /* same surface: interpolate */
            tc += (b->tc - tc) * t;
            hh += (b->hh - hh) * t;
        } else if (t >= 0.5) {                                /* an edge: nearest */
            tc = b->tc; hh = b->hh; seg = b->seg;
        }
    }
    int col = (int)floor(tc), ca = a->col, cb = b->col;
    int lo = seg == b->seg && b->ok ? (ca < cb ? ca : cb) : (seg == a->seg ? ca : cb);
    int hi = seg == b->seg && b->ok ? (ca < cb ? cb : ca) : lo;
    if (seg != a->seg) lo = hi = cb;
    if (col < lo) col = lo; if (col > hi) col = hi;
    *tex = (uint32_t)seg * 16 + (uint32_t)col * 64;
    *hh_out = hh;
    *seg_out = seg;                               /* for the upscaled art: the page, and */
    double f = tc - col;                          /* where in the chosen texel column */
    *tcol = col + (f < 0 ? 0 : f > 0.999 ? 0.999 : f);
    return 1;
}

/* ---- upscaled art (optional: tools/upscale_art.py, docs/art.md) ---------------
 *
 * work/<game>/art/art.bin holds every wall and sprite page of the game, 4x
 * by a neural upscaler, as RGBA. A page is found in guest memory by a hash of
 * its contents -- the page cache moves pages around, but a page's bytes are
 * its bytes -- the same hash tools/upscale_art.py computes from VSWAP: all
 * 4096 bytes of a wall; a sprite's header, post commands and the pixels they
 * use. The art is RGB, so the game's palette effects (the damage flash, the
 * fades) are put back by fitting the live palette against the one the art
 * was made from, per channel, c' = a*c + b: both effects are of that form. */
typedef struct { uint32_t type, key, w, h; const uint8_t *rgba; } ArtEnt;
static ArtEnt *art;
static int nart, art_tried, art_on = 1;
static uint8_t art_pal[256][3];
static int art_hash[4096];                      /* key -> index + 1, open addressing */
static float art_a[3], art_b[3];
static struct { uint16_t seg; uint8_t type; const ArtEnt *e; } acache[256];
static int nacache, art_frozen;

static int art_load(void)
{
    if (art_tried) return nart > 0;
    art_tried = 1;
    char path[256];
    snprintf(path, sizeof path, "work/%s/art/art.bin", g_game_id);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n);
    if (!d || fread(d, 1, n, f) != (size_t)n || n < 24 + 768 || memcmp(d, "WOLFART1", 8)) { fclose(f); free(d); return 0; }
    fclose(f);
    uint32_t count; memcpy(&count, d + 8, 4);
    memcpy(art_pal, d + 24, 768);
    art = calloc(count, sizeof *art);
    size_t at = 24 + 768;
    for (uint32_t i = 0; i < count && at + 16 <= (size_t)n; i++) {
        ArtEnt *e = &art[nart];
        memcpy(e, d + at, 16);
        at += 16;
        if (at + (size_t)e->w * e->h * 4 > (size_t)n) break;
        e->rgba = d + at;
        at += (size_t)e->w * e->h * 4;
        uint32_t k = (e->key ^ e->type * 0x9E3779B9u) & 4095;
        while (art_hash[k]) k = (k + 1) & 4095;
        art_hash[k] = ++nart;
    }
    fprintf(stderr, "[art] %d upscaled pages from %s\n", nart, path);
    return nart > 0;
}

static uint32_t fnv(uint32_t h, uint8_t b) { return (h ^ b) * 0x01000193u; }

static const ArtEnt *art_find(uint16_t seg, int type)
{
    for (int i = 0; i < nacache; i++) if (acache[i].seg == seg && acache[i].type == type) return acache[i].e;
    const uint8_t *m = g_cpu.mem;
    uint32_t base = (uint32_t)seg * 16, h = 0x811C9DC5u;
    if (type == 0) {
        for (int i = 0; i < 4096; i++) h = fnv(h, m[(base + i) & 0xFFFFF]);
    } else {
        int left = seg_word(seg, 0), right = seg_word(seg, 2);
        if (left < 0 || right > 63 || left > right) return NULL;
        for (int i = 0; i < 4 + 2 * (right - left + 1); i++) h = fnv(h, m[(base + i) & 0xFFFFF]);
        for (int c = left; c <= right; c++)
            for (unsigned cmd = seg_word(seg, 4 + 2 * (c - left)), g = 0; g < 64; g++, cmd += 6) {
                int end = seg_word(seg, cmd);
                if (!end) break;
                int src = (int16_t)seg_word(seg, cmd + 2), st = seg_word(seg, cmd + 4);
                for (int k = 0; k < 6; k++) h = fnv(h, m[(base + cmd + k) & 0xFFFFF]);
                for (int r = st / 2; r < end / 2; r++) h = fnv(h, m[(base + ((src + r) & 0xFFFF)) & 0xFFFFF]);
            }
    }
    const ArtEnt *e = NULL;
    for (uint32_t k = (h ^ type * 0x9E3779B9u) & 4095; art_hash[k]; k = (k + 1) & 4095)
        if (art[art_hash[k] - 1].key == h && art[art_hash[k] - 1].type == (uint32_t)type) { e = &art[art_hash[k] - 1]; break; }
    if (!art_frozen && nacache < 256) { acache[nacache].seg = seg; acache[nacache].type = (uint8_t)type; acache[nacache++].e = e; }
    return e;
}

/* The wall pages this frame shows, looked up before the columns are drawn in
 * parallel: from then on art_find only reads its cache. */
static void art_prepare(int nv)
{
    art_frozen = 0;
    for (int i = 0; i < nv; i++) if (vcols[i].ok) art_find(vcols[i].seg, 0);
    art_frozen = 1;
}

/* Once a frame: forget which segment held which page, and fit the palette. */
static void art_frame(const uint32_t *pal)
{
    nacache = 0;
    for (int c = 0; c < 3; c++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < 256; i++) {
            double x = art_pal[i][c], y = pal[i] >> (16 - 8 * c) & 255;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        double v = sxx - sx * sx / 256;
        art_a[c] = (float)(v > 1e-6 ? (sxy - sx * sy / 256) / v : 1);
        art_b[c] = (float)((sy - art_a[c] * sx) / 256);
    }
}

/* The art at texel position (u, v), 0..64 each, bilinear; *alpha 0..255. */
static uint32_t art_px(const ArtEnt *e, double u, double v, int *alpha)
{
    double K = e->w / 64.0, fx = u * K - 0.5, fy = v * K - 0.5;
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    double tx = fx - x0, ty = fy - y0, acc[4] = {0};
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int x = x0 + i, y = y0 + j;
            if (e->type == 0) x = (x + (int)e->w) % (int)e->w;          /* walls wrap */
            else if (x < 0) x = 0; else if (x >= (int)e->w) x = e->w - 1;
            if (x < 0) x = 0;
            if (y < 0) y = 0; if (y >= (int)e->h) y = e->h - 1;
            const uint8_t *p = e->rgba + ((size_t)y * e->w + x) * 4;
            double wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            for (int k = 0; k < 4; k++) acc[k] += p[k] * wgt;
        }
    if (alpha) *alpha = (int)acc[3];
    uint32_t o = 0xFF000000u;
    for (int c = 0; c < 3; c++) {
        double y = art_a[c] * acc[c] + art_b[c];
        o |= (uint32_t)(y < 0 ? 0 : y > 255 ? 255 : y) << (16 - 8 * c);
    }
    return o;
}

int hires_art_available(void) { return art_load(); }
int hires_art_mode(void) { return art_on && nart > 0; }
void hires_set_art(int on) { art_on = on; }

/* Where each sprite goes in the wide view: its centre and height in game
 * pixels, and the game columns it may cover. A side pass's sprites are moved
 * into the wide view and kept to their strip. */
typedef struct { const Sprite *sp; double xc, ht, xlo, xhi; } SprAt;
static SprAt spr_at[MAXSPR];
static int nspr_at;

static void place_sprites(int wide, int g0, int nv, int vx0, int vx1, double cx, double F)
{
    nspr_at = 0;
    for (int si = 0; si < nsprs_shown; si++) {
        const Sprite *sp = &sprs_shown[si];
        SprAt a = { sp, sp->xc, sp->height, vx0, vx1 + 1 };
        if (sp->pass) {
            if (!wide) continue;
            int k = sp->pass - 1;
            double pr = atan((cx - (a.xc - vx0)) / F), psi = pr + (k ? -side_rot : side_rot);
            if (fabs(psi) >= M_PI / 2 - 0.01) continue;
            a.xc = vx0 + cx - F * tan(psi);
            a.ht *= cos(pr) / cos(psi);
            if (k == 0) { a.xlo = g0; a.xhi = vx0; }
            else        { a.xlo = vx1 + 1; a.xhi = g0 + nv; }
        }
        if (a.ht > 0) spr_at[nspr_at++] = a;
    }
}

/* The compositor's per-column work, spread over the cores (src/pool.c):
 * every output column is drawn independently, so the wall pass and each
 * sprite's columns run in parallel. The art lookups are made serially first
 * (art_prepare), so the threads only read. */
typedef struct {
    uint32_t *out; const uint32_t *pal; const uint8_t *mem;
    int S, g0, nv, OX, E, W, vy0, vy1, use_art;
    double yc;
    const SprAt *at; const ArtEnt *ae; int X0;  /* the sprite being drawn */
} Job;

static void wall_col(int i, void *ctx)
{
    const Job *j = ctx;
    const int S = j->S, X = j->g0 * S + i, W = j->W;
    const double yc = j->yc;
    uint32_t tex; double hh;
    uint16_t wseg; double tcol;
    if (!wall_sample((X + 0.5) / S - 0.5 - j->g0, j->nv, &tex, &hh, &wseg, &tcol)) return;
    wallh[i] = hh * 8;
    if (hh <= 0) return;
    /* rows the wall covers: |(Y + 0.5 - yc) / S| < hh */
    int y_lo = (int)ceil(yc - hh * S - 0.5), y_hi = (int)floor(yc + hh * S - 0.5);
    if (y_lo < j->vy0 * S) y_lo = j->vy0 * S;
    if (y_hi > (j->vy1 + 1) * S - 1) y_hi = (j->vy1 + 1) * S - 1;
    double r0 = 32.0 + (y_lo + 0.5 - yc) / S / hh * 32.0, dr = 32.0 / (S * hh);
    int64_t R = (int64_t)(r0 * 65536.0), dR = (int64_t)(dr * 65536.0);
    int x = (X + j->OX) / S - j->E;                         /* the game column; < 0 or > 319 in a strip */
    int gate = x >= 0 && x < 320;
    uint32_t *o = j->out + (size_t)y_lo * W + j->OX + X;
    const ArtEnt *ae = j->use_art ? art_find(wseg, 0) : NULL;
    for (int Y = y_lo; Y <= y_hi; Y++, o += W, R += dR) {
        if (gate) {
            uint8_t ox = ow[Y / S][x];
            if (ox != DRAW_WALL && ox != DRAW_PLANE && !(ox == DRAW_SPRITE && nsprs_shown)) continue;
        }
        if (ae) {                                           /* the upscaled art, at the exact texel position */
            double rv = r0 + (Y - y_lo) * dr;
            *o = art_px(ae, tcol, rv < 0 ? 0 : rv > 63.999 ? 63.999 : rv, NULL);
            continue;
        }
        int r = (int)(R >> 16);
        if (r < 0) r = 0; if (r > 63) r = 63;
        *o = j->pal[j->mem[(tex + r) & 0xFFFFF]];
    }
}

static void sprite_col(int i, void *ctx)
{
    const Job *j = ctx;
    const int S = j->S, X = j->X0 + i, W = j->W, OX = j->OX;
    const SprAt *at = j->at;
    const Sprite *sp = at->sp;
    const double xc = at->xc, ht = at->ht, k = ht / 64.0, hh = ht / 2.0, yc = j->yc;   /* texel width; half height */
    if (!sp->top && wallh[X - j->g0 * S] > ht * 4.0) return;    /* behind a wall (wall units) */
    int x = (X + OX) / S - j->E, gate = !sp->pass && x >= 0 && x < 320;
    if (j->ae) {                                        /* the upscaled art: its own smooth silhouette */
        double uc = ((X + 0.5) / S - xc) / k + 32;
        int Ya = (int)ceil(yc - hh * S - 0.5), Yb = (int)floor(yc + hh * S - 0.5);
        if (Ya < j->vy0 * S) Ya = j->vy0 * S;
        if (Yb > (j->vy1 + 1) * S - 1) Yb = (j->vy1 + 1) * S - 1;
        for (int Y = Ya; Y <= Yb; Y++) {
            if (gate && ow[Y / S][x] == DRAW_OTHER) continue;
            double rr = 32.0 + (Y + 0.5 - yc) / S / hh * 32.0;
            int a;
            uint32_t c = art_px(j->ae, uc, rr < 0 ? 0 : rr > 63.999 ? 63.999 : rr, &a);
            if (a >= 128) j->out[(size_t)Y * W + OX + X] = c;
        }
        return;
    }
    int left = seg_word(sp->seg, 0), right = seg_word(sp->seg, 2);
    int c = (int)floor(((X + 0.5) / S - xc) / k) + 32;
    if (c < left || c > right) return;
    unsigned cmd = seg_word(sp->seg, 4 + 2 * (c - left));
    for (int guard = 0; guard < 64; guard++, cmd += 6) {
        int end = seg_word(sp->seg, cmd) >> 1;
        if (!end) break;
        int src = seg_word(sp->seg, cmd + 2), st = seg_word(sp->seg, cmd + 4) >> 1;
        int Ya = (int)ceil(yc + (st - 32) / 32.0 * hh * S - 0.5);
        int Yb = (int)ceil(yc + (end - 32) / 32.0 * hh * S - 0.5);
        if (Ya < j->vy0 * S) Ya = j->vy0 * S;
        if (Yb > (j->vy1 + 1) * S) Yb = (j->vy1 + 1) * S;
        for (int Y = Ya; Y < Yb; Y++) {
            if (gate && ow[Y / S][x] == DRAW_OTHER) continue;
            int r = (int)floor(32.0 + (Y + 0.5 - yc) / S / hh * 32.0);
            if (r < st) r = st;
            if (r >= end) r = end - 1;
            j->out[(size_t)Y * W + OX + X] = j->pal[j->mem[((uint32_t)sp->seg * 16 + (uint16_t)(src + r)) & 0xFFFFF]];
        }
    }
}

static int compose_inner(uint32_t *out, int S, int *w, int *h)
{
    S = scale_of(S);
    V.ok = 0;
    if (!vga_unchained()) return 0;
    const uint8_t *vram = vga_vram(), *own = vga_owner();
    const uint8_t *mem = g_cpu.mem;
    unsigned start = vga_scan_start(), row = vga_row_bytes();
    int vx0 = 320, vx1 = -1, vy0 = 200, vy1 = -1;
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++) {
            unsigned a = ((((start + y * row + (x >> 2)) & 0xFFFF) << 2) | (x & 3));
            px[y][x] = vram[a];
            ow[y][x] = own[a];
            if (own[a] == DRAW_WALL || own[a] == DRAW_PLANE) {
                if (x < vx0) vx0 = x; if (x > vx1) vx1 = x;
                if (y < vy0) vy0 = y; if (y > vy1) vy1 = y;
            }
        }
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = vga_color(i);
    const int use_art = art_on && art_load();
    if (use_art) art_frame(pal);
    /* Widescreen: the frame is E game columns wider each side, always, so
     * the window's shape holds; the 4:3 picture sits in the middle and only
     * the 3D view, when it spans the screen, reaches into the sides. */
    const int E = wide_cols, OX = E * S;
    const int W = (320 + 2 * E) * S;
    *w = W; *h = 200 * S;

    /* the original, enlarged: one output row per source row, then copied */
    for (int y = 0; y < 200; y++) {
        uint32_t *o = out + (size_t)y * S * W;
        for (int k = 0; k < OX; k++) o[k] = o[W - 1 - k] = 0xFF000000u;
        for (int x = 0; x < 320; x++) {
            uint32_t c = pal[px[y][x]];
            for (int k = 0; k < S; k++) o[OX + x * S + k] = c;
        }
        for (int r = 1; r < S; r++) memcpy(o + (size_t)r * W, o, (size_t)W * 4);
    }
    if (!have || vx1 < vx0 || vx1 - vx0 + 1 > 320) return 1;

    /* the wide view's columns, and the geometry that places the side passes:
     * column i's ray is at pixelangle[i] = atan((cx - i - 0.5) / F) */
    const int vw = vx1 - vx0 + 1;
    const int wide = E && side_shown_ok && vx0 == 0 && vx1 == 319;
    const int e = wide ? E : 0, g0 = vx0 - e, nv = vw + 2 * e;
    const double cx = vw / 2.0, s = pa_tab[0] > 0 ? FINE : -FINE;
    double F = 0;
    {
        double num = 0, den = 0;
        for (int i = 0; i < vw; i++) {
            double d = cx - i - 0.5;
            num += d * d; den += d * tan(pa_tab[i] * s);
        }
        F = den > 0 ? num / den : 0;
    }
    for (int i = 0; i < nv; i++) {
        int g = g0 + i - vx0;                    /* view column */
        if (g >= 0 && g < vw) { vcols[i] = from_col(&shown[g]); continue; }
        int k = g < 0 ? 0 : 1;
        double psi = atan((cx - g - 0.5) / F), pr = psi - (k ? -side_rot : side_rot);
        vcols[i] = side_col(k, pr);
        vcols[i].hh *= cos(pr) / cos(psi);       /* height goes as 1 / perpendicular distance */
    }

    /* floor and ceiling: flat colours, the ones the game cleared to (kept
     * from the last frame that showed any, when a wall fills the view) */
    const double yc = (vy0 + (vy1 - vy0 + 1) / 2.0) * S;     /* the horizon, in output rows */
    for (int y = vy0; y <= vy1; y++)
        for (int x = vx0; x <= vx1; x++)
            if (ow[y][x] == DRAW_PLANE) { if ((y + 0.5) * S < yc) ceil_c = px[y][x]; else floor_c = px[y][x]; }
    for (int Y = vy0 * S; Y < (vy1 + 1) * S; Y++) {
        const uint8_t *owr = ow[Y / S];
        uint32_t c = pal[Y + 0.5 < yc ? ceil_c : floor_c];
        uint32_t *o = out + (size_t)Y * W + OX + g0 * S;
        for (int x = g0; x < g0 + nv; x++) {
            int mine = x < 0 || x >= 320 || owr[x] == DRAW_WALL || owr[x] == DRAW_PLANE
                       || (owr[x] == DRAW_SPRITE && nsprs_shown);
            for (int k = 0; k < S; k++, o++) if (mine) *o = c;
        }
    }

    /* walls, column by column, over the planes; then sprites, back to front
     * as the game drew them, over everything but the overlays the game put
     * on top of them (text, the fizzle) */
    for (int i = 0; i < nv * S; i++) wallh[i] = 0;
    Job job = { out, pal, mem, S, g0, nv, OX, E, W, vy0, vy1, use_art, yc, NULL, NULL, 0 };
    if (use_art) art_prepare(nv);
    pool_for(nv * S, wall_col, &job);
    place_sprites(wide, g0, nv, vx0, vx1, cx, F);
    for (int si = 0; si < nspr_at; si++) {
        const SprAt *at = &spr_at[si];
        double k = at->ht / 64.0;
        int left = seg_word(at->sp->seg, 0), right = seg_word(at->sp->seg, 2);
        int X0 = (int)floor((at->xc + (left - 32) * k) * S), X1 = (int)ceil((at->xc + (right + 1 - 32) * k) * S);
        if (X0 < at->xlo * S) X0 = (int)(at->xlo * S);
        if (X1 > at->xhi * S) X1 = (int)(at->xhi * S);
        job.at = at; job.X0 = X0;
        job.ae = use_art ? art_find(at->sp->seg, 1) : NULL;
        pool_for(X1 - X0, sprite_col, &job);
    }
    V.ok = 1; V.S = S; V.OX = OX; V.W = W;
    V.g0 = g0; V.nv = nv; V.vx0 = vx0; V.vw = vw; V.vy0 = vy0; V.vh = vy1 - vy0 + 1;
    V.F = F; V.cx = vx0 + cx; V.yc = yc / S; V.art = use_art;
    memcpy(V.pal, pal, sizeof pal);
    return 1;
}

/* ---- DLSS -------------------------------------------------------------------
 *
 * With DLSS on (--dlss, F8; built only with the optional SDK, docs/dlss.md),
 * the view the compositor drew is drawn again, by dlss_view, at DLSS's render
 * size with a different sub-pixel jitter each frame, alongside what DLSS
 * needs to accumulate those frames into one at the output size:
 *
 *   depth   from each pixel's height on screen: a wall or sprite of half
 *           height hh is at K / hh tiles, a floor or ceiling pixel dy rows
 *           from the horizon at K / dy. K is measured every frame from the
 *           raycaster's own hit points (distance along the view times hh).
 *   motion  exact for everything that does not move: the pixel's world
 *           position (from its depth and ray) projected through the previous
 *           frame's eye. Sprites get the same, which is right for objects and
 *           corpses and close for walking actors; the weapon moves with the
 *           eye (no motion) and is marked so DLSS trusts the current frame
 *           there, since its animation frames are not motion.
 *
 * Overlays the game drew on the view (text, the fizzle fade) stay on top. */
int dlss_available(void);
int dlss_render_size(int ow, int oh, int mode, int *rw, int *rh);
int dlss_eval(const uint32_t *color, const float *depth, const float *mv, const uint8_t *bias,
              int rw, int rh, float jx, float jy, int reset, uint32_t *out, int ow, int oh, int mode);
#ifndef WOLF_DLSS                       /* built without the SDK: never available */
int dlss_available(void) { return 0; }
int dlss_render_size(int ow, int oh, int mode, int *rw, int *rh) { (void)ow; (void)oh; (void)mode; (void)rw; (void)rh; return 0; }
int dlss_eval(const uint32_t *c, const float *d, const float *m, const uint8_t *b, int rw, int rh, float jx, float jy,
              int reset, uint32_t *out, int ow, int oh, int mode)
{ (void)c; (void)d; (void)m; (void)b; (void)rw; (void)rh; (void)jx; (void)jy; (void)reset; (void)out; (void)ow; (void)oh; (void)mode; return 0; }
void dlss_shutdown(void) {}
#endif

static int dlss_mode;                    /* 0 off, 1 DLAA, 2 quality, 3 balanced, 4 performance, 5 ultra performance */
static const char *dlss_names[] = { "off", "dlaa", "quality", "balanced", "performance", "ultra" };
int hires_dlss_mode(void) { return dlss_mode; }
const char *hires_dlss_name(int m) { return m >= 0 && m < 6 ? dlss_names[m] : "?"; }
int hires_dlss_parse(const char *s) { for (int i = 0; i < 6; i++) if (!strcmp(s, dlss_names[i])) return i; return -1; }
int hires_set_dlss(int m)
{
    if (m && !dlss_available()) { dlss_mode = 0; return 0; }
    dlss_mode = m;
    return 1;
}

static double halton(int i, int b) { double f = 1, r = 0; for (; i > 0; i /= b) { f /= b; r += f * (i % b); } return r; }

/* dlss_view's per-column work, in parallel like the compositor's. */
typedef struct {
    uint32_t *col; float *dep, *mv; uint8_t *bias;
    int rw, rh, reset;
    double jx, jy, sx, sy, K;
    Cam cam, prev;
    double Dx, Dy, Lx, Ly, pDx, pDy, pLx, pLy;   /* this eye's forward and left, and the previous one's */
    const SprAt *at; const ArtEnt *ae;           /* the sprite being drawn */
    int X0, Y0, Y1, left, right;
    double k, hs, d;
} DJob;
static double dl_colhh[4096];                    /* 640 columns x 6, the widest frame */

/* A pixel at game (gx, gy) whose depth is d, through the previous eye. */
static void dj_motion(const DJob *j, size_t i, double gx, double gy, double d, int still)
{
    float *m = &j->mv[2 * i];
    m[0] = m[1] = 0;
    if (still || j->reset) return;
    double t = (V.cx - gx) / V.F;
    double wx = j->cam.ex + d * (j->Dx + t * j->Lx), wy = j->cam.ey + d * (j->Dy + t * j->Ly);
    double rx = wx - j->prev.ex, ry = wy - j->prev.ey;
    double d2 = rx * j->pDx + ry * j->pDy, l2 = rx * j->pLx + ry * j->pLy;
    if (d2 > 0.05) {
        m[0] = (float)((V.cx - V.F * l2 / d2 - gx) / j->sx);
        m[1] = (float)((V.yc + (gy - V.yc) * d / d2 - gy) / j->sy);
    }
}

static void dj_wall_col(int X, void *ctx)
{
    const DJob *j = ctx;
    const uint8_t *mem = g_cpu.mem;
    double gx = V.g0 + (X + 0.5 + j->jx) * j->sx, gxc = V.g0 + (X + 0.5) * j->sx;
    uint32_t tex = 0; double hh = 0;
    uint16_t wseg = 0; double tcol = 0;
    int wall = wall_sample(gx - 0.5 - V.g0, V.nv, &tex, &hh, &wseg, &tcol) && hh > 0;
    const ArtEnt *wae = wall && V.art ? art_find(wseg, 0) : NULL;
    dl_colhh[X < 4096 ? X : 4095] = wall ? hh : 0;
    for (int Y = 0; Y < j->rh; Y++) {
        size_t i = (size_t)Y * j->rw + X;
        double gy = V.vy0 + (Y + 0.5 + j->jy) * j->sy, gyc = V.vy0 + (Y + 0.5) * j->sy, dy = gy - V.yc;
        double he;
        if (wall && fabs(dy) < hh) {
            if (wae) {
                double rv = 32 + dy / hh * 32;
                j->col[i] = art_px(wae, tcol, rv < 0 ? 0 : rv > 63.999 ? 63.999 : rv, NULL);
            } else {
                int r = (int)floor(32 + dy / hh * 32);
                if (r < 0) r = 0; if (r > 63) r = 63;
                j->col[i] = V.pal[mem[(tex + r) & 0xFFFFF]];
            }
            he = hh;
        } else {
            j->col[i] = V.pal[dy < 0 ? ceil_c : floor_c];
            he = fabs(dy) > 0.25 ? fabs(dy) : 0.25;
        }
        double d = j->K / he;
        j->dep[i] = (float)(d > 0.05 ? 0.05 / d : 1.0);
        j->bias[i] = 0;
        dj_motion(j, i, gxc, gyc, d, 0);
    }
}

static void dj_sprite_col(int n, void *ctx)
{
    const DJob *j = ctx;
    const SprAt *at = j->at;
    const Sprite *sp = at->sp;
    const uint8_t *mem = g_cpu.mem;
    const int X = j->X0 + n;
    double gx = V.g0 + (X + 0.5 + j->jx) * j->sx;
    if (gx < at->xlo || gx >= at->xhi) return;
    if (!sp->top && dl_colhh[X < 4096 ? X : 4095] > j->hs) return;   /* behind this column's wall */
    double d = j->d, gxc = V.g0 + (X + 0.5) * j->sx;
    if (j->ae) {                                        /* the upscaled art and its smooth silhouette */
        double uc = (gx - at->xc) / j->k + 32;
        for (int Y = j->Y0; Y < j->Y1; Y++) {
            double rr = 32 + (V.vy0 + (Y + 0.5 + j->jy) * j->sy - V.yc) / j->hs * 32;
            if (rr < 0 || rr >= 64) continue;
            int a;
            uint32_t cc = art_px(j->ae, uc, rr, &a);
            if (a < 128) continue;
            size_t i = (size_t)Y * j->rw + X;
            j->col[i] = cc;
            j->dep[i] = (float)(0.05 / d > 1 ? 1 : 0.05 / d);
            j->bias[i] = sp->top ? 255 : 0;
            dj_motion(j, i, gxc, V.vy0 + (Y + 0.5) * j->sy, d, sp->top);
        }
        return;
    }
    int c = (int)floor((gx - at->xc) / j->k) + 32;
    if (c < j->left || c > j->right) return;
    unsigned cmd0 = seg_word(sp->seg, 4 + 2 * (c - j->left));
    for (int Y = j->Y0; Y < j->Y1; Y++) {
        double gy = V.vy0 + (Y + 0.5 + j->jy) * j->sy;
        int r = (int)floor(32 + (gy - V.yc) / j->hs * 32);
        unsigned cmd = cmd0;
        for (int guard = 0; guard < 64; guard++, cmd += 6) {
            int end = seg_word(sp->seg, cmd) >> 1;
            if (!end) break;
            int st = seg_word(sp->seg, cmd + 4) >> 1;
            if (r < st || r >= end) continue;
            size_t i = (size_t)Y * j->rw + X;
            j->col[i] = V.pal[mem[((uint32_t)sp->seg * 16 + (uint16_t)(seg_word(sp->seg, cmd + 2) + r)) & 0xFFFFF]];
            j->dep[i] = (float)(0.05 / d > 1 ? 1 : 0.05 / d);
            j->bias[i] = sp->top ? 255 : 0;
            dj_motion(j, i, gxc, V.vy0 + (Y + 0.5) * j->sy, d, sp->top);
            break;
        }
    }
}

static void dlss_view(uint32_t *out)
{
    static uint32_t *col, *res;
    static float *dep, *mv;
    static uint8_t *bias;
    static size_t cap, rcap;
    static Cam prev;
    static int frame, last_w, last_h, last_ox;
    static double K = 1;
    const int S = V.S, ow_ = V.nv * S, oh_ = V.vh * S;
    int rw, rh;
    if (!dlss_render_size(ow_, oh_, dlss_mode - 1, &rw, &rh)) return;
    if ((size_t)rw * rh > cap) {
        cap = (size_t)rw * rh;
        free(col); free(dep); free(mv); free(bias);
        col = malloc(cap * 4); dep = malloc(cap * 4); mv = malloc(cap * 8); bias = malloc(cap);
    }
    if ((size_t)ow_ * oh_ > rcap) { rcap = (size_t)ow_ * oh_; free(res); res = malloc(rcap * 4); }
    if (!col || !dep || !mv || !bias || !res) return;

    /* the eye, and K from this frame's hit points (median of hh x distance) */
    const Cam cam = cam_shown;
    double Dx = cos(cam.a), Dy = -sin(cam.a), Lx = -sin(cam.a), Ly = -cos(cam.a);   /* forward, left; y runs south */
    {
        static double ks[320];
        int n = 0;
        for (int i = 0; i < 320; i++)
            if (shown[i].ok && shown[i].wh) {
                double d = (shown[i].hx - cam.ex) * Dx + (shown[i].hy - cam.ey) * Dy;
                if (d > 0.05) ks[n++] = shown[i].wh / 8.0 * d;
            }
        if (n) {
            for (int i = 1; i < n; i++) for (int j = i; j > 0 && ks[j] < ks[j - 1]; j--) { double t = ks[j]; ks[j] = ks[j - 1]; ks[j - 1] = t; }
            K = ks[n / 2];
        }
    }
    int reset = !prev.ok || !cam.ok || ow_ != last_w || oh_ != last_h || V.OX + V.g0 * S != last_ox
                || hypot(cam.ex - prev.ex, cam.ey - prev.ey) > 2 || fabs(remainder(cam.a - prev.a, 2 * M_PI)) > M_PI / 4;
    double pDx = cos(prev.a), pDy = -sin(prev.a), pLx = -sin(prev.a), pLy = -cos(prev.a);

    int phases = (int)(8.0 * ow_ * oh_ / ((double)rw * rh));
    if (phases < 8) phases = 8; if (phases > 64) phases = 64;
    frame = frame % phases + 1;
    /* This sample is at the pixel centre + (jx, jy); DLSS takes the offset
     * the other way round. Measured: with +jx a still view converged ~40%
     * further from the plain render (docs/dlss.md). */
    const double jx = halton(frame, 2) - 0.5, jy = halton(frame, 3) - 0.5;
    const double sx = (double)V.nv / rw, sy = (double)V.vh / rh;   /* game pixels per render pixel */
    const uint8_t *mem = g_cpu.mem;

    DJob dj = { col, dep, mv, bias, rw, rh, reset, jx, jy, sx, sy, K, cam, prev,
                Dx, Dy, Lx, Ly, pDx, pDy, pLx, pLy, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 };
    pool_for(rw, dj_wall_col, &dj);
    for (int si = 0; si < nspr_at; si++) {
        const SprAt *at = &spr_at[si];
        const Sprite *sp = at->sp;
        dj.at = at;
        dj.k = at->ht / 64.0; dj.hs = at->ht / 2.0; dj.d = sp->top ? 0.05 : K / dj.hs;
        dj.left = seg_word(sp->seg, 0); dj.right = seg_word(sp->seg, 2);
        dj.ae = V.art ? art_find(sp->seg, 1) : NULL;
        double xa = at->xc + (dj.left - 32) * dj.k, xb = at->xc + (dj.right + 1 - 32) * dj.k;
        if (xa < at->xlo) xa = at->xlo;
        if (xb > at->xhi) xb = at->xhi;
        int X0 = (int)floor((xa - V.g0) / sx - 0.5 - jx), X1 = (int)ceil((xb - V.g0) / sx - 0.5 - jx);
        if (X0 < 0) X0 = 0; if (X1 > rw) X1 = rw;
        dj.Y0 = (int)floor((V.yc - dj.hs - V.vy0) / sy - 0.5 - jy); dj.Y1 = (int)ceil((V.yc + dj.hs - V.vy0) / sy - 0.5 - jy) + 1;
        if (dj.Y0 < 0) dj.Y0 = 0; if (dj.Y1 > rh) dj.Y1 = rh;
        dj.X0 = X0;
        pool_for(X1 - X0, dj_sprite_col, &dj);
    }
    uint64_t te = host_us();
    int evok = dlss_eval(col, dep, mv, bias, rw, rh, (float)-jx, (float)-jy, reset, res, ow_, oh_, dlss_mode - 1);
    { extern uint64_t g_perf_eval; g_perf_eval += host_us() - te; }
    if (!evok)
        return;
    prev = cam; last_w = ow_; last_h = oh_; last_ox = V.OX + V.g0 * S;
    /* into the frame, under whatever the game drew over the view */
    const int x0 = V.OX + V.g0 * S, y0 = V.vy0 * S;
    for (int Y = 0; Y < oh_; Y++) {
        int gy = (y0 + Y) / S;
        uint32_t *o = out + (size_t)(y0 + Y) * V.W + x0;
        const uint32_t *r = res + (size_t)Y * ow_;
        for (int X = 0; X < ow_; X++) {
            int gxi = (int)floor((double)(x0 + X - V.OX) / S);
            if (gxi >= 0 && gxi < 320 && ow[gy][gxi] == DRAW_OTHER) continue;
            o[X] = r[X];
        }
    }
}

/* The whole screen at S x 320 by S x 200, the 3D view redrawn (and through
 * DLSS when it is on). */
int hires_compose(uint32_t *out, int S, int *w, int *h)
{
    uint64_t t = host_us();
    int r = compose_inner(out, S, w, h);
    uint64_t t1 = host_us();
    if (r && V.ok && dlss_mode) dlss_view(out);
    c_us += host_us() - t; c_n++;
    { extern uint64_t g_perf_inner, g_perf_dlss; g_perf_inner += t1 - t; g_perf_dlss += host_us() - t1; }
    return r;
}
