/* UDP for the two-PC game: see net.h. */
#include "udp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sys_sock;
#define SYS_BAD INVALID_SOCKET
#define close_sock closesocket
static int last_err(void) { return WSAGetLastError(); }
#else
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sys_sock;
#define SYS_BAD (-1)
#define close_sock close
static int last_err(void) { return errno; }
#endif

static char err_text[256];

static void set_err(const char *what, int code)
{
#ifdef _WIN32
    char msg[160] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)code, 0, msg, sizeof msg,
                   NULL);
    size_t l = strlen(msg);
    while (l && (msg[l - 1] == '\n' || msg[l - 1] == '\r' || msg[l - 1] == ' ' || msg[l - 1] == '.'))
        msg[--l] = 0;
    snprintf(err_text, sizeof err_text, "%.100s: %.120s (%d)", what, msg[0] ? msg : "error", code);
#else
    snprintf(err_text, sizeof err_text, "%.100s: %.120s", what, strerror(code));
#endif
}

const char *udp_error(void)
{
    return err_text;
}

int udp_init(void)
{
#ifdef _WIN32
    WSADATA w;
    int r = WSAStartup(MAKEWORD(2, 2), &w);
    if (r) {
        set_err("Winsock", r);
        return -1;
    }
#endif
    return 0;
}

void udp_quit(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

udp_sock udp_open(int port, const udp_addr *like)
{
    int family = like && like->len ? ((const struct sockaddr *)like->raw)->sa_family : AF_INET;
    sys_sock s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SYS_BAD) {
        set_err("socket", last_err());
        return UDP_NONE;
    }
    struct sockaddr_storage a;
    memset(&a, 0, sizeof a);
    socklen_t len;
    if (family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&a;
        a6->sin6_family = AF_INET6;
        a6->sin6_addr = in6addr_any;
        a6->sin6_port = htons((unsigned short)port);
        len = sizeof *a6;
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&a;
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(INADDR_ANY);
        a4->sin_port = htons((unsigned short)port);
        len = sizeof *a4;
    }
    if (bind(s, (struct sockaddr *)&a, len)) {
        char what[64];
        snprintf(what, sizeof what, "port %d", port);
        set_err(what, last_err());
        close_sock(s);
        return UDP_NONE;
    }
    /* room for a burst of datagrams (a joining PC's game, session.c): Windows
     * gives 64 KiB and Linux about 200; the system may give less than this */
    int room = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&room, sizeof room);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&room, sizeof room);
    return (udp_sock)s;
}

int udp_room(udp_sock s)
{
    int room = 0;
    socklen_t len = sizeof room;
    if (getsockopt((sys_sock)s, SOL_SOCKET, SO_RCVBUF, (char *)&room, &len))
        return -1;
    return room;
}

void udp_close(udp_sock s)
{
    if (s != UDP_NONE)
        close_sock((sys_sock)s);
}

