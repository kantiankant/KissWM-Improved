#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ipc.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: kisswmctl <command> [args]\n"
            "commands:\n"
            "  fullscreen\n"
            "  kill\n"
            "  global\n"
            "  focus  left|right|up|down\n"
            "  swap   left|right|up|down\n"
            "  tag    N\n"
            "  move   N\n"
            "  reload\n"
            "  quit\n"
            "  status\n");
        return 1;
    }


    char sockpath[IPC_SOCK_MAX];
    ipc_socket_path(sockpath, getenv("DISPLAY"));


    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("kisswmctl: socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("kisswmctl: connect");
        fprintf(stderr, "kisswmctl: is kisswm running?\n");
        close(fd);
        return 1;
    }


    char msg[IPC_MSG_MAX];
    int  off = 0;
    for (int i = 1; i < argc && off < IPC_MSG_MAX - 2; i++) {
        if (i > 1) msg[off++] = ' ';
        const char *a = argv[i];
        while (*a && off < IPC_MSG_MAX - 2) msg[off++] = *a++;
    }
    msg[off++] = '\n';
    msg[off]   = '\0';

    if (write(fd, msg, (size_t)off) != off) {
        perror("kisswmctl: write");
        close(fd);
        return 1;
    }


    char resp[IPC_RESP_MAX];
    int  n = (int)read(fd, resp, sizeof(resp) - 1);
    close(fd);

    if (n <= 0) {
        fprintf(stderr, "kisswmctl: no response from kisswm\n");
        return 1;
    }
    resp[n] = '\0';


    char *nl = strchr(resp, '\n');
    if (nl) *nl = '\0';

    printf("%s\n", resp);


    return (strncmp(resp, "err", 3) == 0) ? 1 : 0;
}
