/* native-lorevo.dll - Korean lore-note narration for Grim Dawn (own player, no third-party files).
   Loaded by our dinput8.dll proxy from x64\GrimDawnKoreanVoice\.
   Trigger: LocalizationManager::LocalizeWithoutParams for a note's BODY text tag (itemText, "...Text"),
   which the game looks up when a note is actually read or opened in the codex. Title/label tags are ignored,
   so walking past a note lying in the world does not start playback. A tag counts as a new display only
   after 1.5 s without lookups (UI that re-reads the text every frame does not restart it).
   Stops on: another note, starting a conversation (ControllerPlayerStateTalkToNpc::OnBegin), the codex key J.
   Volume follows the game's master x dialog volume from options.txt. */
#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <shlwapi.h>
#include "MinHook.h"

typedef const wchar_t *(*LocalizeFn)(void *self, const char *tag);
typedef void (*OnBeginFn)(void *self);
typedef MMRESULT (WINAPI *OpenFn)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *HdrFn)(HWAVEOUT, LPWAVEHDR, UINT);
typedef MMRESULT (WINAPI *HFn)(HWAVEOUT);

typedef struct { char tag[112]; wchar_t path[MAX_PATH]; DWORD last; int known, canon; volatile LONG title_tick; } Entry;   /* canon: first entry with the same file (title aliases share state with the body tag) */   /* known: in this character's codex */

static LocalizeFn orig_localize;
static OnBeginFn orig_onbegin;
static OpenFn wo_open; static HdrFn wo_prepare, wo_unprepare, wo_write; static HFn wo_reset, wo_close;
static volatile LONG state;                    /* 0 starting, 1 waiting for game, 2 active, 3 disabled */
static volatile LONG pending = -1, stop_req;
static Entry *entries; static int n_entries;
static wchar_t log_path[MAX_PATH], lore_dir[MAX_PATH];
static HWAVEOUT wave; static WAVEHDR hdr; static char *pcm; static int playing = -1;
static volatile LONG click_tick;                 /* last left click while the game had focus */
static volatile LONG journal_key_tick;           /* last time Q/J (journal) went down while the game had focus */

static void log_line(const char *fmt, ...) {
    FILE *f; va_list a; SYSTEMTIME t;
    if (!log_path[0] || !(f = _wfopen(log_path, L"a"))) return;
    GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

static int cmp_entry(const void *a, const void *b) { return _stricmp(((const Entry *)a)->tag, ((const Entry *)b)->tag); }

static int find(const char *tag) {
    int lo = 0, hi = n_entries - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = _stricmp(tag, entries[mid].tag);
        if (!c) return entries[mid].canon;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

static volatile LONG title_last, title_n, rebuild_tick;   /* journal list (re)build = a quick burst of note TITLE lookups */
static volatile LONG delay_i = -1, delay_tick;      /* clicked note waiting to be sure it is not a pickup */
static int title_prev = -1;
/* An NPC conversation (or a quest it completes) looks up the text of notes it refers to, often right as the player
   clicks: not a reading. talk_active: ControllerPlayerStateTalkToNpc between OnBegin and OnEnd. */
static volatile LONG talk_active, talk_end_tick, approach_tick;
static void *delay_stack[6]; static USHORT delay_frames;   /* who looked the text up (diagnostics in the log) */
static volatile LONG delay_journal;
#define JOURNAL_PAGE_RET 0x168aec   /* Grim Dawn.exe 1.3.1.1: return address of the journal note page's text lookup */

static void stack_text(char *out, size_t cap) {
    int k; size_t n = 0; out[0] = 0;
    for (k = 0; k < delay_frames && n + 40 < cap; k++) {
        HMODULE m = NULL; wchar_t name[MAX_PATH], *b;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)delay_stack[k], &m)
            && GetModuleFileNameW(m, name, MAX_PATH)) {
            b = wcsrchr(name, L'\\'); b = b ? b + 1 : name;
            n += sprintf_s(out + n, cap - n, " %.6ls+%llx", b, (unsigned long long)((char *)delay_stack[k] - (char *)m));
        } else n += sprintf_s(out + n, cap - n, " %p", delay_stack[k]);
    }
}

