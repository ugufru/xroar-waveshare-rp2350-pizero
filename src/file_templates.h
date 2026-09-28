// file_templates.h: what the on-screen editor starts from when a file does
// not exist yet (PIZERO-146, PIZERO-147). The settings template is generated
// from the defaults (settings_template in settings.h); this is the rest.

#ifndef FILE_TEMPLATES_H
#define FILE_TEMPLATES_H

// Every line a comment, so saving it unchanged changes nothing: it only shows
// what can go in the file. Lines fit the 32-column screen.
#define AUTORUN_TEMPLATE \
    "# AUTORUN.TXT: WHAT RUNS AT\n" \
    "# POWER-ON. SEE AUTORUN.MD.\n" \
    "# LINES STARTING WITH @ SET UP\n" \
    "# THE MACHINE; OTHER LINES ARE\n" \
    "# TYPED AT THE BASIC PROMPT.\n" \
    "#\n" \
    "# @DISK GAMES.DSK\n" \
    "# @CART POLARIS.CCC\n" \
    "# @DIRECT ORBIT.BIN\n" \
    "# RUN\"HELLO\"\n"

#endif  // FILE_TEMPLATES_H
