#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"

static const struct { const char *name; void (*fn)(void); } builtins[] = {
    { "kill",        do_kill        },
    { "quit",        do_quit        },
    { "focus-left",  do_focus_left  },
    { "focus-right", do_focus_right },
    { "focus-up",    do_focus_up    },
    { "focus-down",  do_focus_down  },
    { "swap-left",   do_swap_left   },
    { "swap-right",  do_swap_right  },
    { "swap-up",     do_swap_up     },
    { "swap-down",   do_swap_down   },
    { "cycle-all",   do_cycle_all   },
    { "cycle-tag",   do_cycle_tag   },
    { "global",      do_global      },
};
#define NBUILTINS (int)(sizeof(builtins) / sizeof(builtins[0]))

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e - 1))) e--;
    *e = '\0';
    return s;
}

static char *safe_strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char  *p   = malloc(len);
    if (!p) { fprintf(stderr, "kisswm: parser: out of memory\n"); exit(1); }
    memcpy(p, s, len);
    return p;
}

static int parse_lhs(const char *lhs, unsigned int *mods_out, KeySym *key_out)
{
    *mods_out = 0;
    *key_out  = NoSymbol;


    if (strncmp(lhs, "XF86-", 5) == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "XF86%s", lhs + 5);
        KeySym ks = XStringToKeysym(buf);
        if (ks == NoSymbol) {
            fprintf(stderr, "kisswm: parser: unknown XF86 key: %s\n", lhs);
            return 0;
        }
        *key_out = ks;
        return 1;
    }


    char buf[64];
    snprintf(buf, sizeof(buf), "%s", lhs);


    char *tokens[16];
    int   ntok = 0;
    char *tok  = strtok(buf, "-");
    while (tok && ntok < 16) { tokens[ntok++] = tok; tok = strtok(NULL, "-"); }
    if (ntok == 0) return 0;


    for (int i = 0; i < ntok - 1; i++) {
        if (strcmp(tokens[i], "M") == 0 || strcmp(tokens[i], "4") == 0)
            *mods_out |= Mod4Mask;
        else if (strcmp(tokens[i], "A") == 0)
            *mods_out |= Mod1Mask;
        else if (strcmp(tokens[i], "C") == 0)
            *mods_out |= ControlMask;
        else if (strcmp(tokens[i], "S") == 0)
            *mods_out |= ShiftMask;
        else {
            fprintf(stderr, "kisswm: parser: unknown modifier: %s\n", tokens[i]);
            return 0;
        }
    }


    const char *keyname = tokens[ntok - 1];


    char keybuf[32];
    if (strlen(keyname) == 1) {
        keybuf[0] = keyname[0];
        keybuf[1] = '\0';
    } else {

        snprintf(keybuf, sizeof(keybuf), "%s", keyname);
    }

    KeySym ks = XStringToKeysym(keybuf);
    if (ks == NoSymbol) {

        keybuf[0] = (char)toupper((unsigned char)keybuf[0]);
        ks = XStringToKeysym(keybuf);
    }
    if (ks == NoSymbol) {
        fprintf(stderr, "kisswm: parser: unknown key: %s\n", keyname);
        return 0;
    }

    *key_out = ks;
    return 1;
}

typedef struct {
    Keybind *data;
    int      len;
    int      cap;
} BindArr;

typedef struct {
    TagBind *data;
    int      len;
    int      cap;
} TagBindArr;

static void bind_push(BindArr *a, Keybind b)
{
    if (a->len == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->data = realloc(a->data, (size_t)a->cap * sizeof(Keybind));
        if (!a->data) { fprintf(stderr, "kisswm: parser: oom\n"); exit(1); }
    }
    a->data[a->len++] = b;
}

static void tagbind_push(TagBindArr *a, TagBind b)
{
    if (a->len == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->data = realloc(a->data, (size_t)a->cap * sizeof(TagBind));
        if (!a->data) { fprintf(stderr, "kisswm: parser: oom\n"); exit(1); }
    }
    a->data[a->len++] = b;
}

static int handle_line(const char *raw, int lineno,
                       BindArr *binds, TagBindArr *tagbinds, int *ntags)
{

    char line[512];
    snprintf(line, sizeof(line), "%s", raw);
    char *s = trim(line);


    if (*s == '\0' || *s == '#') return 1;


    char *eq = strchr(s, '=');
    if (!eq) {
        fprintf(stderr, "kisswm: parser: line %d: missing '='\n", lineno);
        return 0;
    }
    *eq = '\0';
    char *lhs = trim(s);
    char *rhs = trim(eq + 1);


    if (strcmp(lhs, "tag") == 0) {
        int n = atoi(rhs);
        if (n < 1 || n > 32) {
            fprintf(stderr, "kisswm: parser: line %d: tag count must be 1-32\n", lineno);
            return 0;
        }
        *ntags = n;
        return 1;
    }


    unsigned int mods;
    KeySym       key;
    if (!parse_lhs(lhs, &mods, &key)) {
        fprintf(stderr, "kisswm: parser: line %d: bad keybind LHS: %s\n", lineno, lhs);
        return 0;
    }


    if (strncmp(rhs, "tag-", 4) == 0) {
        int idx = atoi(rhs + 4) - 1;
        if (idx < 0) {
            fprintf(stderr, "kisswm: parser: line %d: bad tag index: %s\n", lineno, rhs);
            return 0;
        }
        TagBind tb = { mods, key, idx, 0 };
        tagbind_push(tagbinds, tb);
        return 1;
    }
    if (strncmp(rhs, "move-", 5) == 0) {
        int idx = atoi(rhs + 5) - 1;
        if (idx < 0) {
            fprintf(stderr, "kisswm: parser: line %d: bad tag index: %s\n", lineno, rhs);
            return 0;
        }
        TagBind tb = { mods, key, idx, 1 };
        tagbind_push(tagbinds, tb);
        return 1;
    }


    for (int i = 0; i < NBUILTINS; i++) {
        if (strcmp(rhs, builtins[i].name) == 0) {
            Keybind b = { mods, key, builtins[i].fn, NULL };
            bind_push(binds, b);
            return 1;
        }
    }


    Keybind b = { mods, key, NULL, safe_strdup(rhs) };
    bind_push(binds, b);
    return 1;
}

Config *config_parse(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "kisswm: cannot open config: %s\n", path);
        return NULL;
    }

    BindArr    binds    = { NULL, 0, 0 };
    TagBindArr tagbinds = { NULL, 0, 0 };
    int        ntags    = 9;
    int        lineno   = 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        lineno++;

        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        handle_line(line, lineno, &binds, &tagbinds, &ntags);

    }
    fclose(f);

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) { fprintf(stderr, "kisswm: parser: oom\n"); exit(1); }

    cfg->ntags     = ntags;
    cfg->binds     = binds.data;
    cfg->nbinds    = binds.len;
    cfg->tagbinds  = tagbinds.data;
    cfg->ntagbinds = tagbinds.len;

    return cfg;
}

void config_free(Config *cfg)
{
    if (!cfg) return;
    for (int i = 0; i < cfg->nbinds; i++)
        free(cfg->binds[i].cmd);
    free(cfg->binds);
    free(cfg->tagbinds);
    free(cfg);
}
