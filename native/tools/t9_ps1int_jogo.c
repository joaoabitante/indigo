/* Programa de teste do disco do PlayStation do Índigo (t9_ps1int): dá a partida pela BIOS (SYSTEM.CNF ->
 * JOGO.EXE) e usa o console inteiro:
 *  - GTE: 48 cubos texturizados girando (RTPT/RTPS, NCLIP, AVSZ4, NCS para a luz);
 *  - GPU: quadros duplos, tabela de ordenação limpa por DMA6 (OTC) e desenhada por DMA2 (lista ligada), textura
 *    de 16 bits lida do CD e carregada na VRAM;
 *  - CD: a textura (TEXTURA.BIN) e leituras contínuas do DADOS.BIN, conferidas palavra a palavra; depois a faixa 2
 *    (CD-DA) tocando pela SPU (volume do CD e mixagem do CdMix);
 *  - SPU: duas vozes ADPCM (onda quadrada feita aqui) enviadas por DMA4 e tocadas alternando esquerda e direita;
 *  - controle: os botões e os analógicos mudam a câmera e aparecem na tela.
 * Os cubos param enquanto a faixa 2 toca (quadros 600 a 1199 de cada 1200), para comparar a imagem com outro emulador.
 * Escreve na TTY da BIOS (printf) o que a régua confere. Montado pelo t9_ps1int_build.py com o PSn00bSDK. */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <psxapi.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxspu.h>
#include <psxcd.h>
#include <psxpad.h>
#include <inline_c.h>

#define OT_LEN 1024
#define PACKET_LEN 49152
#define SCREEN_XRES 320
#define SCREEN_YRES 240
#define NCX 8
#define NCY 6
#define NCUBES (NCX * NCY)
#define READ_SECTORS 16
#define DATA_SECTORS 160

typedef struct {
	DISPENV disp;
	DRAWENV draw;
	unsigned int ot[OT_LEN];
	char p[PACKET_LEN];
} DB;

static DB db[2];
static int db_active = 0;
static char *db_nextpri;

typedef struct { short v0, v1, v2, v3; } INDEX;

static SVECTOR cube_verts[] = {
	{ -40, -40, -40, 0 }, {  40, -40, -40, 0 }, { -40,  40, -40, 0 }, {  40,  40, -40, 0 },
	{  40, -40,  40, 0 }, { -40, -40,  40, 0 }, {  40,  40,  40, 0 }, { -40,  40,  40, 0 }
};
static SVECTOR cube_norms[] = {
	{ 0, 0, -ONE, 0 }, { 0, 0, ONE, 0 }, { 0, -ONE, 0, 0 }, { 0, ONE, 0, 0 }, { -ONE, 0, 0, 0 }, { ONE, 0, 0, 0 }
};
static INDEX cube_indices[] = {
	{ 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 5, 4, 0, 1 }, { 6, 7, 3, 2 }, { 0, 2, 5, 7 }, { 3, 1, 6, 4 }
};

static MATRIX color_mtx = { ONE, 0, 0, ONE, 0, 0, ONE, 0, 0 };
static MATRIX light_mtx = { -2048, -2048, -2048, 0, 0, 0, 0, 0, 0 };

static char pad_buff[2][34];
static unsigned int tex_buf[64 * 64 / 2];
static unsigned int read_buf[READ_SECTORS * 512];
static unsigned char adpcm[64];

static CdlFILE data_file;
static int data_lba = 0, read_index = 0, read_ok = 0, read_bad = 0, reading = 0, read_off = 0;
static int playing = 0, cd_tracks = 0;

#define SPU_REG(o) (*(volatile unsigned short *)(0x1F801C00 + (o)))

/* valor da palavra w do DADOS.BIN (o mesmo do t9_ps1int_disco.py) */
static unsigned int data_word(unsigned int w) {
	unsigned int x = w * 2654435761u;
	return x ^ (x >> 13) ^ 0x1D1C0DEu;
}

static void display(void) {
	DrawSync(0);
	VSync(0);
	db_active ^= 1;
	db_nextpri = db[db_active].p;
	ClearOTagR(db[db_active].ot, OT_LEN);
	PutDrawEnv(&db[db_active].draw);
	PutDispEnv(&db[db_active].disp);
	SetDispMask(1);
	DrawOTag(db[1 - db_active].ot + (OT_LEN - 1));
}

