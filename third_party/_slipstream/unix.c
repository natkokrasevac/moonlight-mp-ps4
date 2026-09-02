#define _DEFAULT_SOURCE

#include <enet/enet.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "Limelight-internal.h"
#include <orbis/Net.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef HAS_SOCKLEN_T
typedef int ENetSockLen;
#else
typedef socklen_t ENetSockLen;
#endif

#ifndef FIONBIO
#define FIONBIO 0x8004667e
#endif

static enet_uint32 timeBase = 0;

static int oo_is_wouldblock(int err)
{
    return err == 2143223517 || err == EWOULDBLOCK || err == EAGAIN;
}

static int oo_map_ret(int ret)
{
    if (ret < 0) {
        errno = -ret;
        return -1;
    }

    errno = 0;
    return ret;
}

static int oo_addr_family(const ENetAddress* address)
{
    if (!address || address->addressLength == 0) {
        return 0;
    }

    if (address->addressLength >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)&address->address;
        return sin->sin_family;
    }

    return ((const struct sockaddr*)&address->address)->sa_family;
}

static void oo_make_enet_address(const struct sockaddr_in* sin, ENetAddress* address)
{
    if (!sin || !address) {
        return;
    }

    memset(address, 0, sizeof(*address));
    address->addressLength = sizeof(struct sockaddr_in);
    memcpy(&address->address, sin, sizeof(struct sockaddr_in));
}

int enet_initialize(void)
{
    return 0;
}

void enet_deinitialize(void)
{
}

enet_uint32 enet_host_random_seed(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (enet_uint32)(tv.tv_sec ^ tv.tv_usec);
}

enet_uint32 enet_time_get(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (enet_uint32)(tv.tv_sec * 1000 + tv.tv_usec / 1000 - timeBase);
}

void enet_time_set(enet_uint32 newTimeBase)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    timeBase = (enet_uint32)(tv.tv_sec * 1000 + tv.tv_usec / 1000 - newTimeBase);
}

static void oo_prepare_ipv4_sockaddr(const ENetAddress* address,
                                     struct sockaddr_in* sin,
                                     socklen_t* sinLen)
{
    memset(sin, 0, sizeof(*sin));

    if (address && address->addressLength > 0) {
        size_t copyLen = address->addressLength;
        if (copyLen > sizeof(*sin)) {
            copyLen = sizeof(*sin);
        }
        memcpy(sin, &address->address, copyLen);
    }

    if (sin->sin_family == 0) {
        sin->sin_family = AF_INET;
    }

    if (sinLen) {
        *sinLen = sizeof(*sin);
    }
}

