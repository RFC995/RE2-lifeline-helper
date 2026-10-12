/*
 * LifeLine Helper - cheats para Resident Evil 2 Remake (versão sem ray tracing / TDB66)
 *
 * Programa independente: não precisa de REFramework nem de mods.
 * Liga-se ao re2.exe em execução, desvia 3 funções do jogo para um pequeno bloco de código
 * (ver patches.c) e liga/desliga os cheats escrevendo flags nesse bloco.
 * Ao fechar o LifeLine Helper, o código original do jogo é reposto.
 *
 * Atalhos globais: F5 invencibilidade, F6 munição, F7 itens, F8 matar com um golpe.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "patches.h"
#include "version.h"

#define APP_TITLE L"LifeLine Helper " LLH_VERSION_WSTR
#define GAME_EXE  L"re2.exe"

/* Assinatura escrita no cave (offset 4..15) para reconhecer patches de uma sessão anterior. */
static const uint8_t CAVE_MAGIC[12] = { 'L', 'I', 'F', 'E', 'L', 'I', 'N', 'E', '-', 'v', '1', 0 };

/* --------------------------------------------------------------------------------------------- */

typedef LONG (NTAPI *NtSuspendResumeFn)(HANDLE);
static NtSuspendResumeFn pNtSuspendProcess, pNtResumeProcess;

typedef struct {
    int      flag;
    UINT     vk;
    const wchar_t *label;
    const wchar_t *key;
    HWND     hwnd;
    BOOL     on;
    BOOL     hotkey_ok;
} Cheat;

static Cheat g_cheats[FLAG_COUNT] = {
    { FLAG_GOD,   VK_F5, L"Invencibilidade",                     L"F5", NULL, FALSE, FALSE },
    { FLAG_AMMO,  VK_F6, L"Munição infinita (sem recarregar)",   L"F6", NULL, FALSE, FALSE },
    { FLAG_ITEMS, VK_F7, L"Itens infinitos (ervas, sprays, fitas, balas)", L"F7", NULL, FALSE, FALSE },
    { FLAG_OHK,   VK_F8, L"Matar com um golpe",                  L"F8", NULL, FALSE, FALSE },
};

static struct {
    HANDLE   process;
    DWORD    pid;
    uint64_t module_base;
    uint64_t cave;
    HookPatch patches[NUM_HOOKS];
    BOOL     attached;
    DWORD    mismatch_since;   /* GetTickCount de quando os bytes começaram a não bater certo */
} g;

static HWND g_status, g_topmost, g_main;
static HFONT g_font, g_font_bold, g_font_small;
static int g_dpi = 96;

#define S(x) MulDiv((x), g_dpi, 96)

enum { ID_CHEAT0 = 100, ID_ALL_OFF = 200, ID_TOPMOST = 201, ID_TIMER = 300, ID_HOTKEY0 = 400 };

/* --------------------------------------------------------------------------------------------- */

static void set_status(const wchar_t *fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    buf[511] = 0;
    va_end(ap);
    SetWindowTextW(g_status, buf);
}

static BOOL rpm(uint64_t addr, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(g.process, (LPCVOID)(uintptr_t)addr, out, n, &got) && got == n;
}

static BOOL wpm(uint64_t addr, const void *in, size_t n) {
    SIZE_T put = 0;
    if (!WriteProcessMemory(g.process, (LPVOID)(uintptr_t)addr, in, n, &put) || put != n) return FALSE;
    FlushInstructionCache(g.process, (LPCVOID)(uintptr_t)addr, n);
    return TRUE;
}

