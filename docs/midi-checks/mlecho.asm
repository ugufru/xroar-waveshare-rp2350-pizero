; PIZERO-287: echo every byte the MIDI Pak receives straight back out, fast
; enough for back-to-back MIDI bytes (one every 320 us). BASIC's PEEK loop is
; not: three bytes of one message land before it reads the first, and the
; 6850 loses the rest to overrun, as a real pak would.
;
; Assemble: lwasm --format=raw -o mlecho.raw mlecho.asm
; mlecho.bas POKEs these bytes at $3F00 and EXECs them; BREAK does not stop
; it (it never returns), so reset the machine to leave.

ACIASR  equ     $FF6E           ; status (read) / control (write)
ACIADR  equ     $FF6F           ; receive (read) / transmit (write)

        org     $3F00
wait_rx lda     ACIASR
        bita    #$01            ; RDRF: a byte has arrived
        beq     wait_rx
        ldb     ACIADR          ; take it (clears RDRF)
wait_tx lda     ACIASR
        bita    #$02            ; TDRE: room to send
        beq     wait_tx
        stb     ACIADR          ; send it back
        bra     wait_rx