int enet_address_set_host_ip(ENetAddress* address, const char* name)
{
    struct sockaddr_in sin;

    if (!address || !name) {
        errno = EINVAL;
        return -1;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;

    if (sceNetInetPton(AF_INET, name, &sin.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    oo_make_enet_address(&sin, address);
    errno = 0;
    return 0;
}

int enet_address_set_host(ENetAddress* address, const char* name)
{
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* cur;

    if (!address || !name) {
        errno = EINVAL;
        return -1;
    }

    if (enet_address_set_host_ip(address, name) == 0) {
        return 0;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(name, NULL, &hints, &result) != 0 || !result) {
        errno = EHOSTUNREACH;
        return -1;
    }

    for (cur = result; cur != NULL; cur = cur->ai_next) {
        if (cur->ai_family == AF_INET && cur->ai_addrlen >= (int)sizeof(struct sockaddr_in)) {
            memset(address, 0, sizeof(*address));
            address->addressLength = sizeof(struct sockaddr_in);
            memcpy(&address->address, cur->ai_addr, sizeof(struct sockaddr_in));
            freeaddrinfo(result);
            errno = 0;
            return 0;
        }
    }

    freeaddrinfo(result);
    errno = EHOSTUNREACH;
    return -1;
}

int enet_address_get_host_ip(const ENetAddress* address, char* name, size_t nameLength)
{
    const struct sockaddr_in* sin;

    if (!address || !name || nameLength == 0) {
        errno = EINVAL;
        return -1;
    }

    if (oo_addr_family(address) != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    sin = (const struct sockaddr_in*)&address->address;

    if (!inet_ntop(AF_INET, &sin->sin_addr, name, (socklen_t)nameLength)) {
        return -1;
    }

    errno = 0;
    return 0;
}

int enet_address_get_host(const ENetAddress* address, char* name, size_t nameLength)
{
    return enet_address_get_host_ip(address, name, nameLength);
}

int enet_address_equal(ENetAddress * address1, ENetAddress * address2)
{
    if (!address1 || !address2) {
        return 0;
    }

    if (address1->addressLength != address2->addressLength) {
        return 0;
    }

    if (address1->address.ss_family != address2->address.ss_family) {
        return 0;
    }

    return memcmp(&address1->address, &address2->address, address1->addressLength) == 0;
}

int enet_address_wildcard(const ENetAddress* address)
{
    if (!address) {
        return 0;
    }

    if (oo_addr_family(address) == AF_INET) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)&address->address;
        return sin->sin_addr.s_addr == INADDR_ANY;
    }

#ifdef AF_INET6
    if (oo_addr_family(address) == AF_INET6) {
        const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)&address->address;
        static const struct in6_addr any6 = IN6ADDR_ANY_INIT;
        return memcmp(&sin6->sin6_addr, &any6, sizeof(any6)) == 0;
    }
#endif

    return 0;
}

int enet_socket_get_address(ENetSocket socket, ENetAddress* address)
{
    OrbisNetSocklen_t len;

    if (!address) {
        errno = EINVAL;
        return -1;
    }

    memset(address, 0, sizeof(*address));
    len = sizeof(address->address);

    if (oo_map_ret(sceNetGetsockname(socket,
                                     (OrbisNetSockaddr*)&address->address,
                                     &len)) < 0) {
        return -1;
    }

    address->addressLength = (socklen_t)len;
    errno = 0;
    return 0;
}

int enet_address_set_address(ENetAddress * address, struct sockaddr * addr, socklen_t addrlen)
{
    if (!address || !addr) {
        errno = EINVAL;
        return -1;
    }

    memset(address, 0, sizeof(*address));

    if (addrlen > (socklen_t)sizeof(address->address)) {
        addrlen = (socklen_t)sizeof(address->address);
    }

    memcpy(&address->address, addr, addrlen);
    address->addressLength = addrlen;

    if (addrlen >= (socklen_t)sizeof(struct sockaddr_in)) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&address->address;
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
        sin->sin_len = sizeof(struct sockaddr_in);
#else
        ((unsigned char*)sin)[0] = sizeof(struct sockaddr_in);
#endif
        if (sin->sin_family == 0 || sin->sin_family == 16) {
            sin->sin_family = AF_INET;
        }
    }

    errno = 0;
    return 0;
}

int enet_address_set_port(ENetAddress * address, enet_uint16 port)
{
    if (!address) {
        errno = EINVAL;
        return -1;
    }

    if (address->addressLength == sizeof(struct sockaddr_in) ||
        oo_addr_family(address) == AF_INET ||
        oo_addr_family(address) == 0) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&address->address;

        if (sin->sin_family == 0) {
            sin->sin_family = AF_INET;
        }

        sin->sin_port = htons(port);

        if (address->addressLength == 0) {
            address->addressLength = sizeof(struct sockaddr_in);
        }

        errno = 0;
        return 0;
    }

#ifdef AF_INET6
    if (oo_addr_family(address) == AF_INET6 ||
        address->addressLength == sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&address->address;

        if (sin6->sin6_family == 0) {
            sin6->sin6_family = AF_INET6;
        }

        sin6->sin6_port = htons(port);

        if (address->addressLength == 0) {
            address->addressLength = sizeof(struct sockaddr_in6);
        }

        errno = 0;
        return 0;
    }
#endif

    errno = EAFNOSUPPORT;
    return -1;
}