static void make_adpcm(void) {
	/* dois blocos: 28 amostras altas e 28 baixas (onda quadrada); o segundo volta ao primeiro */
	int b, i;
	for (b = 0; b < 2; b++) {
		unsigned char *p = adpcm + b * 16;
		unsigned char nib = b == 0 ? 0x7 : 0x9;
		p[0] = 4;               /* shift 4, filtro 0 */
		p[1] = b == 0 ? 4 : 3;  /* início do laço / fim com repetição */
		for (i = 2; i < 16; i++) p[i] = (unsigned char)(nib | (nib << 4));
	}
	memset(adpcm + 32, 0, 32);
	adpcm[32 + 1] = 7;
}

static void init_sound(void) {
	SpuVoiceRaw v;
	SpuInit();
	SPU_REG(0x180) = 0x3FFF;  /* volume principal */
	SPU_REG(0x182) = 0x3FFF;
	SPU_REG(0x1B0) = 0x7FFF;  /* volume do CD */
	SPU_REG(0x1B2) = 0x7FFF;
	make_adpcm();
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(0x1000);
	SpuWrite(adpcm, sizeof adpcm);
	SpuWait();
	v.vol.left = 0x3000; v.vol.right = 0x0400;
	v.freq = 0x0600;
	v.addr = 0x1000 / 8; v.loop_addr = 0x1000 / 8; v.res = 0;
	v.adsr_param = 0x0000000F;
	SpuSetVoiceRaw(0, &v);
	v.vol.left = 0x0400; v.vol.right = 0x3000;
	v.freq = 0x0900;
	SpuSetVoiceRaw(1, &v);
	printf("som: ADPCM na SPU por DMA\n");
}

static void init_cd(void) {
	CdlFILE f;
	RECT r;
	int i, bad = 0;
	CdInit();
	CdControl(CdlDemute, 0, 0);  /* a BIOS deixa o áudio do CD mudo */
	if (!CdSearchFile(&f, "\\TEXTURA.BIN;1")) { printf("cd: TEXTURA.BIN nao encontrado\n"); return; }
	CdControl(CdlSetloc, (u_char *)&f.pos, 0);
	CdRead(4, tex_buf, CdlModeSpeed);
	CdReadSync(0, 0);
	for (i = 0; i < 64 * 64; i++) {
		unsigned short px = ((unsigned short *)tex_buf)[i];
		int x = i & 63, y = i >> 6;
		unsigned short exp = (unsigned short)(((x >> 1) & 31) | (((y >> 1) & 31) << 5) | ((((x ^ y) >> 3) & 1) ? 0x7C00 : 0x2000));
		if (px != exp) bad++;
	}
	printf("cd: textura lida (%d pontos diferentes)\n", bad);
	setRECT(&r, 640, 0, 64, 64);
	LoadImage(&r, tex_buf);
	DrawSync(0);
	if (!CdSearchFile(&data_file, "\\DADOS.BIN;1")) { printf("cd: DADOS.BIN nao encontrado\n"); return; }
	data_lba = CdPosToInt(&data_file.pos);
	printf("cd: DADOS.BIN no setor %d, %d bytes\n", data_lba, (int)data_file.size);
	{
		/* GetTN: primeira e última faixa (em BCD); o CdGetToc do PSn00bSDK de 2020 guarda o status no lugar dos minutos */
		unsigned char tn[8];
		CdControl(CdlGetTN, 0, tn);
		CdSync(0, 0);
		cd_tracks = (tn[2] >> 4) * 10 + (tn[2] & 15);
	}
	printf("cd: %d faixas\n", cd_tracks);
}

static void check_read(void) {
	int i, bad = 0;
	unsigned int base = (unsigned int)read_off * 512u;
	for (i = 0; i < READ_SECTORS * 512; i++)
		if (read_buf[i] != data_word(base + (unsigned int)i)) bad++;
	if (bad) read_bad++; else read_ok++;
	if (read_index < 8 || (read_index % 16) == 0 || bad)
		printf("leitura %d (setor %d): %s\n", read_index, read_off, bad ? "ERRO" : "ok");
	read_index++;
}

