/* LifeLine Helper - construção dos patches. Ver patches.h. */
#include "patches.h"
#include <string.h>
#include <stdarg.h>

#define TYPE_HIT_POINT_CONTROLLER       0x8724u  /* app.ropeway.HitPointController (jogador e sobreviventes) */
#define TYPE_ENEMY_HIT_POINT_CONTROLLER 0xD525u  /* app.ropeway.EnemyHitPointController */

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

/* ------------------------------------------------------------------ API */

int build_patches(uint64_t module_base, uint64_t cave_addr, uint8_t *cave, HookPatch out[NUM_HOOKS]) {
    Emit e;
    uint64_t entries[NUM_HOOKS];

    memset(cave, 0xCC, CAVE_SIZE);
    memset(cave + CAVE_FLAGS_OFFSET, 0, 0x10);

    e.buf = cave;
    e.base = cave_addr;
    e.pos = CAVE_CODE_OFFSET;
    e.error = 0;

    entries[0] = emit_add_damage(&e, cave_addr, module_base + HOOK_SITES[0].rva);
    while (e.pos % 16) e.pos++;
    entries[1] = emit_check_dead(&e, cave_addr, module_base + HOOK_SITES[1].rva);
    while (e.pos % 16) e.pos++;
    entries[2] = emit_slot_reduce(&e, cave_addr, module_base + HOOK_SITES[2].rva);

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
