/* ROM de teste do núcleo do Lynx (Índigo, etapa 7), compilada com o cc65 por t7_lynx.sh:
**   cl65 -t lynx -O -o lynxteste.lnx t7_lynx_rom.c   (o script marca EEPROM 93C46 no cabeçalho)
** Confere a matemática da Suzy (multiplicação sem/com sinal, acúmulo, divisão, divisão por zero), a EEPROM
** (gravar/ler) e desenha um painel de sprites (literal/empacotado, 1/4 bits, escala, espelhos, inclinação,
** esticamento, quadrantes, XOR, colisão). O resultado aparece na tela: "M" (matemática), "E" (EEPROM) e "C"
** (colisão) seguidos de 1 = ok / 0 = falha, e um retângulo verde (tudo ok) ou vermelho. O teste em C++ fixa o
** hash da tela, conferida contra o emulador Handy.
*/
#include <lynx.h>
#include <tgi.h>
#include <6502.h>

static unsigned char okMath, okEe, okColl;

static unsigned long efgh(void)
{
    return (unsigned long)SUZY.mathh | ((unsigned long)SUZY.mathg << 8) | ((unsigned long)SUZY.mathf << 16) |
           ((unsigned long)SUZY.mathe << 24);
}
static unsigned long abcd(void)
{
    return (unsigned long)SUZY.mathd | ((unsigned long)SUZY.mathc << 8) | ((unsigned long)SUZY.mathb << 16) |
           ((unsigned long)SUZY.matha << 24);
}
static unsigned long jklm(void)
{
    return (unsigned long)SUZY.mathm | ((unsigned long)SUZY.mathl << 8) | ((unsigned long)SUZY.mathk << 16) |
           ((unsigned long)SUZY.mathj << 24);
}
static void mul(unsigned ab, unsigned cd)
{
    SUZY.mathd = (unsigned char)cd;
    SUZY.mathc = (unsigned char)(cd >> 8);
    SUZY.mathb = (unsigned char)ab;
    SUZY.matha = (unsigned char)(ab >> 8);
}

static void testMath(void)
{
    unsigned char ok = 1;
    SUZY.sprsys = 0;
    mul(1234, 5678);
    if (efgh() != 7006652UL) ok = 0;
    SUZY.sprsys = 0x80;                 /* com sinal */
    mul(0xFFFD, 1000);                  /* -3 * 1000 */
    if (efgh() != 0xFFFFF448UL) ok = 0;
    SUZY.sprsys = 0x40;                 /* acumula */
    SUZY.mathm = 0; SUZY.mathl = 0; SUZY.mathk = 0; SUZY.mathj = 0;
    mul(100, 200);
    mul(300, 400);
    if (jklm() != 140000UL) ok = 0;
    SUZY.sprsys = 0;
    SUZY.mathp = 7; SUZY.mathn = 0;
    SUZY.mathh = 0x40; SUZY.mathg = 0x42; SUZY.mathf = 0x0F; SUZY.mathe = 0x00;  /* 1000000 / 7 */
    if (abcd() != 142857UL || jklm() != 1) ok = 0;
    SUZY.mathp = 0; SUZY.mathn = 0;
    SUZY.mathh = 1; SUZY.mathg = 0; SUZY.mathf = 0; SUZY.mathe = 0;
    if (abcd() != 0xFFFFFFFFUL || !(SUZY.sprsys & 0x40)) ok = 0;
    okMath = ok;
}

static void testEeprom(void)
{
    unsigned char ok = 1;
    lynx_eeprom_write(3, 0xBEEF);
    lynx_eeprom_write(10, 0x1234);
    /* a rotina de leitura do cc65 devolve o byte alto em Y (não em X): só o byte baixo é confiável aqui; a
    ** palavra inteira é conferida depois pelo teste em C++ no save exportado */
    if ((unsigned char)lynx_eeprom_read(3) != 0xEF) ok = 0;
    if ((unsigned char)lynx_eeprom_read(10) != 0x34) ok = 0;
    lynx_eeprom_erase(10);
    if ((unsigned char)lynx_eeprom_read(10) != 0xFF) ok = 0;
    okEe = ok;
}