static void cd_step(int frame) {
	CdlLOC loc;
	if (!data_lba) return;
	if (reading) {
		if (CdReadSync(1, 0) > 0) return;  /* ainda lendo */
		reading = 0;
		check_read();
	}
	/* a cada 600 quadros alterna: leituras do DADOS.BIN / faixa 2 tocando (CD-DA) */
	if ((frame / 600) & 1) {
		if (!playing && cd_tracks >= 2) {
			CdlATV atv;
			unsigned char mode = 0;
			atv.val0 = 0x80; atv.val1 = 0; atv.val2 = 0x80; atv.val3 = 0;
			CdMix(&atv);
			CdControl(CdlSetmode, &mode, 0);
			{
				unsigned char track = 0x02;  /* Play com o número da faixa (BCD) */
				CdControl(CdlPlay, &track, 0);
			}
			playing = 1;
			printf("cdda: faixa 2 tocando\n");
		}
		return;
	}
	if (playing) {
		CdControl(CdlPause, 0, 0);
		playing = 0;
		printf("cdda: pausa\n");
	}
	read_off = (read_index * 7) % (DATA_SECTORS - READ_SECTORS);
	CdIntToPos(data_lba + read_off, &loc);
	CdControl(CdlSetloc, (u_char *)&loc, 0);
	CdRead(READ_SECTORS, read_buf, CdlModeSpeed);
	reading = 1;
}