ENetSocket enet_socket_create(int family, ENetSocketType type)
{
    int sockType = (type == ENET_SOCKET_TYPE_DATAGRAM) ? SOCK_DGRAM : SOCK_STREAM;
    int proto = (type == ENET_SOCKET_TYPE_DATAGRAM) ? IPPROTO_UDP : IPPROTO_TCP;
    int af = (family == 0) ? AF_INET : family;
    int s;

    s = sceNetSocket("enet", af, sockType, proto);
    if (s < 0) {
        printf("[ENET-OO] socket_create failed af=%d type=%d ret=%d err=%d\n",
               af, type, s, -s);
        errno = -s;
        return ENET_SOCKET_NULL;
    }

    errno = 0;
    return s;
}

int enet_socket_bind(ENetSocket socket, const ENetAddress* address)
{
    struct sockaddr_in sin;
    int ret;

    if (address && oo_addr_family(address) == AF_INET) {
        ret = sceNetBind(socket,
                         (OrbisNetSockaddr*)&address->address,
                         (OrbisNetSocklen_t)address->addressLength);
        if (ret < 0) {
            printf("[ENET-OO] bind failed socket=%d ret=%d err=%d\n",
                   socket, ret, -ret);
        }
        return oo_map_ret(ret);
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = 0;
    sin.sin_addr.s_addr = INADDR_ANY;

    ret = sceNetBind(socket, (OrbisNetSockaddr*)&sin, sizeof(sin));
    if (ret < 0) {
        printf("[ENET-OO] bind fallback failed socket=%d ret=%d err=%d\n",
               socket, ret, -ret);
    }
    return oo_map_ret(ret);
}

int enet_socket_listen(ENetSocket socket, int backlog)
{
    return oo_map_ret(sceNetListen(socket, backlog < 0 ? SOMAXCONN : backlog));
}

ENetSocket enet_socket_accept(ENetSocket socket, ENetAddress* address)
{
    struct sockaddr_in sin;
    OrbisNetSocklen_t sinLen = sizeof(sin);
    int s;

    memset(&sin, 0, sizeof(sin));

    s = sceNetAccept(socket, (OrbisNetSockaddr*)&sin, &sinLen);
    if (s < 0) {
        errno = -s;
        return ENET_SOCKET_NULL;
    }

    if (address) {
        oo_make_enet_address(&sin, address);
    }

    errno = 0;
    return s;
}

int enet_socket_set_option(ENetSocket socket, ENetSocketOption option, int value)
{
    int ret;

    switch (option) {
        case ENET_SOCKOPT_NONBLOCK:
        {
            int nb = value ? 1 : 0;
            struct timeval tv;

            sceNetIoctl(socket, FIONBIO, &nb);

            if (value) {
                tv.tv_sec = 0;
                tv.tv_usec = 1000;
            }
            else {
                tv.tv_sec = 0;
                tv.tv_usec = 0;
            }

            sceNetSetsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            sceNetSetsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            errno = 0;
            return 0;
        }

        case ENET_SOCKOPT_BROADCAST:
            ret = sceNetSetsockopt(socket, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value));
            return oo_map_ret(ret);

        case ENET_SOCKOPT_REUSEADDR:
            ret = sceNetSetsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
            return oo_map_ret(ret);

        case ENET_SOCKOPT_RCVBUF:
            ret = sceNetSetsockopt(socket, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
            return oo_map_ret(ret);

        case ENET_SOCKOPT_SNDBUF:
            ret = sceNetSetsockopt(socket, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value));
            return oo_map_ret(ret);

        case ENET_SOCKOPT_NODELAY:
            ret = sceNetSetsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
            return oo_map_ret(ret);

        case ENET_SOCKOPT_QOS:
            errno = 0;
            return 0;

        default:
            errno = 0;
            return 0;
    }
}

