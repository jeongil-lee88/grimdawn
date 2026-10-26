/* In-world note icons (included by native-lorevo.c).
   A scroll icon floats over every place where a lore note lies that this character has ALREADY collected
   (mods/lore_audio/lore_spots.txt: body tag, note record, world x/z). Clicking the icon reads the note aloud;
   the click is swallowed so the character does not walk there.
   Drawn after ActivityManager::Render(GraphicsCanvas&) (Game.dll), projected with WorldCamera::Project.
   Every engine call is wrapped in SEH: on any fault the icons switch themselves off for the session. */

typedef unsigned char byte;
typedef struct { void *region; float x, y, z; unsigned pad; } WorldVec3;
typedef struct { float x, y; } Vec2;
typedef struct { float x, y, w, h; } Rect;              /* Viewport::GetRect: x, y, width, height */
typedef struct { float r, g, b, a; } Color;
typedef struct { union { char sbuf[16]; const char *ptr; } buf; size_t len, cap; } NStr;
typedef struct { char tag[112]; char record[128]; float x, z; int entry; DWORD checked; int owned; } Spot;

typedef void (*ActRenderFn)(void *self, void *canvas);
static ActRenderFn orig_act_render;
static void *(*get_main_player)(void *ge);
static void *(*get_game_camera)(void *ge);
static Vec2 *(*cam_project)(void *cam, Vec2 *out, const WorldVec3 *w, const void *viewport);
static const void *(*canvas_viewport)(void *canvas);
static void (*canvas_rect)(void *canvas, const Rect *dst, const Rect *src, const void *rtex, const Color *c);
static void *(*get_graphics)(void *engine);
static void *(*load_tex)(void *graphics, const NStr *name);
static const void *(*tex_render)(const void *gtex);
static Rect *(*tex_rect)(const void *gtex, Rect *out);
static unsigned (*lore_has)(void *player, const NStr *record);
static void **g_game_engine, **g_engine;
static Spot *spots; static int n_spots;
static const void *icon_gtex, *icon_rtex; static Rect icon_src;
static volatile LONG icons_off;
static HWND game_wnd; static WNDPROC orig_wndproc;
#define MAX_SHOWN 16
static Rect shown[MAX_SHOWN]; static int shown_entry[MAX_SHOWN]; static volatile LONG n_shown;
static float vp_w = 1, vp_h = 1;

static void nstr(NStr *s, const char *t) {
    memset(s, 0, sizeof *s); s->len = strlen(t);
    if (s->len < 16) { memcpy(s->buf.sbuf, t, s->len + 1); s->cap = 15; }
    else { s->buf.ptr = t; s->cap = s->len; }
}

static int load_spots(void) {
    wchar_t path[MAX_PATH]; FILE *f; char line[400]; int cap = 300;
    swprintf_s(path, MAX_PATH, L"%ls\\lore_spots.txt", lore_dir);
    if (!(f = _wfopen(path, L"r"))) return 0;
    spots = (Spot *)calloc(cap, sizeof(Spot));
    while (fgets(line, sizeof line, f)) {
        char tag[112], rec[128]; float x, z;
        if (sscanf_s(line, "%111[^\t]\t%127[^\t]\t%f\t%f", tag, (unsigned)sizeof tag, rec, (unsigned)sizeof rec, &x, &z) != 4) continue;
        if (n_spots == cap) { cap *= 2; spots = (Spot *)realloc(spots, cap * sizeof(Spot)); }
        memset(&spots[n_spots], 0, sizeof(Spot));
        strcpy_s(spots[n_spots].tag, sizeof spots[0].tag, tag); strcpy_s(spots[n_spots].record, sizeof spots[0].record, rec);
        spots[n_spots].x = x; spots[n_spots].z = z; spots[n_spots].entry = find(tag);
        if (spots[n_spots].entry >= 0) n_spots++;
    }
    fclose(f);
    return n_spots;
}