/* Escreve código: tira a proteção, escreve, repõe. */
static BOOL write_code(uint64_t addr, const void *in, size_t n) {
    DWORD old = 0, tmp = 0;
    if (!VirtualProtectEx(g.process, (LPVOID)(uintptr_t)addr, n, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    BOOL ok = wpm(addr, in, n);
    VirtualProtectEx(g.process, (LPVOID)(uintptr_t)addr, n, old, &tmp);
    return ok;
}

static void suspend_game(void) { if (pNtSuspendProcess) pNtSuspendProcess(g.process); }
static void resume_game(void)  { if (pNtResumeProcess)  pNtResumeProcess(g.process); }

static DWORD find_game_pid(void) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, GAME_EXE) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static BOOL find_module(DWORD pid, uint64_t *base, uint64_t *size) {
    BOOL found = FALSE;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, GAME_EXE) == 0) {
                *base = (uint64_t)(uintptr_t)me.modBaseAddr;
                *size = me.modBaseSize;
                found = TRUE;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

/* Reserva memória executável a menos de ~1,5 GB do módulo (para caber num jmp rel32). */
static uint64_t alloc_cave_near(uint64_t module_base, uint64_t module_size) {
    const uint64_t gran = 0x10000, range = 0x60000000ULL;
    uint64_t lo = module_base > range ? module_base - range : 0x10000;
    uint64_t hi = module_base + module_size + range;

    /* primeiro abaixo do módulo, depois acima */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t addr = pass == 0 ? ((module_base - gran) & ~(gran - 1)) : ((module_base + module_size + gran) & ~(gran - 1));
        while (pass == 0 ? addr > lo : addr < hi) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQueryEx(g.process, (LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi)) == 0) break;
            if (mbi.State == MEM_FREE) {
                void *p = VirtualAllocEx(g.process, (LPVOID)(uintptr_t)addr, CAVE_SIZE,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                if (p) return (uint64_t)(uintptr_t)p;
            }
            if (pass == 0) {
                uint64_t region_base = (uint64_t)(uintptr_t)mbi.AllocationBase;
                uint64_t next = (region_base && region_base < addr ? region_base : addr) - gran;
                addr = next & ~(gran - 1);
            } else {
                uint64_t next = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                addr = (next + gran - 1) & ~(gran - 1);
            }
        }
    }
    return 0;
}

static void write_flags(void) {
    if (!g.attached) return;
    uint8_t flags[FLAG_COUNT];
    for (int i = 0; i < FLAG_COUNT; i++) flags[i] = g_cheats[i].on ? 1 : 0;
    wpm(g.cave + CAVE_FLAGS_OFFSET, flags, sizeof(flags));
}

static void detach(BOOL restore) {
    if (g.process) {
        if (restore && g.attached && WaitForSingleObject(g.process, 0) == WAIT_TIMEOUT) {
            uint8_t zero[FLAG_COUNT] = { 0 };
            wpm(g.cave + CAVE_FLAGS_OFFSET, zero, sizeof(zero));
            suspend_game();
            for (int i = 0; i < NUM_HOOKS; i++)
                write_code(g.patches[i].target, g.patches[i].original, g.patches[i].len);
            resume_game();
            /* O cave fica alocado de propósito: alguma thread pode estar lá dentro neste instante. */
        }
        CloseHandle(g.process);
    }
    memset(&g, 0, sizeof(g));
}

typedef enum { SITE_ORIGINAL, SITE_OURS, SITE_UNKNOWN } SiteState;

static SiteState check_site(int i, uint64_t base, uint64_t *existing_cave) {
    const HookSite *s = &HOOK_SITES[i];
    uint8_t cur[32];
    if (!rpm(base + s->rva, cur, s->expected_len)) return SITE_UNKNOWN;
    if (memcmp(cur, s->expected, s->expected_len) == 0) return SITE_ORIGINAL;

    /* Já desviado por uma sessão anterior do LifeLine Helper? (jmp rel32 para um cave com a nossa assinatura) */
    if (cur[0] == 0xE9 && memcmp(cur + s->stolen_len, s->expected + s->stolen_len, s->expected_len - s->stolen_len) == 0) {
        int32_t rel;
        memcpy(&rel, cur + 1, 4);
        uint64_t dest = base + s->rva + 5 + (int64_t)rel;
        uint64_t cave = dest & ~0xFFFULL; /* o cave começa no início de uma página */
        uint8_t magic[sizeof(CAVE_MAGIC)];
        if (rpm(cave + 4, magic, sizeof(magic)) && memcmp(magic, CAVE_MAGIC, sizeof(magic)) == 0) {
            *existing_cave = cave;
            return SITE_OURS;
        }
    }
    return SITE_UNKNOWN;
}

