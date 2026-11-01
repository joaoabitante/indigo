; Teste de som do Neo Geo (Z80 + YM2610), montado pelo t7_neogeo.sh com o sdasz80 (SDCC). Não depende do 68000:
; toca em sequência, ~1 s cada, (1) onda quadrada do SSG, (2) nota FM, (3) ADPCM-A, (4) ADPCM-B e (5) o SSG
; ligado e desligado pela interrupção do temporizador B do YM2610 (testa a contagem dos temporizadores e a IRQ).
; Depois repete. O indigo-t7_neogeo compara o envelope de volume com o do Geolith.
; Ao ligar, o MVS roda o SM1 da BIOS; antes de trocar para o M1 do cartucho a BIOS põe o Z80 num laço na RAM e
; depois manda um comando (NMI), que aqui recomeça o teste. O resto da ROM é NOP (0x00) e o código fica em 0x7000.
        .area   SOM (ABS)
        .org    0x0000
        di
        jp      inicio

        .org    0x0038                  ; IRQ do YM2610 (temporizador B): alterna o volume do SSG
        push    af
        push    bc
        ld      a, (0xf800)
        xor     a, #0x0f
        ld      (0xf800), a
        ld      b, a
        ld      a, #0x08
        out     (0x04), a
        ld      a, b
        out     (0x05), a
        ld      a, #0x27                ; reinicia a bandeira do temporizador B
        out     (0x04), a
        ld      a, #0x2a
        out     (0x05), a
        pop     bc
        pop     af
        ei
        reti

        .org    0x0066                  ; NMI (comando do 68000): recomeça o teste. Ao trocar o SM1 pelo M1 do
        jp      inicio                  ; cartucho a BIOS deixa o Z80 num laço na RAM e manda um comando (NMI)

        .org    0x7000
; escreve o registrador d com o valor e (parte A: portas 4/5; parte B: portas 6/7)
ym_a:   ld      a, d
        out     (0x04), a
        ld      a, e
        out     (0x05), a
        ret
ym_b:   ld      a, d
        out     (0x06), a
        ld      a, e
        out     (0x07), a
        ret

; espera ~bc x 256 voltas (~0,43 s com bc = 1)
espera: ld      hl, #0
1$:     dec     hl
        ld      a, h
        or      a, l
        jr      nz, 1$
        dec     bc
        ld      a, b
        or      a, c
        jr      nz, espera
        ret