static LRESULT CALLBACK icon_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static int swallow_up;
    if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && n_shown > 0) {
        RECT cr; float mx, my; int k;
        GetClientRect(h, &cr);
        mx = (float)(short)LOWORD(lp) * vp_w / (float)(cr.right > 0 ? cr.right : 1);
        my = (float)(short)HIWORD(lp) * vp_h / (float)(cr.bottom > 0 ? cr.bottom : 1);
        for (k = 0; k < n_shown; k++) {
            Rect r = shown[k];
            if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h) {
                int e = shown_entry[k];
                entries[e].last = GetTickCount();
                log_line("ICON %s", entries[e].tag);
                InterlockedExchange(&pending, e);
                swallow_up = 1;
                return 0;                              /* the game never sees this click */
            }
        }
    }
    if (msg == WM_LBUTTONUP && swallow_up) { swallow_up = 0; return 0; }
    return CallWindowProcW(orig_wndproc, h, msg, wp, lp);
}

static BOOL CALLBACK find_wnd(HWND h, LPARAM lp) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}

static void draw_icons(void *canvas) {
    void *ge, *player, *cam, *graphics; byte *pr; const void *vp; float px, py, pz, size; int i, n = 0;
    DWORD now = GetTickCount();
    if (!g_game_engine || !(ge = *g_game_engine) || !(player = get_main_player(ge)) || !(pr = *(byte **)((byte *)player + 0xd8))) { n_shown = 0; return; }
    if (!icon_rtex) {
        NStr name; nstr(&name, "ui/caravan/stashbuttons/button_stashcustom_symbol-scroll01a_up.tex");
        if (!g_engine || !*g_engine || !(graphics = get_graphics(*g_engine))) return;
        if (!(icon_gtex = load_tex(graphics, &name))) return;
        icon_rtex = tex_render(icon_gtex); tex_rect(icon_gtex, &icon_src);
        log_line("ICONS texture %.0fx%.0f", icon_src.w, icon_src.h);
        if (!icon_rtex) return;
    }
    if (!game_wnd) {
        EnumWindows(find_wnd, (LPARAM)&game_wnd);
        if (game_wnd) { orig_wndproc = (WNDPROC)SetWindowLongPtrW(game_wnd, GWLP_WNDPROC, (LONG_PTR)icon_wndproc); log_line("ICONS window hooked"); }
    }
    vp = canvas_viewport(canvas);
    vp_w = (float)((const int *)vp)[2]; vp_h = (float)((const int *)vp)[3];
    cam = get_game_camera(ge);
    if (!cam) return;
    px = *(float *)((byte *)player + 0xe0) + (float)*(int *)(pr + 0x3c);
    py = *(float *)((byte *)player + 0xe4);
    pz = *(float *)((byte *)player + 0xe8) + (float)*(int *)(pr + 0x44);
    size = vp_h / 22.0f;
    for (i = 0; i < n_spots && n < MAX_SHOWN; i++) {
        Spot *s = &spots[i]; float dx = s->x - px, dz = s->z - pz; WorldVec3 w; Vec2 sc; Rect dst; Color c = {1, 1, 1, 1};
        if (dx * dx + dz * dz > 45.0f * 45.0f) continue;
        if (now - s->checked > 2000) { NStr rec; nstr(&rec, s->record); s->owned = lore_has(player, &rec) != 0; s->checked = now; }
        if (!s->owned) continue;                       /* not read yet: the game's own note is still there */
        w.region = pr; w.x = s->x - (float)*(int *)(pr + 0x3c); w.y = py + 1.6f; w.z = s->z - (float)*(int *)(pr + 0x44); w.pad = 0;
        cam_project(cam, &sc, &w, vp);
        if (sc.x < 0 || sc.y < 0 || sc.x > vp_w || sc.y > vp_h) continue;
        dst.x = sc.x - size / 2; dst.y = sc.y - size / 2; dst.w = size; dst.h = size;
        canvas_rect(canvas, &dst, &icon_src, icon_rtex, &c);
        shown[n] = dst; shown_entry[n] = s->entry; n++;
    }
    n_shown = n;
}

