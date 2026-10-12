/* LifeLine Helper - construção dos patches. Ver patches.h. */
#include "patches.h"
#include <string.h>
#include <stdarg.h>

#define TYPE_HIT_POINT_CONTROLLER       0x8724u  /* app.ropeway.HitPointController (jogador e sobreviventes) */
#define TYPE_ENEMY_HIT_POINT_CONTROLLER 0xD525u  /* app.ropeway.EnemyHitPointController */
#define TYPE_EM6200_THINK               0xC17Cu  /* app.ropeway.enemy.em6200.Em6200Think (Mr. X) */

/* Mr. X (Em6200Think): funções chamadas e campos usados pela reposição do chapéu */
#define RVA_CHECK_FLAG    0x10AD7F0u  /* GlobalUserDataManager.checkFlag(System.Guid) -> BoolResult */
#define RVA_SET_HAT_DISP  0x13AE400u  /* Em6200Think.setHatDisp(bool)                               */
#define RVA_GUD_MANAGER   0x709EBD8u  /* ponteiro global para a instância do GlobalUserDataManager    */
#define BOOL_RESULT_TRUE  2           /* GlobalUserDataManager.BoolResult.TRUE                        */

const HookSite HOOK_SITES[NUM_HOOKS] = {
    {
        "HitPointController.addDamage", 0xF9A080, 7,
        /* mov rax,[rcx+50h] ; mov r9,rdx ; cmp qword [rax+18h],0 ; jne ... ; cmp byte [rdx+5Dh],0 ... */
        { 0x48, 0x8B, 0x41, 0x50, 0x4C, 0x8B, 0xCA, 0x48, 0x83, 0x78, 0x18, 0x00, 0x75, 0x1E, 0x80, 0x7A,
          0x5D, 0x00, 0x75, 0x18, 0x8B, 0x4A, 0x58, 0x41, 0x8B, 0xC0, 0x99, 0x33, 0xC2, 0x2B, 0xC2, 0x2B }, 32
    },
    {
        "HitPointController.checkDead", 0xF99DC0, 9,
        /* mov rax,[rcx+50h] ; cmp qword [rax+18h],0 ; jne ... ; cmp byte [rdx+5Dh],0 ... */
        { 0x48, 0x8B, 0x41, 0x50, 0x48, 0x83, 0x78, 0x18, 0x00, 0x75, 0x2E, 0x80, 0x7A, 0x5D, 0x00, 0x75,
          0x28, 0x80, 0x7A, 0x5C, 0x00, 0x75, 0x09, 0x83, 0x7A, 0x58, 0x00, 0x0F, 0x9E, 0xC0, 0xEB, 0x02 }, 32
    },
    {
        "inventory.Slot.reduce", 0x51E620, 5,
        /* mov [rsp+10h],rbp ; mov [rsp+18h],rsi ; push rdi ; sub rsp,20h ; mov rax,[rcx+50h] ... */
        { 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
          0x8B, 0x41, 0x50, 0x41, 0x8B, 0xF8, 0x48, 0x8B, 0xEA, 0x48, 0x8B, 0xF1, 0x48, 0x83, 0x78, 0x18 }, 32
    },
    {
        "em6200.Em6200Think.onThinkUpdate", 0x13A35E0, 6,
        /* push rbp ; push rsi ; push rdi ; push r14 ; lea rbp,[rsp-3Fh] ; sub rsp,88h ; xor r14d,r14d ... */
        { 0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xC1, 0x48, 0x81, 0xEC, 0x88, 0x00,
          0x00, 0x00, 0x45, 0x33, 0xF6, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x4A, 0x10, 0x48, 0x8B, 0xF2, 0x4C }, 32
    },
};

