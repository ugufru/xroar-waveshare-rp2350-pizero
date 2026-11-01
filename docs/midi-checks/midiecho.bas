' PIZERO-287: the MIDI Pak end to end, without Lyra. Sets the 6850 up as
' Lyra and CoCo MIDI 3 do ($03 master reset, then $15: divide by 16, 8N1),
' sends middle C on and off by polling TDRE as they do, then echoes every
' byte it receives back out. On the computer the board's USB MIDI port shows
' 90 3C 64, 80 3C 00, then each byte sent to it comes back. Needs
' midi_pak = on (the CoCo Zero's default).
' Type it with: python3 scripts/serial_type.py docs/midi-checks/midiecho.bas
NEW
10 POKE &HFF6E,3:POKE &HFF6E,21:CLS:PRINT"MIDI PAK: SENDING C4"
20 FOR I=1 TO 6:READ B
30 IF (PEEK(&HFF6E) AND 2)=0 THEN 30
40 POKE &HFF6F,B:NEXT
50 PRINT"ECHOING WHAT ARRIVES"
60 IF (PEEK(&HFF6E) AND 1)=0 THEN 60
70 B=PEEK(&HFF6F):PRINT HEX$(B);" ";
80 IF (PEEK(&HFF6E) AND 2)=0 THEN 80
90 POKE &HFF6F,B:GOTO 60
100 DATA 144,60,100,128,60,0
RUN
