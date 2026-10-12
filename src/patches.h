/*
 * LifeLine Helper - construção dos patches (código partilhado entre o programa e os testes).
 *
 * Alvo: re2.exe (Resident Evil 2 Remake, build sem ray tracing, TDB66,
 *       PE timestamp 2021-10-13). Os RVAs e bytes abaixo foram tirados deste executável.
 *
 * Quatro funções do jogo são desviadas para um bloco de código nosso ("cave"):
 *
 *   app.ropeway.HitPointController.addDamage(int)   RVA 0xF9A080
 *       jogador/sobreviventes (tipo 0x8724) + invencível  -> sai sem dano
 *       inimigos (EnemyHitPointController, tipo 0xD525) + 1 golpe -> dano = vida atual
 *
 *   app.ropeway.HitPointController.checkDead(int)   RVA 0xF99DC0
 *       jogador/sobreviventes + invencível -> devolve false ("não morre")
 *
 *   app.ropeway.inventory.Slot.reduce(int)           RVA 0x51E620
 *       slot de arma  + munição infinita -> devolve N sem gastar
 *       slot de item  + itens infinitos  -> devolve N sem gastar
 *
 *   app.ropeway.enemy.em6200.Em6200Think.onThinkUpdate()   RVA 0x13A35E0   (Mr. X, 1x por frame)
 *       só faz alguma coisa quando o programa pede para repor o chapéu (byte CAVE_HAT_OFFSET):
 *       volta a ligar o bit FLAG.OnHat e chama setHatDisp(true), na thread do jogo.
 *
 * Convenção de chamada dos métodos do jogo: rcx = contexto da VM, rdx = this, r8 = 1.º argumento.
 */
#ifndef RE2_PATCHES_H
#define RE2_PATCHES_H

#include <stdint.h>
#include <stddef.h>

enum {
    FLAG_GOD   = 0,
    FLAG_AMMO  = 1,
    FLAG_ITEMS = 2,
    FLAG_OHK   = 3,
    FLAG_COUNT = 4
};

/* Pedido de reposição do chapéu do Mr. X (byte em CAVE_HAT_OFFSET). */
enum {
    HAT_IDLE      = 0,   /* nada pedido                                                     */
    HAT_REQUEST   = 1,   /* o programa pediu; o jogo trata no próximo onThinkUpdate          */
    HAT_DONE      = 2,   /* chapéu reposto                                                   */
    HAT_BLOCKED   = 3,   /* o jogo marcou o chapéu como perdido por uma flag da história     */
    HAT_HAS_HAT   = 4    /* o Mr. X visto pelo jogo já tem chapéu (o pedido continua aberto) */
};

#define CAVE_FLAGS_OFFSET 0x00   /* 4 bytes de flags no início do cave; assinatura em 0x04..0x0F */
#define CAVE_HAT_OFFSET   0x10   /* 1 byte: estado do pedido do chapéu (HAT_*)                    */
#define CAVE_CODE_OFFSET  0x20
#define CAVE_SIZE         0x400

#define NUM_HOOKS 4
#define NUM_VERIFY 3
#define MAX_STOLEN 16

typedef struct {
    const char *name;
    uint32_t rva;                     /* início da função no módulo */
    uint8_t  stolen_len;              /* bytes substituídos pelo salto (>= 5, fronteira de instrução) */
    uint8_t  expected[32];            /* bytes originais esperados (verificação de versão) */
    uint8_t  expected_len;
} HookSite;

/* Os sítios a desviar, com os bytes originais esperados. */
extern const HookSite HOOK_SITES[NUM_HOOKS];

/* Outros sítios que o código injetado usa (funções chamadas, ponteiro global): só são verificados. */
extern const HookSite VERIFY_SITES[NUM_VERIFY];

typedef struct {
    uint64_t target;                  /* endereço absoluto da função no jogo */
    uint8_t  original[MAX_STOLEN];    /* bytes originais (para repor) */
    uint8_t  patch[MAX_STOLEN];       /* jmp rel32 para o cave + NOPs */
    uint8_t  len;
} HookPatch;

/*
 * Constrói o código do cave e os saltos de cada função.
 *   module_base : endereço base do re2.exe no processo do jogo
 *   cave_addr   : endereço onde o cave vai ficar (tem de estar a menos de 2 GB do módulo)
 *   cave        : buffer de CAVE_SIZE bytes que recebe o conteúdo do cave (flags a 0)
 *   out         : NUM_HOOKS patches
 * Devolve 0 em caso de sucesso, ou um código de erro (<0) se algum salto não couber em rel32.
 */
int build_patches(uint64_t module_base, uint64_t cave_addr, uint8_t *cave, HookPatch out[NUM_HOOKS]);

#endif