static int has_suffix(const char *s, size_t L, const char *suf) {
    size_t n = strlen(suf);
    return L >= n && _stricmp(s + L - n, suf) == 0;
}

static int is_title(const char *tag, size_t L) {
    return (has_suffix(tag, L, "name") || has_suffix(tag, L, "codextitle") || has_suffix(tag, L, "name_codex"))
        && (StrStrIA(tag, "loreobj") || StrStrIA(tag, "questitem"));
}

/* Codex: clicking an entry looks up that note's body text (and its own title for the header). Opening the journal
   (Q/J) or a refresh after a quest update rebuilds the whole list (a burst of title lookups, only notes the character
   owns) and then re-shows the last note: ignore that, but remember every listed note as owned ("known"). */
static const wchar_t *hook_localize(void *self, const char *tag) {
    const wchar_t *r = orig_localize(self, tag);
    size_t L;
    if (!tag || (L = strlen(tag)) < 6 || L >= 112) return r;
    if (is_title(tag, L)) {
        DWORD now = GetTickCount(); int k = find(tag);
        if (k >= 0) InterlockedExchange(&entries[k].title_tick, (LONG)now);   /* an opened note shows its own title too */
        if (now - (DWORD)title_last < 150) {
            if (InterlockedIncrement(&title_n) >= 3) InterlockedExchange(&rebuild_tick, (LONG)now);
            if (k >= 0) entries[k].known = 1;
            if (title_prev >= 0) entries[title_prev].known = 1;
        } else InterlockedExchange(&title_n, 1);
        title_prev = k;
        InterlockedExchange(&title_last, (LONG)now);
    } else if (has_suffix(tag, L, "text")) {
        int i = find(tag);
        if (i >= 0) {
            DWORD now = GetTickCount();
            int rebuild = now - (DWORD)rebuild_tick < 400;
            int journal = (GetAsyncKeyState('Q') & 0x8000) || (GetAsyncKeyState('J') & 0x8000) || now - (DWORD)journal_key_tick < 800;
            /* only a note this character owns, opened by a click (codex entry). A note lying in the world
               (tooltip, nearby) is not in the codex yet. */
            int clicked = now - (DWORD)click_tick < 700;
            (void)rebuild;
            if (now - entries[i].last > 1500 && playing != i) {
                /* a click opened this note (journal entry or the note in the inventory). Wait 600 ms: if the game adds
                   the same note to the codex meanwhile, the click was a PICKUP from the ground - stay silent. */
                if (!journal && clicked) {
                    int k; char *exe = (char *)GetModuleHandleW(NULL);
                    delay_frames = RtlCaptureStackBackTrace(1, 6, delay_stack, NULL);
                    for (delay_journal = 0, k = 0; k < delay_frames; k++)
                        if ((char *)delay_stack[k] == exe + JOURNAL_PAGE_RET) delay_journal = 1;
                    InterlockedExchange(&delay_i, i); InterlockedExchange(&delay_tick, (LONG)now);
                }
                else if (clicked) log_line("SKIP %s journal=%d", entries[i].tag, journal);
            }
            entries[i].last = now;
        }
    }
    return r;
}

/* Reading a note directly (right-click in the inventory, picking it up): ItemNote::Use and
   Player::AddItemToLoreCodex. The note is found through its codex title tag (std::string at +0xca8). */
typedef void (*UseFn)(void *note, void *character);
typedef unsigned char (*AddFn)(void *player, void *note);
typedef const void *(*TitleFn)(const void *note);
static UseFn orig_use; static AddFn orig_add; static TitleFn note_title;
static volatile LONG add_tick, add_count, add_first = -1, add_new, pickup = -1;