static volatile LONG last_move_tick;               /* last frame the character's position changed */
static void track_move(void) {
    static float lx, lz; void *ge, *player; float x, z;
    if (!g_game_engine || !(ge = *g_game_engine) || !(player = get_main_player(ge))) return;
    x = *(float *)((byte *)player + 0xe0); z = *(float *)((byte *)player + 0xe8);
    if ((x - lx) * (x - lx) + (z - lz) * (z - lz) > 0.0004f) InterlockedExchange(&last_move_tick, (LONG)GetTickCount());
    lx = x; lz = z;
}

static void hook_act_render(void *self, void *canvas) {
    orig_act_render(self, canvas);
    __try { track_move(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (icons_off || !canvas) return;
    __try { draw_icons(canvas); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&icons_off, 1); n_shown = 0; log_line("ICONS disabled after a fault (code %08lx)", GetExceptionCode()); }
}

static int init_icons(HMODULE game, HMODULE engine) {
    void *act = GetProcAddress(game, "?Render@ActivityManager@GAME@@QEBAXAEAVGraphicsCanvas@2@@Z");
    g_game_engine = (void **)GetProcAddress(game, "?gGameEngine@GAME@@3PEAVGameEngine@1@EA");
    g_engine = (void **)GetProcAddress(engine, "?gEngine@GAME@@3PEAVEngine@1@EA");
    get_main_player = (void *(*)(void *))GetProcAddress(game, "?GetMainPlayer@GameEngine@GAME@@QEBAPEAVPlayer@2@XZ");
    get_game_camera = (void *(*)(void *))GetProcAddress(game, "?GetCamera@GameEngine@GAME@@QEAAPEAVGameCamera@2@XZ");
    cam_project = (Vec2 *(*)(void *, Vec2 *, const WorldVec3 *, const void *))GetProcAddress(engine, "?Project@WorldCamera@GAME@@QEBA?AVVec2@2@AEBVWorldVec3@2@AEBVViewport@2@@Z");
    canvas_viewport = (const void *(*)(void *))GetProcAddress(engine, "?GetViewport@GraphicsCanvas@GAME@@QEBAAEBVViewport@2@XZ");
    canvas_rect = (void (*)(void *, const Rect *, const Rect *, const void *, const Color *))GetProcAddress(engine, "?RenderRect@GraphicsCanvas@GAME@@QEAAXAEBVRect@2@0PEBVRenderTexture@2@AEBVColor@2@@Z");
    get_graphics = (void *(*)(void *))GetProcAddress(engine, "?GetGraphicsEngine@Engine@GAME@@QEBAPEAVGraphicsEngine@2@XZ");
    load_tex = (void *(*)(void *, const NStr *))GetProcAddress(engine, "?LoadTexture@GraphicsEngine@GAME@@QEAAPEBVGraphicsTexture@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
    tex_render = (const void *(*)(const void *))GetProcAddress(engine, "?GetTexture@GraphicsTexture@GAME@@QEBAPEBVRenderTexture@2@XZ");
    tex_rect = (Rect *(*)(const void *, Rect *))GetProcAddress(engine, "?GetRect@GraphicsTexture@GAME@@QEBA?AVRect@2@XZ");
    lore_has = (unsigned (*)(void *, const NStr *))GetProcAddress(game, "?GetItemInLoreCodex@Player@GAME@@QEBAIAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
    if (!act || !g_game_engine || !g_engine || !get_main_player || !get_game_camera || !cam_project || !canvas_viewport || !canvas_rect
        || !get_graphics || !load_tex || !tex_render || !tex_rect || !lore_has) { log_line("ICONS unavailable (missing export)"); return 0; }
    if (!load_spots()) { log_line("ICONS no lore_spots.txt"); return 0; }
    if (MH_CreateHook(act, (void *)hook_act_render, (void **)&orig_act_render) != MH_OK) { log_line("ICONS hook failed"); return 0; }
    log_line("ICONS ready spots=%d", n_spots);
    return 1;
}
