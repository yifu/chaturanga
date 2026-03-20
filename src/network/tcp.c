#include "chess_app/network_tcp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

bool chess_tcp_listener_open(ChessTcpListener *listener, uint16_t requested_port)
{
    int fd = -1;
    struct sockaddr_in addr;
    struct sockaddr_in bound_addr;
    socklen_t bound_len = sizeof(bound_addr);

    if (!listener) {
        return false;
    }

    listener->fd = -1;
    listener->port = 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(requested_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    if (listen(fd, 8) != 0) {
        close(fd);
        return false;
    }

    memset(&bound_addr, 0, sizeof(bound_addr));
    if (getsockname(fd, (struct sockaddr *)&bound_addr, &bound_len) != 0) {
        close(fd);
        return false;
    }

    listener->fd = fd;
    listener->port = ntohs(bound_addr.sin_port);
    return true;
}

void chess_tcp_listener_close(ChessTcpListener *listener)
{
    if (!listener) {
        return;
    }

    if (listener->fd >= 0) {
        close(listener->fd);
    }

    listener->fd = -1;
    listener->port = 0;
}