int enet_socket_get_option(ENetSocket socket, ENetSocketOption option, int* value)
{
    OrbisNetSocklen_t len = sizeof(int);
    int optname;
    int level;

    if (!value) {
        errno = EINVAL;
        return -1;
    }

    switch (option) {
        case ENET_SOCKOPT_RCVBUF:
            level = SOL_SOCKET;
            optname = SO_RCVBUF;
            break;
        case ENET_SOCKOPT_SNDBUF:
            level = SOL_SOCKET;
            optname = SO_SNDBUF;
            break;
        case ENET_SOCKOPT_NODELAY:
            level = IPPROTO_TCP;
            optname = TCP_NODELAY;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    return oo_map_ret(sceNetGetsockopt(socket, level, optname, value, &len));
}

int enet_socket_connect(ENetSocket socket, const ENetAddress* address)
{
    int ret;
    int af;
    unsigned port;
    struct sockaddr_in sin;
    socklen_t sinLen;

    if (!address) {
        errno = EINVAL;
        return -1;
    }

    oo_prepare_ipv4_sockaddr(address, &sin, &sinLen);
    af = sin.sin_family;
    port = (unsigned)ntohs(sin.sin_port);

    ret = sceNetConnect(socket,
                        (OrbisNetSockaddr*)&sin,
                        (OrbisNetSocklen_t)sinLen);

    if (ret < 0) {
        int err = -ret;
        errno = err;

        if (err == EINPROGRESS || err == EALREADY || err == EWOULDBLOCK || err == 2143223517) {
            return 0;
        }

        printf("[ENET-OO] connect failed socket=%d af=%d port=%u err=%d\n",
               socket, af, port, err);
        return -1;
    }

    errno = 0;
    return 0;
}

int enet_socket_shutdown(ENetSocket socket, ENetSocketShutdown how)
{
    int mode;

    switch (how) {
        case ENET_SOCKET_SHUTDOWN_READ:
            mode = SHUT_RD;
            break;
        case ENET_SOCKET_SHUTDOWN_WRITE:
            mode = SHUT_WR;
            break;
        case ENET_SOCKET_SHUTDOWN_READ_WRITE:
        default:
            mode = SHUT_RDWR;
            break;
    }

    return oo_map_ret(sceNetShutdown(socket, mode));
}

void enet_socket_destroy(ENetSocket socket)
{
    if (socket != ENET_SOCKET_NULL) {
        oo_map_ret(sceNetSocketClose(socket));
    }
}

int enet_socket_send(ENetSocket socket,
                     const ENetAddress* address,
                     const ENetAddress* broadcastAddress,
                     const ENetBuffer* buffers,
                     size_t bufferCount)
{
    int sentLength = 0;
    size_t i;

    (void)broadcastAddress;

    if (address != NULL) {
        struct sockaddr_in sin;
        socklen_t sinLen;
        size_t totalLen = 0;
        char* packet;
        int ret;

        oo_prepare_ipv4_sockaddr(address, &sin, &sinLen);

        for (i = 0; i < bufferCount; ++i) {
            totalLen += buffers[i].dataLength;
        }

        packet = (char*)malloc(totalLen);
        if (!packet) {
            errno = ENOMEM;
            return -1;
        }

        {
            size_t offset = 0;
            for (i = 0; i < bufferCount; ++i) {
                memcpy(packet + offset, buffers[i].data, buffers[i].dataLength);
                offset += buffers[i].dataLength;
            }
        }

        ret = sceNetSendto(socket,
                           packet,
                           (int)totalLen,
                           0,
                           (const OrbisNetSockaddr*)&sin,
                           (OrbisNetSocklen_t)sinLen);

        free(packet);

        if (ret < 0) {
            int err = -ret;

            if (oo_is_wouldblock(err)) {
                errno = EWOULDBLOCK;
                return 0;
            }

            errno = err;
            printf("[ENET-OO] sendto failed socket=%d len=%u err=%d\n",
                   socket, (unsigned)totalLen, err);
            return -1;
        }

        errno = 0;
        return ret;
    }
    else {
        for (i = 0; i < bufferCount; ++i) {
            int ret = sceNetSend(socket,
                                 buffers[i].data,
                                 (int)buffers[i].dataLength,
                                 0);

            if (ret < 0) {
                int err = -ret;

                if (oo_is_wouldblock(err)) {
                    errno = EWOULDBLOCK;
                    return 0;
                }

                errno = err;
                printf("[ENET-OO] send failed socket=%d len=%u err=%d\n",
                       socket, (unsigned)buffers[i].dataLength, err);
                return -1;
            }

            sentLength += ret;
        }

        errno = 0;
        return sentLength;
    }
}

int enet_socket_receive(ENetSocket socket,
                        ENetAddress* address,
                        ENetAddress* broadcastAddress,
                        ENetBuffer* buffers,
                        size_t bufferCount)
{
    int recvLength = 0;
    size_t i;

    (void)broadcastAddress;

    if (!buffers || bufferCount == 0) {
        errno = EINVAL;
        return -1;
    }

    if (address != NULL) {
        struct sockaddr_storage from;
        OrbisNetSocklen_t fromLen;

        for (i = 0; i < bufferCount; ++i) {
            int ret;

            memset(&from, 0, sizeof(from));
            fromLen = sizeof(from);

            ret = sceNetRecvfrom(socket,
                                 buffers[i].data,
                                 (int)buffers[i].dataLength,
                                 0,
                                 (OrbisNetSockaddr*)&from,
                                 &fromLen);

            if (ret < 0) {
                int err = -ret;

                if (oo_is_wouldblock(err)) {
                    errno = EWOULDBLOCK;
                    return 0;
                }

                errno = err;
                printf("[ENET-OO] recvfrom failed socket=%d err=%d\n", socket, err);
                return -1;
            }

            if (ret == 0) {
                break;
            }

            recvLength += ret;

            if ((size_t)ret < buffers[i].dataLength) {
                break;
            }
        }

        memset(address, 0, sizeof(*address));
        if (fromLen > sizeof(struct sockaddr_in)) {
            fromLen = sizeof(struct sockaddr_in);
        }
        address->addressLength = (socklen_t)fromLen;
        memcpy(&address->address, &from, fromLen);
        if (address->addressLength >= (socklen_t)sizeof(struct sockaddr_in)) {
            ((struct sockaddr_in*)&address->address)->sin_family = AF_INET;
            address->addressLength = sizeof(struct sockaddr_in);
        }
    }
    else {
        for (i = 0; i < bufferCount; ++i) {
            int ret = sceNetRecv(socket,
                                 buffers[i].data,
                                 (int)buffers[i].dataLength,
                                 0);

            if (ret < 0) {
                int err = -ret;

                if (oo_is_wouldblock(err)) {
                    errno = EWOULDBLOCK;
                    return 0;
                }

                errno = err;
                printf("[ENET-OO] recv failed socket=%d err=%d\n", socket, err);
                return -1;
            }

            if (ret == 0) {
                break;
            }

            recvLength += ret;

            if ((size_t)ret < buffers[i].dataLength) {
                break;
            }
        }
    }

    errno = 0;
    return recvLength;
}

int enet_socketset_select(ENetSocket maxSocket,
                          ENetSocketSet* readSet,
                          ENetSocketSet* writeSet,
                          enet_uint32 timeout)
{
    struct timeval tv;
    struct timeval* tvp = NULL;
    int ret;

    if (timeout != (enet_uint32)-1) {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        tvp = &tv;
    }

    ret = select(maxSocket + 1, readSet, writeSet, NULL, tvp);
    if (ret >= 0) {
        errno = 0;
    }
    return ret;
}

int enet_socket_wait(ENetSocket socket, enet_uint32* condition, enet_uint32 timeout)
{
    fd_set readSet, writeSet;
    struct timeval timeVal;
    int selectCount;

    if (condition == NULL) {
        errno = EINVAL;
        return -1;
    }

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);

    if (*condition & ENET_SOCKET_WAIT_SEND)
        FD_SET(socket, &writeSet);

    if (*condition & ENET_SOCKET_WAIT_RECEIVE)
        FD_SET(socket, &readSet);

    selectCount = select(socket + 1, &readSet, &writeSet, NULL, &timeVal);

    if (selectCount < 0) {
        if (errno == EINTR && (*condition & ENET_SOCKET_WAIT_INTERRUPT)) {
            *condition = ENET_SOCKET_WAIT_INTERRUPT;
            return 0;
        }

        printf("[ENET-OO] wait select failed socket=%d err=%d\n", socket, errno);
        return -1;
    }

    *condition = ENET_SOCKET_WAIT_NONE;

    if (selectCount == 0) {
        errno = 0;
        return 0;
    }

    if (FD_ISSET(socket, &writeSet))
        *condition |= ENET_SOCKET_WAIT_SEND;

    if (FD_ISSET(socket, &readSet))
        *condition |= ENET_SOCKET_WAIT_RECEIVE;

    errno = 0;
    return 0;
}
