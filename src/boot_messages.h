// boot_messages.h — the words on the diagnostic pages (PIZERO-92).
//
// They live here rather than inline in main.cpp so the host tests can check
// what the reader will actually see: that every page wraps inside 32 columns,
// fits the card, and that the two ROM failures do not read alike. A message
// that overflows is invisible to us and fatal to the reader, who has no serial
// console and no second guess.
//
// House style for these pages, learned from the failure they exist for:
//   * say what happened, then what to do, in that order
//   * name exact files and folders; that IS the fix
//   * no error codes, no jargon, no blame
//   * upper case, because the machine has no lower case

#ifndef BOOT_MESSAGES_H
#define BOOT_MESSAGES_H

#define MSG_NOSD_TITLE  "NO SD CARD"
#define MSG_NOSD_BODY   "THE CARD IS MISSING OR CANNOT BE READ. PUT IN A " \
                        "FAT32 CARD HOLDING /COCO/ROMS/BAS12.ROM AND SWITCH " \
                        "THE POWER OFF AND ON AGAIN."
#define MSG_NOSD_DETAIL "CHECK THE CARD IS PUSHED IN UNTIL IT CLICKS."

#define MSG_NOROM_TITLE  "NO ROM FOUND"
#define MSG_NOROM_BODY   "COPY BAS12.ROM INTO THE FOLDER /COCO/ROMS/ ON THE " \
                         "SD CARD, THEN SWITCH THE POWER OFF AND ON AGAIN. " \
                         "SEE THE CARD IN THE BOX FOR WHERE TO GET IT."
#define MSG_NOROM_DETAIL "THE CARD READS OK. ONLY THE ROM FILE IS MISSING."

#define MSG_BADROM_TITLE "ROM FILE IS DAMAGED"
#define MSG_BADROM_BODY  "BAS12.ROM IS ON THE CARD BUT IS THE WRONG SIZE, SO " \
                         "IT CANNOT BE A COLOR BASIC ROM. COPY IT AGAIN, AND " \
                         "CHECK THE COPY FINISHED BEFORE REMOVING THE CARD."

#define MSG_INIT_TITLE  "EMULATOR DID NOT START"
#define MSG_INIT_BODY   "THE ROM LOADED BUT THE MACHINE WOULD NOT START. " \
                        "THIS IS NOT SOMETHING YOU CAN FIX ON THE CARD. " \
                        "PLEASE REPORT IT WITH THE DETAIL BELOW."
#define MSG_INIT_DETAIL "ROM READ OK, 8192 BYTES. FAULT IS IN THE FIRMWARE."

#define MSG_CBONLY_TITLE  "COLOR BASIC ONLY"
#define MSG_CBONLY_BODY   "EXTBAS11.ROM IS MISSING, SO EXTENDED AND DISK " \
                          "BASIC ARE NOT AVAILABLE AND DISK COMMANDS WILL " \
                          "FAIL. COPY IT TO /COCO/ROMS/ TO GET THEM."
#define MSG_CBONLY_DETAIL "STARTING IN A MOMENT."

#endif  // BOOT_MESSAGES_H
