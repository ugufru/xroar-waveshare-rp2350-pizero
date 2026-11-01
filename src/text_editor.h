// text_editor.h: the on-screen text editor (PIZERO-146).
//
// Edits a text file on the SD card (settings.txt; autorun.txt in PIZERO-147)
// on the 32x16 CoCo text card, from inside the F12 overlay, with the machine
// paused. The editing rules are text_edit.h and text_edit_keys.h (host-
// tested); this is the screen, the file and the memory.

#ifndef TEXT_EDITOR_H
#define TEXT_EDITOR_H

#include <stdint.h>

typedef void (*text_editor_present_fn)(void);
// Called after a successful save, so the owner can re-read the file. It
// returns what the status row should say (NULL: just "SAVED").
typedef const char *(*text_editor_saved_fn)(const char *path);

void text_editor_init(text_editor_present_fn present, text_editor_saved_fn saved);

// Open `path`, or `template_text` when the file does not exist. The buffer
// is taken from the heap only while editing. Returns false if there is no
// memory for it (the message is then on screen), or if the file is too big
// or cannot be read (PIZERO-296: never opened cut short). `tidy` is for a
// settings file: it opens comments first, then the settings by name
// (PIZERO-181). `caps`: Caps Lock on at the start, as for autorun.txt and
// CoCo source, which look like the BASIC prompt (PIZERO-180); off for
// settings files and prose such as a README.
bool text_editor_open(const char *path, const char *title, const char *template_text, bool tidy,
                      bool caps);

// PIZERO-309: open text that is not a card file (a file inside a mounted
// disk) READ-ONLY: fill(ctx, buf, max, &len) puts it in the editor's own
// buffer and returns COCO_TEXT_OK or the reason it cannot (coco_boot.h).
// Its CR line ends are shown as lines, since nothing is saved back; only
// moving and ESC work. Returns COCO_TEXT_OK when open.
typedef int (*text_editor_fill_fn)(void *ctx, char *buf, uint32_t max, uint32_t *len);
int text_editor_open_view(const char *title, text_editor_fill_fn fill, void *ctx);

bool text_editor_is_open(void);

// Treat these keys as already held, so the key that opened the editor is
// not also typed into it.
void text_editor_hold(const uint8_t codes[6]);

// One HID report while open (modifier byte and six key codes). Closes the
// editor on ESC (or a second ESC, with unsaved changes).
void text_editor_key(uint8_t mods, const uint8_t codes[6], uint32_t frame);

// Once a frame while open: key repeat and redraw-if-changed.
void text_editor_frame(uint32_t frame);

#endif  // TEXT_EDITOR_H