static int note_entry(void *note) {
    const unsigned char *s; const char *t; size_t len; char base[112], key[128]; int i;
    if (!note || !note_title || !(s = (const unsigned char *)note_title(note))) return -1;
    len = *(const size_t *)(s + 16);
    t = *(const size_t *)(s + 24) >= 16 ? *(const char *const *)s : (const char *)s;   /* MSVC std::string */
    if (!len || len >= sizeof base - 8) return -1;
    memcpy(base, t, len); base[len] = 0;
    if ((i = find(base)) >= 0) return i;              /* exact title tag listed in lore_voice_ko.txt */
    if (has_suffix(base, len, "_codextitle")) base[len - 11] = 0;
    else if (has_suffix(base, len, "codextitle")) base[len - 10] = 0;
    else if (has_suffix(base, len, "_name")) base[len - 5] = 0;
    else if (has_suffix(base, len, "name")) base[len - 4] = 0;
    sprintf_s(key, sizeof key, "%s_text", base); if ((i = find(key)) >= 0) return i;
    sprintf_s(key, sizeof key, "%stext", base); if ((i = find(key)) >= 0) return i;
    log_line("NOTE WITHOUT VOICE %s", base);
    return -1;
}

static void read_note(void *note, const char *how) {
    int i = note_entry(note);
    if (i < 0) return;
    entries[i].last = GetTickCount();                 /* the journal page that follows must not restart it */
    entries[i].known = 1;
    InterlockedExchange(&pickup, -1);                  /* Use() adds the note to the codex itself: play once */
    InterlockedExchange(&pending, i);
    log_line("%s %s", how, entries[i].tag);
}

static void hook_use(void *note, void *character) {
    orig_use(note, character);
    read_note(note, "READ");
}

static unsigned char hook_add(void *player, void *note) {
    unsigned char added = orig_add(player, note);
    DWORD now = GetTickCount();
    int i = note_entry(note);
    /* a character load re-adds every known note in one burst (many different notes): not a pickup.
       A real pickup adds one note (the game may call this twice for it). */
    if (now - (DWORD)add_tick > 500) { InterlockedExchange(&add_count, 0); InterlockedExchange(&add_first, i); InterlockedExchange(&add_new, 0); }
    InterlockedExchange(&add_tick, (LONG)now);
    if (i != add_first) InterlockedIncrement(&add_count);               /* a different note in the same burst */
    if (added) InterlockedExchange(&add_new, 1);
    if (i >= 0) {
        entries[i].known = 1;
        if (delay_i == i) { InterlockedExchange(&delay_i, -1); log_line("PICKUP-CLICK %s (silent)", entries[i].tag); }
        if (add_count == 0 && add_new) InterlockedExchange(&pickup, i);    /* played by the worker after 400 ms */
    }
    return added;
}

static volatile LONG talk_begin_tick;
static void hook_onbegin(void *self) {
    InterlockedExchange(&talk_active, 1);
    InterlockedExchange(&talk_begin_tick, (LONG)GetTickCount());
    InterlockedExchange(&stop_req, 1);
    orig_onbegin(self);
}

/* TalkToNpc::OnEnd is folded with ~1500 empty functions in Game.dll (it fires every frame): a conversation is over
   when the player controller enters another state instead (Idle / MoveTo / MoveToActorBase). */
static OnBeginFn orig_idle, orig_moveto, orig_approach;
static void talk_over(void) {
    if (InterlockedExchange(&talk_active, 0)) InterlockedExchange(&talk_end_tick, (LONG)GetTickCount());
}
static void hook_idle(void *self) { talk_over(); orig_idle(self); }
static void hook_moveto(void *self) { talk_over(); orig_moveto(self); }
static void hook_approach(void *self) {                 /* walking up to an NPC / object the player clicked */
    talk_over();
    InterlockedExchange(&approach_tick, (LONG)GetTickCount());
    orig_approach(self);
}

static float game_volume(void) {
    wchar_t path[MAX_PATH]; FILE *f; char line[256]; float master = 1, dialog = 1, v;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, path))) return 1;
    wcscat_s(path, MAX_PATH, L"\\My Games\\Grim Dawn\\Settings\\options.txt");
    if (!(f = _wfopen(path, L"r"))) return 1;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " masterVolume = %f", &v) == 1) master = v;
        else if (sscanf(line, " dialogVolume = %f", &v) == 1) dialog = v;
    }
    fclose(f);
    v = master * dialog;
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float lore_boost(void) {                     /* optional mods/lore_audio/lore_volume.txt: one number, e.g. 1.3 */
    static float b = -1; wchar_t path[MAX_PATH]; FILE *f;
    if (b >= 0) return b;
    b = 1.0f;
    swprintf_s(path, MAX_PATH, L"%ls\\lore_volume.txt", lore_dir);
    if ((f = _wfopen(path, L"r"))) { if (fscanf(f, "%f", &b) != 1 || b < 0 || b > 4) b = 1.0f; fclose(f); }   /* 0 = notes off */
    return b;
}

