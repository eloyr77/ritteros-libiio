/*
 * A stand-in for RitterOS's ritteros.h, so that the library and iiotest
 * build on the Mac (make check-host): BSD sockets for RitterOS's, and
 * CommonCrypto's SHA-256. Only what libiio and iiotest use.
 */
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define SOCKET_TCP 2
#define SHA256_SIZE 32
struct sha256 { CC_SHA256_CTX c; };
static inline void sha256_init(struct sha256 *s) { CC_SHA256_Init(&s->c); }
static inline void sha256_update(struct sha256 *s, const void *d, size_t n) { CC_SHA256_Update(&s->c, d, (CC_LONG)n); }
static inline void sha256_final(struct sha256 *s, uint8_t *o) { CC_SHA256_Final(o, &s->c); }
static inline int ritteros_socket(int t) { (void)t; return socket(AF_INET, SOCK_STREAM, 0); }
static inline int ritteros_connect(int fd, uint32_t ip, uint16_t port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip) };
    return connect(fd, (struct sockaddr *)&a, sizeof(a));
}
static inline ssize_t ritteros_recvfrom(int fd, void *d, size_t n, uint32_t *ip, uint16_t *port, uint32_t ms)
{
    (void)ip, (void)port;
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, ms ? (int)ms : -1);
    if (r == 0) { errno = ETIMEDOUT; return -1; }
    if (r < 0) return -1;
    return recv(fd, d, n, 0);
}
static inline int ritteros_resolve(const char *name, uint32_t *ip)
{
    struct in_addr a;
    if (inet_pton(AF_INET, name, &a) == 1) { *ip = ntohl(a.s_addr); return 0; }
    errno = ENOENT;
    return -1;
}
#define PROGRAM_HELP(a, b)