const HookSite VERIFY_SITES[NUM_VERIFY] = {
    {
        "GlobalUserDataManager.checkFlag(Guid)", RVA_CHECK_FLAG, 0,
        { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
          0x89, 0x7C, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B }, 32
    },
    {
        "em6200.Em6200Think.setHatDisp", RVA_SET_HAT_DISP, 0,
        { 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x68, 0x48, 0x8B, 0x52, 0x10, 0x41, 0x0F, 0xB6, 0xD8, 0x48,
          0x8B, 0xF9, 0x48, 0x85, 0xD2, 0x75, 0x0E, 0x45, 0x33, 0xC0, 0x41, 0x8D, 0x50, 0x46, 0xE8, 0xED }, 32
    },
    {
        /* mov r9,[rip+5CFA909h] em onThinkUpdate: confirma o RVA do ponteiro global RVA_GUD_MANAGER */
        "onThinkUpdate: mov r9, [GlobalUserDataManager]", 0x13A42C8, 0,
        { 0x4C, 0x8B, 0x0D, 0x09, 0xA9, 0xCF, 0x05 }, 7
    },
};

/* ------------------------------------------------------------------ emissor mínimo */

typedef struct {
    uint8_t *buf;
    uint64_t base;   /* endereço do buf[0] no processo do jogo */
    size_t   pos;
    int      error;
} Emit;

static void b(Emit *e, int n, ...) {
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        int v = va_arg(ap, int);
        if (e->pos < CAVE_SIZE) e->buf[e->pos] = (uint8_t)v;
        e->pos++;
    }
    va_end(ap);
    if (e->pos > CAVE_SIZE) e->error = -1;
}

