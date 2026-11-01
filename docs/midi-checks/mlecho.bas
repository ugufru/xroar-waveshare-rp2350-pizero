' PIZERO-287: the MIDI Pak's input, at MIDI's full rate. Sets the 6850 up as
' Lyra and CoCo MIDI 3 do ($03, then $15), sends middle C on and off, then
' runs mlecho.asm (22 bytes at $3F00), which echoes every byte it receives
' straight back. Send a note to the board's USB MIDI port and it comes back
' unchanged. Reset to leave. Needs midi_pak = on (the CoCo Zero's default).
' Type it with: python3 scripts/serial_type.py docs/midi-checks/mlecho.bas
NEW
10 CLEAR 200,&H3EFF:POKE &HFF6E,3:POKE &HFF6E,21
20 FOR I=1 TO 6:READ B
30 IF (PEEK(&HFF6E) AND 2)=0 THEN 30
40 POKE &HFF6F,B:NEXT
50 FOR A=&H3F00 TO &H3F15:READ B:POKE A,B:NEXT
60 CLS:PRINT"MIDI PAK: ECHOING (RESET TO END)":EXEC &H3F00
70 DATA 144,60,100,128,60,0
80 DATA 182,255,110,133,1,39,249,246,255,111,182,255,110,133,2,39,249,247,255,111,32,234
RUN
