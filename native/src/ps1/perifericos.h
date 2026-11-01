// Periféricos do PS1 feitos em trabalhos separados da etapa 9 e ligados ao System na integração:
//  - GTE (COP2): readData/writeData/readCtrl/writeCtrl e command(), que devolve os ciclos do comando;
//  - SPU: read/write, run, take/available (som a 44,1 kHz), onIrq (IRQ9), DMA4 e pushCdAudio (áudio do CD);
//  - CD-ROM: read/write, run, onIrq (IRQ2), onAudio (para a SPU), DMA3 e o disco por setDisc;
//  - MDEC: read/write, run e os pedidos de DMA0 (entrada) e DMA1 (saída).
#pragma once

#include "cdrom.h"
#include "gte.h"
#include "mdec.h"
#include "spu.h"