int udp_resolve(const char *text, int default_port, udp_addr *out)
{
    char host[256], port[16];
    const char *p = text, *colon = NULL;
    size_t hl;
    memset(out, 0, sizeof *out);
    while (*p == ' ')
        p++;
    if (*p == '[') {                        /* [v6]:port */
        const char *close = strchr(p, ']');
        if (!close) {
            snprintf(err_text, sizeof err_text, "%.120s: no ] after the IPv6 address", text);
            return -1;
        }
        hl = (size_t)(close - p - 1);
        if (hl >= sizeof host)
            hl = sizeof host - 1;
        memcpy(host, p + 1, hl);
        host[hl] = 0;
        colon = close[1] == ':' ? close + 1 : NULL;
    } else {
        colon = strchr(p, ':');
        if (colon && strchr(colon + 1, ':'))
            colon = NULL;                   /* a bare IPv6 address: no port */
        hl = colon ? (size_t)(colon - p) : strlen(p);
        if (hl >= sizeof host)
            hl = sizeof host - 1;
        memcpy(host, p, hl);
        host[hl] = 0;
    }
    while (hl && host[hl - 1] == ' ')
        host[--hl] = 0;
    if (!host[0]) {
        snprintf(err_text, sizeof err_text, "no address given");
        return -1;
    }
    snprintf(port, sizeof port, "%d", colon ? atoi(colon + 1) : default_port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    int r = getaddrinfo(host, port, &hints, &res);
    if (r || !res) {
#ifdef _WIN32
        set_err(host, r ? r : last_err());
#else
        snprintf(err_text, sizeof err_text, "%.120s: %.100s", host, gai_strerror(r));
#endif
        return -1;
    }
    const struct addrinfo *pick = res;
    for (const struct addrinfo *i = res; i; i = i->ai_next)
        if (i->ai_family == AF_INET) {
            pick = i;
            break;
        }
    if (pick->ai_addrlen > sizeof out->raw) {
        freeaddrinfo(res);
        snprintf(err_text, sizeof err_text, "%.120s: an address too long", host);
        return -1;
    }
    memcpy(out->raw, pick->ai_addr, pick->ai_addrlen);
    out->len = (int)pick->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

int udp_send(udp_sock s, const udp_addr *to, const void *buf, size_t n)
{
    if (sendto((sys_sock)s, (const char *)buf, (int)n, 0, (const struct sockaddr *)to->raw, (socklen_t)to->len) < 0) {
        set_err("send", last_err());
        return -1;
    }
    return 0;
}

int udp_recv(udp_sock s, void *buf, size_t max, udp_addr *from, int timeout_ms)
{
#ifdef _WIN32
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET((sys_sock)s, &rd);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select(0, &rd, NULL, NULL, &tv);
#else
    struct pollfd pf = { (int)s, POLLIN, 0 };
    int r = poll(&pf, 1, timeout_ms);
#endif
    if (r < 0) {
#ifndef _WIN32
        if (errno == EINTR)
            return 0;
#endif
        set_err("wait", last_err());
        return -1;
    }
    if (r == 0)
        return 0;
    struct sockaddr_storage a;
    socklen_t al = sizeof a;
    int n = (int)recvfrom((sys_sock)s, (char *)buf, (int)max, 0, (struct sockaddr *)&a, &al);
    if (n < 0) {
        int e = last_err();
#ifdef _WIN32
        if (e == WSAECONNRESET || e == WSAEMSGSIZE)     /* an ICMP "no one there" for an earlier send; too big */
            return 0;
#else
        if (e == ECONNREFUSED || e == EINTR || e == EAGAIN)
            return 0;
#endif
        set_err("receive", e);
        return -1;
    }
    if (from) {
        memset(from, 0, sizeof *from);
        memcpy(from->raw, &a, al <= (socklen_t)sizeof from->raw ? (size_t)al : sizeof from->raw);
        from->len = (int)al;
    }
    return n;
}

int udp_same(const udp_addr *a, const udp_addr *b)
{
    const struct sockaddr *x = (const struct sockaddr *)a->raw, *y = (const struct sockaddr *)b->raw;
    if (!a->len || !b->len || x->sa_family != y->sa_family)
        return 0;
    if (x->sa_family == AF_INET) {
        const struct sockaddr_in *p = (const void *)x, *q = (const void *)y;
        return p->sin_port == q->sin_port && p->sin_addr.s_addr == q->sin_addr.s_addr;
    }
    if (x->sa_family == AF_INET6) {
        const struct sockaddr_in6 *p = (const void *)x, *q = (const void *)y;
        return p->sin6_port == q->sin6_port && !memcmp(&p->sin6_addr, &q->sin6_addr, sizeof p->sin6_addr);
    }
    return 0;
}

const char *udp_text(const udp_addr *a, char *buf, size_t n)
{
    char host[96] = "?", port[16] = "?";
    if (a->len)
        getnameinfo((const struct sockaddr *)a->raw, (socklen_t)a->len, host, sizeof host, port, sizeof port,
                    NI_NUMERICHOST | NI_NUMERICSERV);
    if (a->len && ((const struct sockaddr *)a->raw)->sa_family == AF_INET6)
        snprintf(buf, n, "[%s]:%s", host, port);
    else
        snprintf(buf, n, "%s:%s", host, port);
    return buf;
}

const char *udp_local_ip(char *buf, size_t n)
{
    /* a UDP socket "connected" to an address outside: the system picks the
     * route and the address it would send from; connect sends nothing */
    buf[0] = 0;
    sys_sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SYS_BAD)
        return buf;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = htonl(0xc0000201u);    /* 192.0.2.1 (TEST-NET-1): routed, never reached */
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    if (!connect(s, (struct sockaddr *)&to, sizeof to) && !getsockname(s, (struct sockaddr *)&me, &ml))
        getnameinfo((struct sockaddr *)&me, ml, buf, (socklen_t)n, NULL, 0, NI_NUMERICHOST);
    close_sock(s);
    if (!strcmp(buf, "0.0.0.0"))
        buf[0] = 0;
    return buf;
}