static void try_attach(void) {
    DWORD pid = find_game_pid();
    if (!pid) {
        set_status(L"À espera do jogo... abre o Resident Evil 2.");
        return;
    }

    HANDLE h = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                           PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME | SYNCHRONIZE, FALSE, pid);
    if (!h) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            set_status(L"Sem permissão para aceder ao jogo. Fecha o LifeLine Helper e abre-o com \"Executar como administrador\".");
        else
            set_status(L"Não foi possível abrir o processo do jogo (erro %lu).", err);
        return;
    }
    g.process = h;
    g.pid = pid;

    uint64_t base = 0, size = 0;
    if (!find_module(pid, &base, &size) || size < 0x7000000) {
        set_status(L"Jogo encontrado, a aguardar que termine de arrancar...");
        CloseHandle(h);
        g.process = NULL;
        return;
    }
    g.module_base = base;

    /* Verificar os 3 sítios */
    int n_orig = 0, n_ours = 0;
    uint64_t existing = 0, cave_seen = 0;
    BOOL caves_differ = FALSE;
    for (int i = 0; i < NUM_HOOKS; i++) {
        SiteState st = check_site(i, base, &existing);
        if (st == SITE_ORIGINAL) n_orig++;
        else if (st == SITE_OURS) {
            n_ours++;
            if (cave_seen && cave_seen != existing) caves_differ = TRUE;
            cave_seen = existing;
        }
    }

    if (n_orig + n_ours != NUM_HOOKS || caves_differ) {
        DWORD now = GetTickCount();
        if (!g.mismatch_since) g.mismatch_since = now;
        CloseHandle(h);
        DWORD since = g.mismatch_since;
        memset(&g, 0, sizeof(g));
        g.mismatch_since = since;
        if (now - since > 30000)
            set_status(L"Esta versão do re2.exe não é a esperada (ou outro programa já alterou o jogo). "
                       L"O LifeLine Helper não vai mexer em nada.");
        else
            set_status(L"A verificar o jogo...");
        return;
    }
    g.mismatch_since = 0;

    if (n_ours == NUM_HOOKS) {
        /* Reaproveitar o cave da sessão anterior. */
        g.cave = cave_seen;
        uint8_t dummy[CAVE_SIZE];
        if (build_patches(base, g.cave, dummy, g.patches) != 0) {
            set_status(L"Erro interno ao reconstruir os patches.");
            detach(FALSE);
            return;
        }
        g.attached = TRUE;
        write_flags();
        set_status(L"Ligado ao RE2 (processo %lu). Escolhe os cheats abaixo.", pid);
        return;
    }

    if (n_ours != 0) {
        /* Estado misto (só alguns desviados): repor tudo o que for nosso não é seguro sem o cave; recusar. */
        set_status(L"O jogo está num estado inesperado. Reinicia o jogo e volta a abrir o LifeLine Helper.");
        detach(FALSE);
        return;
    }

    uint64_t cave = alloc_cave_near(base, size);
    if (!cave) {
        set_status(L"Não foi possível reservar memória perto do jogo.");
        detach(FALSE);
        return;
    }
    g.cave = cave;

    static uint8_t cave_bytes[CAVE_SIZE];
    int r = build_patches(base, cave, cave_bytes, g.patches);
    if (r != 0) {
        set_status(L"Erro interno ao preparar os patches (%d).", r);
        VirtualFreeEx(g.process, (LPVOID)(uintptr_t)cave, 0, MEM_RELEASE);
        detach(FALSE);
        return;
    }
    memcpy(cave_bytes + 4, CAVE_MAGIC, sizeof(CAVE_MAGIC));
    for (int i = 0; i < FLAG_COUNT; i++) cave_bytes[CAVE_FLAGS_OFFSET + i] = g_cheats[i].on ? 1 : 0;

    if (!wpm(cave, cave_bytes, CAVE_SIZE)) {
        set_status(L"Não foi possível escrever na memória do jogo.");
        VirtualFreeEx(g.process, (LPVOID)(uintptr_t)cave, 0, MEM_RELEASE);
        detach(FALSE);
        return;
    }

    suspend_game();
    BOOL ok = TRUE;
    int done = 0;
    for (int i = 0; i < NUM_HOOKS && ok; i++) {
        ok = write_code(g.patches[i].target, g.patches[i].patch, g.patches[i].len);
        if (ok) done++;
    }
    if (!ok) {
        for (int i = 0; i < done; i++)
            write_code(g.patches[i].target, g.patches[i].original, g.patches[i].len);
    }
    resume_game();

    if (!ok) {
        set_status(L"Não foi possível aplicar as alterações ao jogo.");
        detach(FALSE);
        return;
    }

    g.attached = TRUE;
    set_status(L"Ligado ao RE2 (processo %lu). Escolhe os cheats abaixo.", pid);
}

static void on_timer(void) {
    if (g.attached) {
        if (WaitForSingleObject(g.process, 0) != WAIT_TIMEOUT) {
            detach(FALSE);
            set_status(L"O jogo fechou. À espera que volte a abrir...");
        }
        return;
    }
    if (g.process) { CloseHandle(g.process); g.process = NULL; }
    try_attach();
}

static DWORD WINAPI beep_on_thread(LPVOID p)  { (void)p; Beep(1100, 70); Beep(1500, 90); return 0; }
static DWORD WINAPI beep_off_thread(LPVOID p) { (void)p; Beep(700, 70); Beep(450, 90); return 0; }