/* ---------------------------------------------------------------- dados dos sprites */
/* 8x8, 1 bit por ponto, literal */
static unsigned char box1[] = {
    2, 0xFF, 2, 0x81, 2, 0xBD, 2, 0xA5, 2, 0xA5, 2, 0xBD, 2, 0x83, 2, 0xFF, 0
};
/* 8x6, 4 bits, empacotado: 4 pontos da caneta 3 e 4 da caneta 9 por linha */
static unsigned char packed4[] = {
    4, 0x19, 0x8E, 0x40, 4, 0x19, 0x8E, 0x40, 4, 0x19, 0x8E, 0x40,
    4, 0x19, 0x8E, 0x40, 4, 0x19, 0x8E, 0x40, 4, 0x19, 0x8E, 0x40, 0
};
/* quatro quadrantes 4x4 de cores diferentes, 4 bits literal */
static unsigned char quads[] = {
    3, 0xAA, 0xAA, 3, 0xAA, 0xAA, 3, 0xAA, 0xAA, 3, 0xAA, 0xAA, 1,
    3, 0xBB, 0xBB, 3, 0xBB, 0xBB, 3, 0xBB, 0xBB, 3, 0xBB, 0xBB, 1,
    3, 0xCC, 0xCC, 3, 0xCC, 0xCC, 3, 0xCC, 0xCC, 3, 0xCC, 0xCC, 1,
    3, 0xDD, 0xDD, 3, 0xDD, 0xDD, 3, 0xDD, 0xDD, 3, 0xDD, 0xDD, 0
};
/* 16 linhas de 8 pontos (1 bit) para inclinação/esticamento */
static unsigned char bar[] = {
    2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF,
    2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 0
};
/* bloco cheio 8x8 (1 bit) para colisão e XOR */
static unsigned char solid[] = {
    2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 2, 0xFF, 0
};

static SCB_REHV_PAL s1 = { BPP_1 | TYPE_NORMAL, LITERAL | REHV, 0x20, 0, box1, 8, 8, 0x100, 0x100, {0x0E} };
static SCB_REHV_PAL s2 = { BPP_1 | TYPE_NORMAL, LITERAL | REHV, 0x20, 0, box1, 22, 8, 0x200, 0x200, {0x0D} };
static SCB_REHV_PAL s3 = { BPP_1 | TYPE_NORMAL | HFLIP, LITERAL | REHV, 0x20, 0, box1, 50, 8, 0x100, 0x100, {0x0C} };
static SCB_REHV_PAL s4 = { BPP_1 | TYPE_NORMAL | VFLIP, LITERAL | REHV, 0x20, 0, box1, 60, 18, 0x100, 0x100, {0x0B} };
static SCB_REHV_PAL s5 = { BPP_4 | TYPE_NORMAL, PACKED | REHV, 0x20, 0, packed4, 70, 8, 0x180, 0x100,
                           {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF} };
static SCB_REHV_PAL s6 = { BPP_4 | TYPE_NORMAL, LITERAL | REHV | DRAWUP | DRAWLEFT, 0x20, 0, quads, 110, 20, 0x100, 0x100,
                           {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF} };
static SCB_REHVST_PAL s7 = { BPP_1 | TYPE_NORMAL, LITERAL | REHVST, 0x20, 0, bar, 10, 40, 0x100, 0x100, 0x0020, 0x0060,
                             {0x07} };
static SCB_REHV_PAL s8 = { BPP_1 | TYPE_BACKNONCOLL, LITERAL | REHV, 0x20, 0, box1, 140, 40, 0x100, 0x180, {0x45} };
/* colisão: A (número 1) e B (número 2) sobrepostos; o depósito fica no byte antes do SCB */
static struct { unsigned char dep; SCB_REHV_PAL scb; } ca = {
    0, { BPP_1 | TYPE_NORMAL, LITERAL | REHV, 0x01, 0, solid, 60, 40, 0x200, 0x200, {0x06} } };
static struct { unsigned char dep; SCB_REHV_PAL scb; } cb = {
    0, { BPP_1 | TYPE_NORMAL, LITERAL | REHV, 0x02, 0, solid, 70, 50, 0x200, 0x200, {0x05} } };
static SCB_REHV_PAL sx = { BPP_1 | TYPE_XOR, LITERAL | REHV, 0x20, 0, solid, 100, 50, 0x300, 0x100, {0x0F} };

void main(void)
{
    unsigned char ok;
    tgi_install(tgi_static_stddrv);
    tgi_init();
    CLI();
    while (tgi_busy()) {}
    tgi_setcollisiondetection(1);
    tgi_setbgcolor(0);
    tgi_clear();

    testMath();
    testEeprom();

    tgi_sprite(&s1);
    tgi_sprite(&s2);
    tgi_sprite(&s3);
    tgi_sprite(&s4);
    tgi_sprite(&s5);
    tgi_sprite(&s6);
    tgi_sprite(&s7);
    tgi_sprite(&s8);
    tgi_sprite(&ca.scb);
    tgi_sprite(&cb.scb);
    tgi_sprite(&sx);
    okColl = (cb.dep == 1) && (ca.dep == 0);

    ok = okMath && okEe && okColl;
    tgi_setcolor(ok ? 0x05 : 0x02);
    tgi_bar(120, 80, 155, 98);
    tgi_setcolor(0x0F);
    tgi_outtextxy(4, 80, okMath ? "M1" : "M0");
    tgi_outtextxy(28, 80, okEe ? "E1" : "E0");
    tgi_outtextxy(52, 80, okColl ? "C1" : "C0");
    tgi_updatedisplay();
    while (1) {}
}