int main(void) {
	SVECTOR rot[NCUBES];
	VECTOR pos;
	MATRIX mtx, lmtx;
	POLY_FT4 *pol;
	int i, c, p, frame = 0, polys = 0, speed = 16, zoom = 900, camx = 0, camy = 0;
	int last_vs, vs0, last_btn = 0xFFFF, last_type = 0;
	unsigned short tpage = getTPage(2, 0, 640, 0);
	PADTYPE *pad;

	ResetGraph(0);
	SetDefDispEnv(&db[0].disp, 0, 0, SCREEN_XRES, SCREEN_YRES);
	SetDefDrawEnv(&db[0].draw, 0, 240, SCREEN_XRES, SCREEN_YRES);
	SetDefDispEnv(&db[1].disp, 0, 240, SCREEN_XRES, SCREEN_YRES);
	SetDefDrawEnv(&db[1].draw, 0, 0, SCREEN_XRES, SCREEN_YRES);
	for (i = 0; i < 2; i++) {
		setRGB0(&db[i].draw, 16, 24, 64);
		db[i].draw.isbg = 1;
		db[i].draw.dtd = 1;
	}
	PutDrawEnv(&db[0].draw);
	ClearOTagR(db[0].ot, OT_LEN);
	ClearOTagR(db[1].ot, OT_LEN);
	db_nextpri = db[0].p;

	InitGeom();
	gte_SetGeomOffset(SCREEN_XRES / 2, SCREEN_YRES / 2);
	gte_SetGeomScreen(SCREEN_XRES / 2);
	gte_SetBackColor(48, 48, 48);
	gte_SetColorMatrix(&color_mtx);

	InitPAD(&pad_buff[0][0], 34, &pad_buff[1][0], 34);
	StartPAD();
	ChangeClearPAD(0);
	FntLoad(960, 0);
	FntOpen(8, 8, 304, 64, 0, 256);

	printf("Indigo: disco de teste do PlayStation\n");
	init_sound();
	init_cd();
	for (c = 0; c < NCUBES; c++) { rot[c].vx = (short)(c * 300); rot[c].vy = (short)(c * 170); rot[c].vz = 0; }
	vs0 = last_vs = VSync(-1);

	for (;;) {
		pad = (PADTYPE *)&pad_buff[0][0];
		if (pad->stat == 0) {
			if (!(pad->btn & PAD_LEFT)) camx -= 4;
			if (!(pad->btn & PAD_RIGHT)) camx += 4;
			if (!(pad->btn & PAD_UP)) camy -= 4;
			if (!(pad->btn & PAD_DOWN)) camy += 4;
			if (!(pad->btn & PAD_L1)) zoom += 8;
			if (!(pad->btn & PAD_R1)) zoom -= 8;
			if (!(pad->btn & PAD_CROSS)) speed = 48;
			else if (!(pad->btn & PAD_CIRCLE)) speed = 0;
			else speed = 16;
			if (pad->type == 0x7) { camx += (pad->ls_x - 128) / 16; camy += (pad->ls_y - 128) / 16; }
			if (pad->btn != last_btn || pad->type != last_type) {
				printf("controle: tipo %X botoes %04X analog %d %d %d %d\n", pad->type, pad->btn, pad->ls_x, pad->ls_y, pad->rs_x, pad->rs_y);
				last_btn = pad->btn;
				last_type = pad->type;
			}
			if (zoom < 300) zoom = 300;
			if (zoom > 3000) zoom = 3000;
		}

		polys = 0;
		pol = (POLY_FT4 *)db_nextpri;
		for (c = 0; c < NCUBES; c++) {
			pos.vx = (c % NCX) * 110 - (NCX - 1) * 55 + camx;
			pos.vy = (c / NCX) * 110 - (NCY - 1) * 55 + camy;
			pos.vz = zoom;
			RotMatrix(&rot[c], &mtx);
			TransMatrix(&mtx, &pos);
			MulMatrix0(&light_mtx, &mtx, &lmtx);
			gte_SetRotMatrix(&mtx);
			gte_SetTransMatrix(&mtx);
			gte_SetLightMatrix(&lmtx);
			/* enquanto a faixa 2 toca, os cubos param (quadro estático para comparar com outro emulador) */
			if (!((frame / 600) & 1)) {
				rot[c].vx += (short)speed;
				rot[c].vy += (short)(speed / 2 + (c & 3));
			}
			for (i = 0; i < 6; i++) {
				gte_ldv3(&cube_verts[cube_indices[i].v0], &cube_verts[cube_indices[i].v1], &cube_verts[cube_indices[i].v2]);
				gte_rtpt();
				gte_nclip();
				gte_stopz(&p);
				if (p < 0) continue;
				gte_avsz4();
				gte_stotz(&p);
				if ((p >> 2) >= OT_LEN || p <= 0) continue;
				setPolyFT4(pol);
				gte_stsxy0(&pol->x0);
				gte_stsxy1(&pol->x1);
				gte_stsxy2(&pol->x2);
				gte_ldv0(&cube_verts[cube_indices[i].v3]);
				gte_rtps();
				gte_stsxy(&pol->x3);
				gte_ldrgb(&pol->r0);
				gte_ldv0(&cube_norms[i]);
				gte_ncs();
				gte_strgb(&pol->r0);
				setPolyFT4(pol);  /* o NCS devolve o código do primitivo no quarto byte */
				setUVWH(pol, 0, 0, 63, 63);
				pol->tpage = tpage;
				pol->clut = 0;
				addPrim(db[db_active].ot + (p >> 2), pol);
				pol++;
				polys++;
			}
		}
		db_nextpri = (char *)pol;

		cd_step(frame);
		if ((frame % 30) == 0) {
			/* som: alterna as vozes (esquerda, direita); KON/KOFF direto nos registradores (o SpuSetKey do
			 * PSn00bSDK de 2020 grava pelo registrador errado) */
			SPU_REG(0x18C) = (unsigned short)(1 << (((frame / 30) & 1) ^ 1));
			SPU_REG(0x188) = (unsigned short)(1 << ((frame / 30) & 1));
		}

		FntPrint(-1, "INDIGO PS1  QUADRO %d  POLIGONOS %d\n", frame, polys);
		FntPrint(-1, "CD: LEITURAS %d OK %d ERROS %d %s\n", read_index, read_ok, read_bad, playing ? "CD-DA" : "");
		if (pad->stat == 0)
			FntPrint(-1, "CONTROLE %02X BOTOES %04X\nANALOG %d %d %d %d\n", pad->type, pad->btn, pad->ls_x, pad->ls_y, pad->rs_x, pad->rs_y);
		FntFlush(-1);
		display();
		frame++;
		if ((frame % 300) == 0) {
			int vs = VSync(-1);
			printf("quadro %d: %d vblanks (%d nos ultimos 300), %d poligonos\n", frame, vs - vs0, vs - last_vs, polys);
			last_vs = vs;
		}
	}
	return 0;
}