static void u32(Emit *e, uint32_t v) {
    b(e, 4, v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
}

static uint64_t here(Emit *e) { return e->base + e->pos; }

static int fits_rel32(int64_t d) { return d >= INT32_MIN && d <= INT32_MAX; }

/* jmp rel32 para um endereço absoluto */
static void jmp_abs(Emit *e, uint64_t target) {
    int64_t rel = (int64_t)(target - (here(e) + 5));
    if (!fits_rel32(rel)) e->error = -2;
    b(e, 1, 0xE9);
    u32(e, (uint32_t)(int32_t)rel);
}

/* cmp byte ptr [rip + flag], 0 */
static void cmp_flag(Emit *e, uint64_t cave_base, int flag) {
    uint64_t flag_addr = cave_base + CAVE_FLAGS_OFFSET + (uint64_t)flag;
    int64_t rel = (int64_t)(flag_addr - (here(e) + 7));
    b(e, 2, 0x80, 0x3D);
    u32(e, (uint32_t)(int32_t)rel);
    b(e, 1, 0x00);
}

/* Saltos curtos para a frente: emite o opcode + placeholder e devolve a posição a corrigir. */
static size_t jcc8(Emit *e, int opcode) {
    b(e, 2, opcode, 0x00);
    return e->pos - 1;
}

static void land(Emit *e, size_t fix) {
    int64_t rel = (int64_t)e->pos - (int64_t)(fix + 1);
    if (rel < -128 || rel > 127) e->error = -3;
    e->buf[fix] = (uint8_t)(int8_t)rel;
}

/* Salto curto para trás, para uma posição já emitida. */
static void jcc8_back(Emit *e, int opcode, size_t target) {
    int64_t rel = (int64_t)target - (int64_t)(e->pos + 2);
    if (rel < -128 || rel > 127) e->error = -3;
    b(e, 2, opcode, (int)(uint8_t)(int8_t)rel);
}

/* cmp/mov byte ptr [rip + addr], imm8 */
static void rip_byte(Emit *e, int op1, int op2, uint64_t addr, int imm) {
    int64_t rel = (int64_t)(addr - (here(e) + 7));
    b(e, 2, op1, op2);
    u32(e, (uint32_t)(int32_t)rel);
    b(e, 1, imm);
}
#define cmp_byte(e, addr, v) rip_byte((e), 0x80, 0x3D, (addr), (v))
#define mov_byte(e, addr, v) rip_byte((e), 0xC6, 0x05, (addr), (v))

/* mov rax, imm64 */
static void movabs_rax(Emit *e, uint64_t v) {
    b(e, 2, 0x48, 0xB8);
    u32(e, (uint32_t)v);
    u32(e, (uint32_t)(v >> 32));
}

#define JNC 0x73
#define JZ  0x74
#define JNZ 0x75
#define JG  0x7F
#define JMP8 0xEB

/*
 * rdx = objeto; deixa em eax o índice do tipo do objeto ([[rdx]] -> definição do tipo -> u16).
 * Se algum ponteiro for nulo, salta para 'orig' (as correções ficam em fixes[0..2]).
 */
static void emit_type_index(Emit *e, size_t fixes[3]) {
    b(e, 3, 0x48, 0x85, 0xD2);            /* test rdx, rdx            */
    fixes[0] = jcc8(e, JZ);               /* jz   orig                */
    b(e, 3, 0x48, 0x8B, 0x02);            /* mov  rax, [rdx]          ; REObjectInfo*  */
    b(e, 3, 0x48, 0x85, 0xC0);            /* test rax, rax            */
    fixes[1] = jcc8(e, JZ);               /* jz   orig                */
    b(e, 3, 0x48, 0x8B, 0x00);            /* mov  rax, [rax]          ; definição do tipo */
    b(e, 3, 0x48, 0x85, 0xC0);            /* test rax, rax            */
    fixes[2] = jcc8(e, JZ);               /* jz   orig                */
    b(e, 3, 0x0F, 0xB7, 0x00);            /* movzx eax, word [rax]    ; índice do tipo */
}

static void cmp_eax_imm32(Emit *e, uint32_t v) {
    b(e, 1, 0x3D);                        /* cmp eax, imm32 */
    u32(e, v);
}

/* ------------------------------------------------------------------ caves */

static uint64_t emit_add_damage(Emit *e, uint64_t cave_base, uint64_t fn) {
    uint64_t entry = here(e);
    size_t null_fix[3], to_enemy, god_off, not_enemy, ohk_off;

    emit_type_index(e, null_fix);
    cmp_eax_imm32(e, TYPE_HIT_POINT_CONTROLLER);
    to_enemy = jcc8(e, JNZ);              /* jne enemy */
    cmp_flag(e, cave_base, FLAG_GOD);
    god_off = jcc8(e, JZ);                /* je  orig  */
    b(e, 1, 0xC3);                        /* ret       ; jogador invencível: não perde vida */

    land(e, to_enemy);                    /* enemy: */
    cmp_eax_imm32(e, TYPE_ENEMY_HIT_POINT_CONTROLLER);
    not_enemy = jcc8(e, JNZ);             /* jne orig  */
    cmp_flag(e, cave_base, FLAG_OHK);
    ohk_off = jcc8(e, JZ);                /* je  orig  */
    b(e, 4, 0x44, 0x8B, 0x42, 0x58);      /* mov r8d, [rdx+58h] ; dano = vida atual */

    /* orig: instruções originais substituídas, depois volta à função */
    land(e, null_fix[0]); land(e, null_fix[1]); land(e, null_fix[2]);
    land(e, god_off); land(e, not_enemy); land(e, ohk_off);
    b(e, 4, 0x48, 0x8B, 0x41, 0x50);      /* mov rax, [rcx+50h] */
    b(e, 3, 0x4C, 0x8B, 0xCA);            /* mov r9, rdx        */
    jmp_abs(e, fn + 7);
    return entry;
}

static uint64_t emit_check_dead(Emit *e, uint64_t cave_base, uint64_t fn) {
    uint64_t entry = here(e);
    size_t null_fix[3], not_player, god_off;

    emit_type_index(e, null_fix);
    cmp_eax_imm32(e, TYPE_HIT_POINT_CONTROLLER);
    not_player = jcc8(e, JNZ);            /* jne orig */
    cmp_flag(e, cave_base, FLAG_GOD);
    god_off = jcc8(e, JZ);                /* je  orig */
    b(e, 2, 0x31, 0xC0);                  /* xor eax, eax ; "não morre" */
    b(e, 1, 0xC3);                        /* ret */

    land(e, null_fix[0]); land(e, null_fix[1]); land(e, null_fix[2]);
    land(e, not_player); land(e, god_off);
    b(e, 4, 0x48, 0x8B, 0x41, 0x50);      /* mov rax, [rcx+50h]        */
    b(e, 5, 0x48, 0x83, 0x78, 0x18, 0x00);/* cmp qword [rax+18h], 0    */
    jmp_abs(e, fn + 9);                   /* volta para o jne original (flags intactas) */
    return entry;
}

static uint64_t emit_slot_reduce(Emit *e, uint64_t cave_base, uint64_t fn) {
    uint64_t entry = here(e);
    size_t f_rdx, f_stock, f_item, to_weapon, f_noitem, f_items_off, to_skip, f_ammo_off;

    b(e, 3, 0x48, 0x85, 0xD2);            /* test rdx, rdx               */
    f_rdx = jcc8(e, JZ);                  /* jz   orig                   */
    b(e, 4, 0x48, 0x8B, 0x42, 0x18);      /* mov  rax, [rdx+18h]         ; Slot._Stock */
    b(e, 3, 0x48, 0x85, 0xC0);            /* test rax, rax               */
    f_stock = jcc8(e, JZ);                /* jz   orig                   */
    b(e, 4, 0x48, 0x8B, 0x40, 0x10);      /* mov  rax, [rax+10h]         ; Stock.DefaultItem (PrimitiveItem) */
    b(e, 3, 0x48, 0x85, 0xC0);            /* test rax, rax               */
    f_item = jcc8(e, JZ);                 /* jz   orig                   */
    b(e, 4, 0x83, 0x78, 0x14, 0x00);      /* cmp  dword [rax+14h], 0     ; WeaponId */
    to_weapon = jcc8(e, JG);              /* jg   weapon                 */
    b(e, 4, 0x83, 0x78, 0x10, 0x00);      /* cmp  dword [rax+10h], 0     ; ItemId */
    f_noitem = jcc8(e, JZ);               /* je   orig                   */
    cmp_flag(e, cave_base, FLAG_ITEMS);
    f_items_off = jcc8(e, JZ);            /* je   orig                   */
    to_skip = jcc8(e, JMP8);              /* jmp  skip                   */

    land(e, to_weapon);                   /* weapon: */
    cmp_flag(e, cave_base, FLAG_AMMO);
    f_ammo_off = jcc8(e, JZ);             /* je   orig                   */

    land(e, to_skip);                     /* skip: devolve N sem gastar nada */
    b(e, 3, 0x44, 0x89, 0xC0);            /* mov eax, r8d */
    b(e, 1, 0xC3);                        /* ret */

    land(e, f_rdx); land(e, f_stock); land(e, f_item);
    land(e, f_noitem); land(e, f_items_off); land(e, f_ammo_off);
    b(e, 5, 0x48, 0x89, 0x6C, 0x24, 0x10);/* mov [rsp+10h], rbp ; instrução original */
    jmp_abs(e, fn + 5);
    return entry;
}

/*
 * Mr. X: repor o chapéu. Entra no início de Em6200Think.onThinkUpdate (rcx = contexto, rdx = this),
 * ou seja, na thread do jogo e com um contexto válido. Sem pedido pendente, só faz dois cmp e segue.
 *
 * Com pedido (HAT_REQUEST ou HAT_HAS_HAT) e um Em6200Think sem o bit FLAG.OnHat:
 *   - se HatLostFlag (Guid em this+200h) não estiver vazio e o jogo o tiver a TRUE, o chapéu foi tirado
 *     pela história e o próprio onThinkUpdate voltaria a deixá-lo cair: não mexe e responde HAT_BLOCKED;
 *   - senão liga o bit OnHat em this.Flag (BitFlag em this+1E0h, valor em +10h) e chama
 *     setHatDisp(true), que volta a mostrar o chapéu na cabeça. Responde HAT_DONE.
 * As flags globais do jogo nunca são alteradas.
 */
static uint64_t emit_mrx_hat(Emit *e, uint64_t cave_base, uint64_t module_base, uint64_t fn) {
    const uint64_t req = cave_base + CAVE_HAT_OFFSET;
    uint64_t entry = here(e);
    size_t go1, go2, orig, f_nohat, f_restore, f_no_mgr, f_exc, f_lost, f_done;

    cmp_byte(e, req, HAT_REQUEST);
    go1 = jcc8(e, JZ);
    cmp_byte(e, req, HAT_HAS_HAT);
    go2 = jcc8(e, JZ);

    /* orig: instruções originais substituídas, depois volta à função */
    orig = e->pos;
    b(e, 6, 0x40, 0x55, 0x56, 0x57, 0x41, 0x56); /* push rbp ; push rsi ; push rdi ; push r14 */
    jmp_abs(e, fn + 6);

    land(e, go1); land(e, go2);
    b(e, 4, 0x48, 0x8B, 0x41, 0x50);      /* mov  rax, [rcx+50h]       */
    b(e, 5, 0x48, 0x83, 0x78, 0x18, 0x00);/* cmp  qword [rax+18h], 0   ; exceção pendente na VM? */
    jcc8_back(e, JNZ, orig);
    b(e, 3, 0x48, 0x85, 0xD2);            /* test rdx, rdx             */
    jcc8_back(e, JZ, orig);
    b(e, 3, 0x48, 0x8B, 0x02);            /* mov  rax, [rdx]           ; REObjectInfo* */
    b(e, 3, 0x48, 0x85, 0xC0);            /* test rax, rax             */
    jcc8_back(e, JZ, orig);
    b(e, 3, 0x48, 0x8B, 0x00);            /* mov  rax, [rax]           ; definição do tipo */
    b(e, 3, 0x48, 0x85, 0xC0);            /* test rax, rax             */
    jcc8_back(e, JZ, orig);
    b(e, 3, 0x0F, 0xB7, 0x00);            /* movzx eax, word [rax]     ; índice do tipo */
    cmp_eax_imm32(e, TYPE_EM6200_THINK);
    jcc8_back(e, JNZ, orig);
    b(e, 7, 0x48, 0x8B, 0x82, 0xE0, 0x01, 0x00, 0x00); /* mov rax, [rdx+1E0h] ; this.Flag (BitFlag) */
    b(e, 3, 0x48, 0x85, 0xC0);            /* test rax, rax             */
    jcc8_back(e, JZ, orig);
    b(e, 5, 0x0F, 0xBA, 0x60, 0x10, 0x09);/* bt   dword [rax+10h], 9   ; FLAG.OnHat */
    f_nohat = jcc8(e, JNC);               /* jnc  nohat                */

    /* Este Mr. X já tem chapéu: avisa o programa (sem fechar o pedido; pode haver outro sem chapéu). */
    cmp_byte(e, req, HAT_REQUEST);
    jcc8_back(e, JNZ, orig);
    mov_byte(e, req, HAT_HAS_HAT);
    jcc8_back(e, JMP8, orig);

    land(e, f_nohat);                     /* nohat: */
    b(e, 4, 0x51, 0x52, 0x53, 0x56);      /* push rcx ; push rdx ; push rbx ; push rsi */
    b(e, 4, 0x48, 0x83, 0xEC, 0x38);      /* sub  rsp, 38h  ; sombra 20h + Guid 10h, rsp alinhado a 16 */
    b(e, 3, 0x48, 0x89, 0xCB);            /* mov  rbx, rcx  ; contexto */
    b(e, 3, 0x48, 0x89, 0xD6);            /* mov  rsi, rdx  ; this     */
    b(e, 7, 0x48, 0x8B, 0x86, 0x00, 0x02, 0x00, 0x00); /* mov rax, [rsi+200h] ; HatLostFlag (Guid) */
    b(e, 7, 0x48, 0x0B, 0x86, 0x08, 0x02, 0x00, 0x00); /* or  rax, [rsi+208h] */
    f_restore = jcc8(e, JZ);              /* Guid vazio: o jogo nunca o consulta -> repor */
    movabs_rax(e, module_base + RVA_GUD_MANAGER);
    b(e, 3, 0x48, 0x8B, 0x10);            /* mov  rdx, [rax] ; GlobalUserDataManager */
    b(e, 3, 0x48, 0x85, 0xD2);            /* test rdx, rdx   */
    f_no_mgr = jcc8(e, JZ);               /* sem gestor: não arriscar */
    b(e, 7, 0x48, 0x8B, 0x86, 0x00, 0x02, 0x00, 0x00); /* mov rax, [rsi+200h] */
    b(e, 5, 0x48, 0x89, 0x44, 0x24, 0x20);             /* mov [rsp+20h], rax  */
    b(e, 7, 0x48, 0x8B, 0x86, 0x08, 0x02, 0x00, 0x00); /* mov rax, [rsi+208h] */
    b(e, 5, 0x48, 0x89, 0x44, 0x24, 0x28);             /* mov [rsp+28h], rax  */
    b(e, 5, 0x4C, 0x8D, 0x44, 0x24, 0x20);             /* lea r8, [rsp+20h]   ; &Guid */
    b(e, 3, 0x48, 0x89, 0xD9);            /* mov  rcx, rbx   */
    movabs_rax(e, module_base + RVA_CHECK_FLAG);
    b(e, 2, 0xFF, 0xD0);                  /* call rax        ; checkFlag(HatLostFlag) */
    b(e, 4, 0x48, 0x8B, 0x4B, 0x50);      /* mov  rcx, [rbx+50h]     */
    b(e, 5, 0x48, 0x83, 0x79, 0x18, 0x00);/* cmp  qword [rcx+18h], 0 */
    f_exc = jcc8(e, JNZ);
    b(e, 3, 0x83, 0xF8, BOOL_RESULT_TRUE);/* cmp  eax, TRUE  */
    f_lost = jcc8(e, JZ);

    land(e, f_restore);                   /* restore: */
    b(e, 7, 0x48, 0x8B, 0x86, 0xE0, 0x01, 0x00, 0x00); /* mov rax, [rsi+1E0h]          */
    b(e, 7, 0x81, 0x48, 0x10, 0x00, 0x02, 0x00, 0x00); /* or  dword [rax+10h], 200h    ; liga OnHat */
    b(e, 3, 0x48, 0x89, 0xD9);            /* mov  rcx, rbx   */
    b(e, 3, 0x48, 0x89, 0xF2);            /* mov  rdx, rsi   */
    b(e, 6, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00); /* mov r8d, 1 */
    movabs_rax(e, module_base + RVA_SET_HAT_DISP);
    b(e, 2, 0xFF, 0xD0);                  /* call rax        ; setHatDisp(true) */
    mov_byte(e, req, HAT_DONE);
    f_done = jcc8(e, JMP8);

    land(e, f_no_mgr); land(e, f_exc); land(e, f_lost);
    mov_byte(e, req, HAT_BLOCKED);

    land(e, f_done);
    b(e, 4, 0x48, 0x83, 0xC4, 0x38);      /* add  rsp, 38h   */
    b(e, 4, 0x5E, 0x5B, 0x5A, 0x59);      /* pop rsi ; pop rbx ; pop rdx ; pop rcx */
    jmp_abs(e, e->base + orig);           /* segue para a função original */
    return entry;
}

/* ------------------------------------------------------------------ API */

int build_patches(uint64_t module_base, uint64_t cave_addr, uint8_t *cave, HookPatch out[NUM_HOOKS]) {
    Emit e;
    uint64_t entries[NUM_HOOKS];

    memset(cave, 0xCC, CAVE_SIZE);
    memset(cave + CAVE_FLAGS_OFFSET, 0, CAVE_CODE_OFFSET);

    e.buf = cave;
    e.base = cave_addr;
    e.pos = CAVE_CODE_OFFSET;
    e.error = 0;

    entries[0] = emit_add_damage(&e, cave_addr, module_base + HOOK_SITES[0].rva);
    while (e.pos % 16) e.pos++;
    entries[1] = emit_check_dead(&e, cave_addr, module_base + HOOK_SITES[1].rva);
    while (e.pos % 16) e.pos++;
    entries[2] = emit_slot_reduce(&e, cave_addr, module_base + HOOK_SITES[2].rva);
    while (e.pos % 16) e.pos++;
    entries[3] = emit_mrx_hat(&e, cave_addr, module_base, module_base + HOOK_SITES[3].rva);

    if (e.error) return e.error;

    for (int i = 0; i < NUM_HOOKS; i++) {
        const HookSite *s = &HOOK_SITES[i];
        HookPatch *p = &out[i];
        uint64_t fn = module_base + s->rva;
        int64_t rel = (int64_t)(entries[i] - (fn + 5));
        if (!fits_rel32(rel)) return -4;

        memset(p, 0, sizeof(*p));
        p->target = fn;
        p->len = s->stolen_len;
        memcpy(p->original, s->expected, s->stolen_len);
        memset(p->patch, 0x90, sizeof(p->patch));
        p->patch[0] = 0xE9;
        p->patch[1] = (uint8_t)(rel & 0xFF);
        p->patch[2] = (uint8_t)((rel >> 8) & 0xFF);
        p->patch[3] = (uint8_t)((rel >> 16) & 0xFF);
        p->patch[4] = (uint8_t)((rel >> 24) & 0xFF);
    }
    return 0;
}
