#ifndef PARSER_H
#define PARSER_H

#include <X11/Xlib.h>
#include <X11/keysym.h>

void do_kill(void);
void do_quit(void);
void do_focus_left(void);
void do_focus_right(void);
void do_focus_up(void);
void do_focus_down(void);
void do_swap_left(void);
void do_swap_right(void);
void do_swap_up(void);
void do_swap_down(void);
void do_cycle_all(void);
void do_cycle_tag(void);
void do_global(void);

typedef struct {
    unsigned int  mod;
    KeySym        key;
    void        (*fn)(void);
    char         *cmd;
} Keybind;

typedef struct {
    unsigned int  mod;
    KeySym        key;
    int           tag;
    int           move;
} TagBind;

typedef struct {

    int           ntags;


    Keybind      *binds;
    int           nbinds;


    TagBind      *tagbinds;
    int           ntagbinds;
} Config;

Config *config_parse(const char *path);

void config_free(Config *cfg);

#define CONFIG_PATH_SUFFIX "/.config/kisswm/kisswmrc"

#endif
