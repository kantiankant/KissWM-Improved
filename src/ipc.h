#ifndef IPC_H
#define IPC_H

#define IPC_SOCK_PREFIX "/tmp/kisswm-"
#define IPC_SOCK_SUFFIX ".sock"
#define IPC_SOCK_MAX    128

#define IPC_MSG_MAX  256
#define IPC_RESP_MAX 256

static inline char *ipc_socket_path(char *buf, const char *display)
{

    const char *d = display ? display : ":0";
    if (*d == ':') d++;
    int i = 0;
    buf[i] = '\0';

    const char *p = IPC_SOCK_PREFIX;
    while (*p && i < IPC_SOCK_MAX - 2) buf[i++] = *p++;

    while (*d && i < IPC_SOCK_MAX - 2) {
        buf[i++] = (*d == '/') ? '-' : *d;
        d++;
    }

    const char *s = IPC_SOCK_SUFFIX;
    while (*s && i < IPC_SOCK_MAX - 2) buf[i++] = *s++;
    buf[i] = '\0';
    return buf;
}

#endif
