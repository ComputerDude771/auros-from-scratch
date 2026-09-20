/* aurctl — talk to aurinit.
 *
 * A datagram client for the init control socket. It binds its own
 * abstract-free socket in /run so init has a return address to reply to.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#define CTRL_SOCKET "/run/auros/init.sock"

static void usage(void)
{
    fputs("aurctl — control aurinit\n\n"
          "  aurctl status              list services and their state\n"
          "  aurctl start NAME\n"
          "  aurctl stop NAME\n"
          "  aurctl restart NAME\n"
          "  aurctl reboot\n"
          "  aurctl poweroff\n", stderr);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }

    char msg[256] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(msg, " ", sizeof msg - strlen(msg) - 1);
        strncat(msg, argv[i], sizeof msg - strlen(msg) - 1);
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    /* Bind a unique reply address; init sends its answer back here. */
    struct sockaddr_un me = { .sun_family = AF_UNIX };
    snprintf(me.sun_path, sizeof me.sun_path, "/run/auros/aurctl.%d", getpid());
    unlink(me.sun_path);
    if (bind(fd, (struct sockaddr *)&me, sizeof me) < 0) { perror("bind"); return 1; }

    struct sockaddr_un to = { .sun_family = AF_UNIX };
    snprintf(to.sun_path, sizeof to.sun_path, "%s", CTRL_SOCKET);
    if (sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof to) < 0) {
        fprintf(stderr, "aurctl: init is not listening (%s)\n", strerror(errno));
        unlink(me.sun_path);
        return 1;
    }

    /* reboot/poweroff never reply — init is already tearing down. */
    struct timeval tv = { .tv_sec = 3 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    unlink(me.sun_path);
    if (n > 0) { buf[n] = '\0'; fputs(buf, stdout); return 0; }
    return 0;
}