inicio: di
        im      1
        ld      sp, #0xfffc
        ; silêncio: SSG com volume 0, sem tom/ruído
        ld      de, #0x073f
        call    ym_a
        ld      de, #0x0800
        call    ym_a
        ; (1) SSG canal A: período 0x0100, volume 15
        ld      de, #0x0000
        call    ym_a
        ld      de, #0x0101
        call    ym_a
        ld      de, #0x073e
        call    ym_a
        ld      de, #0x080f
        call    ym_a
        ld      bc, #0x0002
        call    espera
        ld      de, #0x0800
        call    ym_a
        ld      bc, #0x0001
        call    espera

        ; (2) FM canal 2 (registradores +1 da parte A): algoritmo 7, só o operador 4 com volume
        ld      de, #0x31 * 256 + 0x01  ; DT/MUL op1..op4
        call    ym_a
        ld      de, #0x35 * 256 + 0x01
        call    ym_a
        ld      de, #0x39 * 256 + 0x01
        call    ym_a
        ld      de, #0x3d * 256 + 0x01
        call    ym_a
        ld      de, #0x41 * 256 + 0x7f  ; TL
        call    ym_a
        ld      de, #0x45 * 256 + 0x7f
        call    ym_a
        ld      de, #0x49 * 256 + 0x7f
        call    ym_a
        ld      de, #0x4d * 256 + 0x08
        call    ym_a
        ld      de, #0x51 * 256 + 0x1f  ; AR
        call    ym_a
        ld      de, #0x55 * 256 + 0x1f
        call    ym_a
        ld      de, #0x59 * 256 + 0x1f
        call    ym_a
        ld      de, #0x5d * 256 + 0x1f
        call    ym_a
        ld      de, #0x61 * 256 + 0x00  ; D1R
        call    ym_a
        ld      de, #0x65 * 256 + 0x00
        call    ym_a
        ld      de, #0x69 * 256 + 0x00
        call    ym_a
        ld      de, #0x6d * 256 + 0x00
        call    ym_a
        ld      de, #0x81 * 256 + 0x0f  ; D1L/RR
        call    ym_a
        ld      de, #0x85 * 256 + 0x0f
        call    ym_a
        ld      de, #0x89 * 256 + 0x0f
        call    ym_a
        ld      de, #0x8d * 256 + 0x0f
        call    ym_a
        ld      de, #0xb1 * 256 + 0x07  ; algoritmo 7
        call    ym_a
        ld      de, #0xb5 * 256 + 0xc0  ; esquerda e direita
        call    ym_a
        ld      de, #0xa5 * 256 + 0x22  ; bloco 4, fnum 0x269 (lá 440 Hz)
        call    ym_a
        ld      de, #0xa1 * 256 + 0x69
        call    ym_a
        ld      de, #0x28 * 256 + 0xf1  ; liga o canal 2
        call    ym_a
        ld      bc, #0x0002
        call    espera
        ld      de, #0x28 * 256 + 0x01
        call    ym_a
        ld      bc, #0x0001
        call    espera

        ; (3) ADPCM-A canal 1: amostra de 0x0000 a 0x0fff da V ROM, volume máximo
        ld      de, #0x0100 + 0x3f      ; volume total
        call    ym_b
        ld      de, #0x0800 + 0xdf      ; esquerda/direita + volume do canal
        call    ym_b
        ld      de, #0x1000
        call    ym_b
        ld      de, #0x1800
        call    ym_b
        ld      de, #0x200f
        call    ym_b
        ld      de, #0x2800
        call    ym_b
        ld      b, #3
2$:     push    bc
        ld      de, #0x0001             ; liga o canal 1
        call    ym_b
        ld      bc, #0x0001
        call    espera
        pop     bc
        djnz    2$
        ld      bc, #0x0001
        call    espera

        ; (4) ADPCM-B: 0x1000 a 0x1fff, delta-N 0x4000, volume 0xff, repetindo
        ld      de, #0x1100 + 0xc0
        call    ym_a
        ld      de, #0x1200 + 0x10
        call    ym_a
        ld      de, #0x1300
        call    ym_a
        ld      de, #0x1400 + 0x1f
        call    ym_a
        ld      de, #0x1500
        call    ym_a
        ld      de, #0x1900
        call    ym_a
        ld      de, #0x1a00 + 0x40
        call    ym_a
        ld      de, #0x1b00 + 0xff
        call    ym_a
        ld      de, #0x1000 + 0x90      ; começa, repetindo
        call    ym_a
        ld      bc, #0x0002
        call    espera
        ld      de, #0x1001             ; para
        call    ym_a
        ld      bc, #0x0001
        call    espera

        ; (5) temporizador B (valor 0x80) com IRQ alternando o volume do SSG
        ld      de, #0x0000
        call    ym_a
        ld      de, #0x0101
        call    ym_a
        ld      de, #0x073e
        call    ym_a
        ld      a, #0x0f
        ld      (0xf800), a
        ld      de, #0x080f
        call    ym_a
        ld      de, #0x2680             ; temporizador B
        call    ym_a
        ld      de, #0x272a             ; liga o B com IRQ (e reinicia a bandeira)
        call    ym_a
        ei
        ld      bc, #0x0004
        call    espera
        di
        ld      de, #0x2730             ; desliga os temporizadores
        call    ym_a
        ld      de, #0x0800
        call    ym_a
        ld      bc, #0x0001
        call    espera
        jp      inicio
