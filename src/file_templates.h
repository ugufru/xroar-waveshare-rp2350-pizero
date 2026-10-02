// file_templates.h: what the on-screen editor starts from when a file does
// not exist yet (PIZERO-146, PIZERO-147). The settings template is generated
// from the defaults (settings_template in settings.h); this is the rest.

#ifndef FILE_TEMPLATES_H
#define FILE_TEMPLATES_H

// Every line a comment, so saving it unchanged changes nothing: it only shows
// what can go in the file. Lines fit the 32-column screen.
#define AUTORUN_TEMPLATE \
    "# AUTORUN.TXT: WHAT RUNS AT\n" \
    "# BOOT. SEE AUTORUN.MD.\n" \
    "# LINES STARTING WITH @ SET UP\n" \
    "# THE MACHINE; OTHER LINES ARE\n" \
    "# TYPED AT THE BASIC PROMPT.\n" \
    "#\n" \
    "# @CART POLARIS.CCC\n" \
    "# @DIRECT ORBIT.BIN\n" \
    "# RUN\"HELLO\"\n"

// PIZERO-154: a game's own settings file (NAME.TXT beside it). All comments,
// so saving it unchanged changes nothing.
#define GAME_TEMPLATE \
    "# SETTINGS FOR THIS GAME ONLY.\n" \
    "# THEY OVERRIDE SETTINGS.TXT\n" \
    "# WHILE IT RUNS. SAME NAMES:\n" \
    "#\n" \
    "# artifact_colors = swapped\n" \
    "# volume = 8\n" \
    "# joystick_swap = on\n" \
    "# color_green = #1ED01E\n"

#endif  // FILE_TEMPLATES_H
