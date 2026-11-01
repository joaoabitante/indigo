/* Programas de teste do 32X do Índigo (lado do 68000): os mesmos comandos do exemplo 32x-skeleton do Marsdev
 * (ler o controle, escrever no plano B e na VRAM a pedido do SH-2 pelo COMM0), com a cor de fundo fixa, e o
 * comando 9: mandar 1024 palavras pela FIFO de DREQ (registradores A15106-A15112) para o DMA do SH-2. */
#include "common.h"

static volatile uint16_t* const mars_comm0 = (uint16_t*)MARS_COMM0;
static volatile uint16_t* const mars_comm2 = (uint16_t*)MARS_COMM2;
static volatile uint16_t* const mars_comm8 = (uint16_t*)MARS_COMM8;
static volatile uint32_t* const mars_comm12 = (uint32_t*)MARS_COMM12;
static volatile uint16_t* const vdp_data_port = (uint16_t*)VDP_DATA_PORT;
static volatile uint16_t* const vdp_ctrl_port = (uint16_t*)VDP_CTRL_PORT;
static volatile uint32_t* const vdp_ctrl_wide = (uint32_t*)VDP_CTRL_PORT;
static volatile uint8_t* const dreq_ctl = (uint8_t*)0xA15107;
static volatile uint16_t* const dreq_len = (uint16_t*)0xA15110;
static volatile uint16_t* const dreq_fifo = (uint16_t*)0xA15112;

extern uint16_t read_joypad(uint8_t player);

uint32_t timer = 0;
uint16_t vramOffset = 0;

__attribute__((section(".data"))) void vdp_color(uint16_t index, uint16_t color) {
  index <<= 1;
  *vdp_ctrl_wide = ((0xC000 + (((uint32_t)index) & 0x3FFF)) << 16) + (((uint32_t)index) >> 14);
  *vdp_data_port = color;
}

__attribute__((section(".data"))) static void send_dreq(void) {
  uint16_t i;
  *dreq_len = 1024;
  *dreq_ctl = 4;  /* 68S */
  for (i = 0; i < 1024; i++) {
    while (*dreq_ctl & 0x80) {}  /* FIFO cheia */
    *dreq_fifo = (uint16_t)(i * 37 + (i >> 3));
  }
}

__attribute__((section(".data"))) void do_commands(void) {
  uint16_t cmd = *mars_comm0;
  switch (cmd >> 8) {
    default: break;
    case 0: return;
    case 3: *mars_comm8 = read_joypad(cmd); break;
    case 5: vramOffset = *mars_comm2; break;
    case 6:
      *vdp_ctrl_wide = (((uint32_t)0x6000 + ((vramOffset) & 0x3FFF)) << 16) + (((vramOffset) >> 14) | 0x03);
      *vdp_data_port = *mars_comm2;
      vramOffset += 2;
      break;
    case 7:
      *vdp_ctrl_wide = (((uint32_t)0x4000 + ((vramOffset) & 0x3FFF)) << 16) + (((vramOffset) >> 14) | 0x00);
      *vdp_data_port = *mars_comm2;
      vramOffset += 2;
      break;
    case 9: send_dreq(); break;
  }
  *mars_comm0 = 0;
}

__attribute__((section(".data"))) void main(void) {
  vdp_color(0, 0x0A4);
  while (1) {
    while (*vdp_ctrl_port & 8) do_commands();
    while (!(*vdp_ctrl_port & 8)) do_commands();
    *mars_comm12 = ++timer;
  }
}