static void stop(void) {
    if (!wave) return;
    wo_reset(wave); wo_unprepare(wave, &hdr, sizeof hdr); wo_close(wave);
    wave = NULL; free(pcm); pcm = NULL;
    if (playing >= 0) log_line("STOP %s", entries[playing].tag);
    playing = -1;
}

static float lore_boost(void);
static void play(int i) {
    HANDLE h; DWORD size, got; unsigned char *d; WAVEFORMATEX fmt; DWORD p = 12, data_off = 0, data_len = 0;
    float vol; DWORD k;
    stop();
    if (lore_boost() == 0.0f) { log_line("MUTED %s", entries[i].tag); return; }
    h = CreateFileW(entries[i].path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { log_line("MISSING %s", entries[i].tag); return; }
    size = GetFileSize(h, NULL); d = (unsigned char *)malloc(size ? size : 1);
    if (!d || !ReadFile(h, d, size, &got, NULL) || got != size || size < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) {
        CloseHandle(h); free(d); log_line("BAD WAV %s", entries[i].tag); return;
    }
    CloseHandle(h);
    memset(&fmt, 0, sizeof fmt);
    while (p + 8 <= size) {                            /* walk RIFF chunks: fmt + data */
        DWORD len = *(DWORD *)(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4) && len >= 16) memcpy(&fmt, d + p + 8, 16);
        else if (!memcmp(d + p, "data", 4)) { data_off = p + 8; data_len = min(len, size - p - 8); break; }
        p += 8 + len + (len & 1);
    }
    if (fmt.wFormatTag != WAVE_FORMAT_PCM || fmt.wBitsPerSample != 16 || !data_len) { free(d); log_line("UNSUPPORTED %s", entries[i].tag); return; }
    pcm = (char *)malloc(data_len); memcpy(pcm, d + data_off, data_len); free(d);
    vol = game_volume();
    {   /* notes are read while fighting in the field: the recordings sit ~7 dB under NPC dialogue and drowned in
           combat. Lift them (~+9 dB, x user factor from lore_volume.txt) and soft-limit the peaks so nothing clips. */
        float g = vol * 2.8f * lore_boost(), x, ax;
        for (k = 0; k < data_len / 2; k++) {
            x = ((short *)pcm)[k] / 32768.0f * g; ax = x < 0 ? -x : x;
            if (ax > 0.7f) { ax = 0.7f + 0.29f * tanhf((ax - 0.7f) / 0.29f); x = x < 0 ? -ax : ax; }
            ((short *)pcm)[k] = (short)(x * 32767.0f);
        }
    }
    if (wo_open(&wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { free(pcm); pcm = NULL; wave = NULL; log_line("WAVEOUT FAILED"); return; }
    memset(&hdr, 0, sizeof hdr); hdr.lpData = pcm; hdr.dwBufferLength = data_len;
    wo_prepare(wave, &hdr, sizeof hdr); wo_write(wave, &hdr, sizeof hdr);
    playing = i;
    log_line("PLAY %s vol=%.2f boost=%.2f", entries[i].tag, vol, 2.8f * lore_boost());
}

static int game_has_focus(void) {
    DWORD pid = 0; HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static int load_map(void) {
    wchar_t path[MAX_PATH]; FILE *f; char line[512]; int cap = 512;
    swprintf_s(path, MAX_PATH, L"%ls\\lore_voice_ko.txt", lore_dir);
    if (!(f = _wfopen(path, L"r"))) return 0;
    entries = (Entry *)calloc(cap, sizeof(Entry));
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t'), *e; wchar_t rel[MAX_PATH]; int j;
        if (!tab) continue;
        *tab = 0; e = tab + 1 + strcspn(tab + 1, "\r\n"); *e = 0;
        if (strlen(line) >= sizeof entries[0].tag) continue;
        if (n_entries == cap) { cap *= 2; entries = (Entry *)realloc(entries, cap * sizeof(Entry)); }
        memset(&entries[n_entries], 0, sizeof(Entry));
        strcpy_s(entries[n_entries].tag, sizeof entries[0].tag, line);
        MultiByteToWideChar(CP_UTF8, 0, tab + 1, -1, rel, MAX_PATH);
        for (j = 0; rel[j]; j++) if (rel[j] == L'/') rel[j] = L'\\';
        swprintf_s(entries[n_entries].path, MAX_PATH, L"%ls\\%ls", lore_dir, rel);
        n_entries++;
    }
    fclose(f);
    qsort(entries, n_entries, sizeof(Entry), cmp_entry);
    {   int a, b;
        for (a = 0; a < n_entries; a++) {
            entries[a].canon = a;
            for (b = 0; b < a; b++) if (!_wcsicmp(entries[a].path, entries[b].path)) { entries[a].canon = entries[b].canon; break; }
        }
    }
    return n_entries;
}

#include "note_icons.inc"

static DWORD WINAPI worker(LPVOID self) {
    wchar_t dir[MAX_PATH], *slash; HMODULE engine, game, wm; void *loc, *onb; int waited, key_was = 0;
    GetModuleFileNameW((HMODULE)self, dir, MAX_PATH); slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
    swprintf_s(log_path, MAX_PATH, L"%ls\\lore-voice.log", dir);
    GetModuleFileNameW(NULL, dir, MAX_PATH); slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;   /* ...\x64 */
    slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;                                             /* game root */
    swprintf_s(lore_dir, MAX_PATH, L"%ls\\mods\\lore_audio", dir);
    state = 1;
    for (waited = 0; !(GetModuleHandleW(L"Engine.dll") && GetModuleHandleW(L"Game.dll")); waited++) {
        if (waited > 1200) { state = 3; log_line("DISABLED game modules never loaded"); return 0; }
        Sleep(100);
    }
    engine = GetModuleHandleW(L"Engine.dll"); game = GetModuleHandleW(L"Game.dll");
    if (!load_map()) { state = 3; log_line("DISABLED no lore_voice_ko.txt in mods\\lore_audio"); return 0; }
    {   wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); wcscat_s(sys, MAX_PATH, L"\\winmm.dll");
        wm = LoadLibraryW(sys); }                     /* the system copy, never a winmm.dll proxy in x64 */
    if (!wm) { state = 3; log_line("DISABLED winmm"); return 0; }
    wo_open = (OpenFn)GetProcAddress(wm, "waveOutOpen"); wo_prepare = (HdrFn)GetProcAddress(wm, "waveOutPrepareHeader");
    wo_unprepare = (HdrFn)GetProcAddress(wm, "waveOutUnprepareHeader"); wo_write = (HdrFn)GetProcAddress(wm, "waveOutWrite");
    wo_reset = (HFn)GetProcAddress(wm, "waveOutReset"); wo_close = (HFn)GetProcAddress(wm, "waveOutClose");
    loc = GetProcAddress(engine, "?LocalizeWithoutParams@LocalizationManager@GAME@@QEAAPEBGPEBD@Z");
    onb = GetProcAddress(game, "?OnBegin@ControllerPlayerStateTalkToNpc@GAME@@UEAAXXZ");
    if (!loc || !wo_open || MH_Initialize() != MH_OK || MH_CreateHook(loc, (void *)hook_localize, (void **)&orig_localize) != MH_OK) {
        state = 3; log_line("DISABLED hooks (localize=%p)", loc); return 0;
    }
    if (onb && MH_CreateHook(onb, (void *)hook_onbegin, (void **)&orig_onbegin) != MH_OK) onb = NULL;
    {   void *idle = GetProcAddress(game, "?OnBegin@ControllerPlayerStateIdle@GAME@@UEAAXXZ");
        void *mov = GetProcAddress(game, "?OnBegin@ControllerPlayerStateMoveTo@GAME@@UEAAXXZ");
        void *app = GetProcAddress(game, "?OnBegin@ControllerPlayerStateMoveToActorBase@GAME@@UEAAXXZ");
        if (idle) MH_CreateHook(idle, (void *)hook_idle, (void **)&orig_idle);
        if (mov) MH_CreateHook(mov, (void *)hook_moveto, (void **)&orig_moveto);
        if (app) MH_CreateHook(app, (void *)hook_approach, (void **)&orig_approach);
        log_line("talk hooks: idle=%d move=%d approach=%d", orig_idle != NULL, orig_moveto != NULL, orig_approach != NULL);
    }
    {   void *use = GetProcAddress(game, "?Use@ItemNote@GAME@@UEAAXPEAVCharacter@2@@Z");
        void *add = GetProcAddress(game, "?AddItemToLoreCodex@Player@GAME@@QEAA_NPEAVItemNote@2@@Z");
        note_title = (TitleFn)GetProcAddress(game, "?GetCodexTitleTag@ItemNote@GAME@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ");
        if (note_title && use) MH_CreateHook(use, (void *)hook_use, (void **)&orig_use);
        if (note_title && add) MH_CreateHook(add, (void *)hook_add, (void **)&orig_add);
        log_line("note hooks: use=%d pickup=%d title=%d", orig_use != NULL, orig_add != NULL, note_title != NULL);
    }
    init_icons(game, engine);
    MH_EnableHook(MH_ALL_HOOKS);
    state = 2;
    log_line("READY notes=%d talk-stop=%d dir=%ls", n_entries, onb != NULL, lore_dir);
    for (;;) {
        int i, key;
        Sleep(30);
        if (pickup >= 0 && GetTickCount() - (DWORD)add_tick > 400) {      /* a lone pickup, not a load burst */
            int p = InterlockedExchange(&pickup, -1);
            if (p >= 0 && add_count == 0) log_line("PICKUP %s (silent: plays only when opened)", entries[p].tag);
        }
        if (delay_i >= 0 && GetTickCount() - (DWORD)delay_tick > 600) {
            int d = InterlockedExchange(&delay_i, -1);
            /* Only the journal's note page counts: it looks the text up from one place in Grim Dawn.exe (seen in the log
               2026-10-26, 10 clicks). Other callers are a note's tooltip (ItemNote::GetUIDisplayText), NPCs and quests.
               A quest finished in a conversation refreshes the journal page too: not right after a conversation. */
            LONG now = (LONG)GetTickCount(), since_end = now - (LONG)talk_end_tick;
            int talking = talk_active && !(last_move_tick - talk_begin_tick > 1500);   /* moved since: the end was missed */
            char who[320]; stack_text(who, sizeof who);
            if (d < 0) ;
            else if (!delay_journal)
                log_line("SKIP %s (not the journal)%s", entries[d].tag, who);
            else if (talking || (talk_end_tick && since_end < 3000))
                log_line("SKIP %s (conversation: talking=%d, ended %ld ms ago)%s", entries[d].tag, talking, (long)since_end, who);
            else { log_line("OPEN %s%s", entries[d].tag, who); InterlockedExchange(&pending, d); }
        }
        i = InterlockedExchange(&pending, -1);
        if (i >= 0) { InterlockedExchange(&stop_req, 0); play(i); }
        if (InterlockedExchange(&stop_req, 0) && wave) { log_line("conversation started"); stop(); }
        if (game_has_focus() && ((GetAsyncKeyState(VK_LBUTTON) & 0x8001) || (GetAsyncKeyState(VK_RBUTTON) & 0x8001)))
            InterlockedExchange(&click_tick, (LONG)GetTickCount());
        key = game_has_focus() && ((GetAsyncKeyState('J') & 0x8000) || (GetAsyncKeyState('Q') & 0x8000));
        if (key && !key_was) {
            InterlockedExchange(&journal_key_tick, (LONG)GetTickCount());
            if (GetAsyncKeyState('J') & 0x8000 && wave) { log_line("codex key"); stop(); }
        }
        key_was = key;
        if (wave && (hdr.dwFlags & WHDR_DONE)) stop();
    }
}

__declspec(dllexport) DWORD WINAPI LoreStatus(void *unused) { return (DWORD)state; }

BOOL WINAPI DllMain(HINSTANCE dll, DWORD why, LPVOID reserved) {
    if (why == DLL_PROCESS_ATTACH) {
        HANDLE t; DisableThreadLibraryCalls(dll);
        t = CreateThread(NULL, 0, worker, dll, 0, NULL); if (t) CloseHandle(t);
    }
    return TRUE;
}