static void set_cheat(int i, BOOL on, BOOL from_hotkey) {
    g_cheats[i].on = on;
    SendMessageW(g_cheats[i].hwnd, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    write_flags();
    if (from_hotkey) {
        HANDLE t = CreateThread(NULL, 0, on ? beep_on_thread : beep_off_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
}

/* --------------------------------------------------------------------------------------------- */

static HWND add_label(HWND parent, const wchar_t *text, int x, int y, int w, int h, HFONT font) {
    HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, S(x), S(y), S(w), S(h),
                             parent, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static void create_controls(HWND hwnd) {
    add_label(hwnd, L"LifeLine Helper  ·  Resident Evil 2", 16, 12, 420, 26, g_font_bold);
    g_status = add_label(hwnd, L"A procurar o jogo...", 16, 42, 420, 52, g_font_small);

    int y = 100;
    for (int i = 0; i < FLAG_COUNT; i++) {
        wchar_t label[160];
        _snwprintf(label, 159, L"%ls   [%ls]", g_cheats[i].label, g_cheats[i].key);
        label[159] = 0;
        g_cheats[i].hwnd = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                           S(16), S(y), S(420), S(24), hwnd, (HMENU)(INT_PTR)(ID_CHEAT0 + i), NULL, NULL);
        SendMessageW(g_cheats[i].hwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
        y += 30;
    }

    y += 6;
    HWND all_off = CreateWindowExW(0, L"BUTTON", L"Desligar tudo", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   S(16), S(y), S(130), S(28), hwnd, (HMENU)ID_ALL_OFF, NULL, NULL);
    SendMessageW(all_off, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_topmost = CreateWindowExW(0, L"BUTTON", L"Sempre visível", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                S(170), S(y + 3), S(240), S(24), hwnd, (HMENU)ID_TOPMOST, NULL, NULL);
    SendMessageW(g_topmost, WM_SETFONT, (WPARAM)g_font, TRUE);

    y += 42;
    add_label(hwnd, L"Os atalhos F5 a F8 funcionam com o jogo aberto à frente (ouve-se um bip).\n"
                    L"Ao fechar esta janela, o jogo volta ao normal.", 16, y, 420, 40, g_font_small);
}

static void register_hotkeys(HWND hwnd) {
    wchar_t missing[64] = L"";
    for (int i = 0; i < FLAG_COUNT; i++) {
        g_cheats[i].hotkey_ok = RegisterHotKey(hwnd, ID_HOTKEY0 + i, 0x4000 /* MOD_NOREPEAT */, g_cheats[i].vk);
        if (!g_cheats[i].hotkey_ok) {
            wcsncat(missing, L" ", 63 - wcslen(missing));
            wcsncat(missing, g_cheats[i].key, 63 - wcslen(missing));
        }
    }
    if (missing[0]) {
        wchar_t msg[200];
        _snwprintf(msg, 199, L"Atenção: outro programa já usa a(s) tecla(s)%ls. Usa as caixas na janela.", missing);
        msg[199] = 0;
        MessageBoxW(hwnd, msg, APP_TITLE, MB_OK | MB_ICONINFORMATION);
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd);
        register_hotkeys(hwnd);
        SetTimer(hwnd, ID_TIMER, 1000, NULL);
        try_attach();
        return 0;

    case WM_TIMER:
        if (wp == ID_TIMER) on_timer();
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_CHEAT0 && id < ID_CHEAT0 + FLAG_COUNT) {
            int i = id - ID_CHEAT0;
            set_cheat(i, SendMessageW(g_cheats[i].hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED, FALSE);
        } else if (id == ID_ALL_OFF) {
            for (int i = 0; i < FLAG_COUNT; i++) set_cheat(i, FALSE, FALSE);
        } else if (id == ID_TOPMOST) {
            BOOL top = SendMessageW(g_topmost, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SetWindowPos(hwnd, top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        return 0;
    }

    case WM_HOTKEY: {
        int i = (int)wp - ID_HOTKEY0;
        if (i >= 0 && i < FLAG_COUNT) set_cheat(i, !g_cheats[i].on, TRUE);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        if ((HWND)lp == g_status) SetTextColor(dc, g.attached ? RGB(0, 120, 40) : RGB(150, 90, 0));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        for (int i = 0; i < FLAG_COUNT; i++) UnregisterHotKey(hwnd, ID_HOTKEY0 + i);
        detach(TRUE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd;

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"LifeLineHelper.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"LifeLineHelperWnd", NULL);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }

    SetProcessDPIAware();
    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    pNtSuspendProcess = (NtSuspendResumeFn)(void *)GetProcAddress(ntdll, "NtSuspendProcess");
    pNtResumeProcess  = (NtSuspendResumeFn)(void *)GetProcAddress(ntdll, "NtResumeProcess");

    g_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_bold = CreateFontW(-S(19), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_small = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"LifeLineHelperWnd";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, S(452), S(392) };
    AdjustWindowRect(&rc, style, FALSE);
    g_main = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_main, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
