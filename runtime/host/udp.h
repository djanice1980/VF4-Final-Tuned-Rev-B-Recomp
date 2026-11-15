/* UDP for the two-PC game (docs/design.md section 46): the system's own
 * sockets (Winsock on Windows, BSD sockets elsewhere). Nothing here knows
 * about the game; session.c speaks over it. A host listens on IPv4 (a
 * LAN's); an address to join may be IPv4 or IPv6. */
#ifndef VF4FT_UDP_H
#define VF4FT_UDP_H

#include <stddef.h>
#include <stdint.h>

#define UDP_PORT 29404              /* the host's port unless one is chosen */

typedef intptr_t udp_sock;          /* a socket, or UDP_NONE */
#define UDP_NONE ((udp_sock)-1)

typedef struct udp_addr {
    unsigned char raw[128];         /* a sockaddr_storage's bytes */
    int len;                        /* 0: none */
} udp_addr;

/* once before anything else: 0, or -1 (Winsock would not start) */
int udp_init(void);
void udp_quit(void);

/* a UDP socket on port (0: any free one), of the address family of `like`
 * (NULL: IPv4). UDP_NONE when it cannot be had (udp_error) */
udp_sock udp_open(int port, const udp_addr *like);
void udp_close(udp_sock s);

/* the socket's receive buffer in bytes as the system gave it (-1 unknown) */
int udp_room(udp_sock s);

/* "host", "host:port", "[v6]:port" or a bare IPv6 address into an address
 * (an IPv4 one when the name has both); default_port when the text names
 * none. 0, or -1 (udp_error) */
int udp_resolve(const char *text, int default_port, udp_addr *out);

/* one datagram: 0 sent, -1 not (udp_error) */
int udp_send(udp_sock s, const udp_addr *to, const void *buf, size_t n);

/* waits up to timeout_ms for a datagram: its length (> 0) and where it came
 * from; 0 when none came (or the system reported that an earlier one found
 * no one there); -1 on an error (udp_error) */
int udp_recv(udp_sock s, void *buf, size_t max, udp_addr *from, int timeout_ms);

/* 1 when a and b are the same address and port */
int udp_same(const udp_addr *a, const udp_addr *b);

/* "192.168.1.20:29404" or "[fe80::1]:29404" */
const char *udp_text(const udp_addr *a, char *buf, size_t n);

/* this PC's IPv4 address on its network, the one the system would send
 * from to reach the internet (found without sending anything), or "" when
 * there is no network */
const char *udp_local_ip(char *buf, size_t n);

/* what went wrong last, as text */
const char *udp_error(void);

#endif
