// === PlatformSockets.c (ported) =============================================
//
// OpenOrbis Moonlight client — PS4 socket layer for moonlight-common-c
//
// This is a project-owned replacement for
// third_party/moonlight-common-c/src/PlatformSockets.c. It implements the same
// PlatformSockets.h API but uses the native sceNet* API directly, because the
// OpenOrbis POSIX socket wrappers are unreliable for UDP unicast reception on
// an ephemeral socket (the same issue documented in DiscoveryManager.cpp).
//
// The proven OpenOrbis reference (moonlight-ps4_2) uses the native sceNet* API
// directly; we follow that approach here. Nothing under third_party/ is
// modified — this file is swapped in via target_sources(... REMOVE ...).
//
// Error mapping: sceNet* functions return negative SCE error codes. We negate
// them to a positive errno-like value and store it in errno, so the upstream
// PlatformSockets.h's LastSocketError() (which reads errno) works unchanged.
// ===============================================================================

#include "Limelight-internal.h"
#include "PlatformSockets.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <orbis/Net.h>

#define TEST_PORT_TIMEOUT_SEC 3
#define RCV_BUFFER_SIZE_MIN  32767
#define RCV_BUFFER_SIZE_STEP 16384
#define TCPv4_MSS 536
#define TCPv6_MSS 1220
#define UDP_RECV_POLL_TIMEOUT_MS 100

// SCE_NET_ERROR_EAGAIN / SCE_NET_ERROR_EWOULDBLOCK (no data now).
#define SCE_NET_EAGAIN 2143223517

// ---------------------------------------------------------------------------
// sceNet* wrappers with errno mapping
// ---------------------------------------------------------------------------

static int map_sce_result(int ret) {
    if (ret < 0) {
        errno = -ret;
        return SOCKET_ERROR;
    }
    errno = 0;
    return ret;
}

static SOCKET oo_socket(int af, int type, int proto) {
    int ret = sceNetSocket("moonlight-common", af, type, proto);
    if (ret < 0) {
        errno = -ret;
        return INVALID_SOCKET;
    }
    errno = 0;
    return ret;
}

static int oo_connect(SOCKET s, const struct sockaddr* addr, SOCKADDR_LEN len) {
    return map_sce_result(sceNetConnect(s, (const OrbisNetSockaddr*)addr, (OrbisNetSocklen_t)len));
}

static int oo_bind(SOCKET s, const struct sockaddr* addr, SOCKADDR_LEN len) {
    return map_sce_result(sceNetBind(s, (const OrbisNetSockaddr*)addr, (OrbisNetSocklen_t)len));
}

static int oo_getsockname(SOCKET s, struct sockaddr* addr, SOCKADDR_LEN* len) {
    return map_sce_result(sceNetGetsockname(s, (OrbisNetSockaddr*)addr, (OrbisNetSocklen_t*)len));
}

static SOCK_RET oo_send(SOCKET s, const void* buf, size_t len, int flags) {
    int ret = sceNetSend(s, buf, len, flags);
    if (ret < 0) {
        errno = -ret;
        return SOCKET_ERROR;
    }
    errno = 0;
    return ret;
}

static SOCK_RET oo_sendto(SOCKET s, const void* buf, size_t len, int flags,
                          const struct sockaddr* to, SOCKADDR_LEN tolen) {
    int ret = sceNetSendto(s, buf, len, flags,
                           (const OrbisNetSockaddr*)to, (OrbisNetSocklen_t)tolen);
    if (ret < 0) {
        errno = -ret;
        return SOCKET_ERROR;
    }
    errno = 0;
    return ret;
}

static SOCK_RET oo_recv(SOCKET s, void* buf, size_t len, int flags) {
    int ret = sceNetRecv(s, buf, len, flags);
    if (ret < 0) {
        errno = -ret;
        return SOCKET_ERROR;
    }
    errno = 0;
    return ret;
}

static SOCK_RET oo_recvfrom(SOCKET s, void* buf, size_t len, int flags,
                            struct sockaddr* from, SOCKADDR_LEN* fromlen) {
    int ret = sceNetRecvfrom(s, buf, len, flags,
                             (OrbisNetSockaddr*)from, (OrbisNetSocklen_t*)fromlen);
    if (ret < 0) {
        errno = -ret;
        return SOCKET_ERROR;
    }
    errno = 0;
    return ret;
}

static int oo_setsockopt(SOCKET s, int level, int optname, const void* optval, SOCKADDR_LEN optlen) {
    return map_sce_result(sceNetSetsockopt(s, level, optname, optval, (OrbisNetSocklen_t)optlen));
}

static int oo_getsockopt(SOCKET s, int level, int optname, void* optval, SOCKADDR_LEN* optlen) {
    return map_sce_result(sceNetGetsockopt(s, level, optname, optval, (OrbisNetSocklen_t*)optlen));
}

static int oo_shutdown(SOCKET s, int how) {
    return map_sce_result(sceNetShutdown(s, how));
}

static int oo_close(SOCKET s) {
    return map_sce_result(sceNetSocketClose(s));
}

// ---------------------------------------------------------------------------
// PlatformSockets.h API implementation
// ---------------------------------------------------------------------------

void addrToUrlSafeString(struct sockaddr_storage* addr, char* string, size_t stringLength)
{
    char addrstr[URLSAFESTRING_LEN];

#ifdef AF_INET6
    if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)addr;
        inet_ntop(addr->ss_family, &sin6->sin6_addr, addrstr, sizeof(addrstr));
        snprintf(string, stringLength, "[%s]", addrstr);
    }
    else
#endif
    {
        struct sockaddr_in* sin = (struct sockaddr_in*)addr;
        inet_ntop(addr->ss_family, &sin->sin_addr, addrstr, sizeof(addrstr));
        snprintf(string, stringLength, "%s", addrstr);
    }
}

void shutdownTcpSocket(SOCKET s) {
    oo_shutdown(s, SHUT_RDWR);
}

int setNonFatalRecvTimeoutMs(SOCKET s, int timeoutMs) {
    struct timeval val;

    val.tv_sec = timeoutMs / 1000;
    val.tv_usec = (timeoutMs % 1000) * 1000;

    return oo_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &val, sizeof(val));
}

int pollSockets(struct pollfd* pollFds, int pollFdsCount, int timeoutMs) {
    fd_set readFds, writeFds, exceptFds;
    struct timeval tv;
    struct timeval* ptv;
    int i;
    int maxfd = -1;
    int err;

    FD_ZERO(&readFds);
    FD_ZERO(&writeFds);
    FD_ZERO(&exceptFds);

    for (i = 0; i < pollFdsCount; i++) {
        pollFds[i].revents = 0;

        if (pollFds[i].fd < 0) {
            pollFds[i].revents |= POLLNVAL;
            continue;
        }

        if (pollFds[i].events & POLLIN) {
            FD_SET(pollFds[i].fd, &readFds);
        }
        if (pollFds[i].events & POLLOUT) {
            FD_SET(pollFds[i].fd, &writeFds);
        }
        FD_SET(pollFds[i].fd, &exceptFds);

        if (pollFds[i].fd > maxfd) {
            maxfd = pollFds[i].fd;
        }
    }

    if (timeoutMs >= 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ptv = &tv;
    }
    else {
        ptv = NULL;
    }

    err = select(maxfd + 1, &readFds, &writeFds, &exceptFds, ptv);
    if (err < 0) {
        errno = errno;
        return SOCKET_ERROR;
    }
    if (err == 0) {
        return 0;
    }

    for (i = 0; i < pollFdsCount; i++) {
        if (pollFds[i].fd < 0) {
            continue;
        }

        if (FD_ISSET(pollFds[i].fd, &readFds)) {
            pollFds[i].revents |= POLLRDNORM;
        }
        if (FD_ISSET(pollFds[i].fd, &writeFds)) {
            pollFds[i].revents |= POLLWRNORM;
        }
        if (FD_ISSET(pollFds[i].fd, &exceptFds)) {
            pollFds[i].revents |= POLLERR;
        }
    }

    return err;
}

bool isSocketReadable(SOCKET s) {
    struct pollfd pfd;
    int err;

    pfd.fd = s;
    pfd.events = POLLIN;
    pfd.revents = 0;

    err = pollSockets(&pfd, 1, 0);
    if (err <= 0) {
        return false;
    }

    return (pfd.revents & POLLRDNORM) != 0;
}

int recvUdpSocket(SOCKET s, char* buffer, int size, bool useSelect) {
    int err;

    do {
        if (useSelect) {
            struct pollfd pfd;

            pfd.fd = s;
            pfd.events = POLLIN;
            pfd.revents = 0;

            err = pollSockets(&pfd, 1, UDP_RECV_POLL_TIMEOUT_MS);
            if (err <= 0) {
                return err;
            }

            err = (int)oo_recvfrom(s, buffer, size, 0, NULL, NULL);
        }
        else {
            err = (int)oo_recvfrom(s, buffer, size, 0, NULL, NULL);
            if (err < 0 &&
                    (LastSocketError() == EWOULDBLOCK ||
                     LastSocketError() == EINTR ||
                     LastSocketError() == EAGAIN ||
                     LastSocketError() == ETIMEDOUT ||
                     LastSocketError() == SCE_NET_EAGAIN)) {
                return 0;
            }
        }

    } while (err < 0 &&
             (LastSocketError() == ECONNREFUSED ||
              LastSocketError() == EINTR));

    return err;
}

int setSocketNonBlocking(SOCKET s, bool enabled) {
    (void)s;
    (void)enabled;
    // On OpenOrbis, sockets are created blocking; the sceNet* API does not
    // expose a reliable non-blocking flag via fcntl. The select()-based
    // pollSockets() provides the needed non-blocking behavior.
    return 0;
}

SOCKET createSocket(int addressFamily, int socketType, int protocol, bool nonBlocking) {
    SOCKET s;

    if (addressFamily == 0) {
        addressFamily = AF_INET;
    }

    s = oo_socket(addressFamily, socketType, protocol);
    if (s == INVALID_SOCKET) {
        Limelog("socket() failed: %d\n", LastSocketError());
        return INVALID_SOCKET;
    }

    if (nonBlocking) {
        setSocketNonBlocking(s, true);
    }

    return s;
}

SOCKET connectTcpSocket(struct sockaddr_storage* dstaddr, SOCKADDR_LEN addrlen, unsigned short port, int timeoutSec) {
    SOCKET s;
    LC_SOCKADDR addr;
    int val;

    (void)timeoutSec;

    s = createSocket(dstaddr->ss_family, SOCK_STREAM, IPPROTO_TCP, false);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

#ifdef TCP_MAXSEG
    val = dstaddr->ss_family == AF_INET ? TCPv4_MSS : TCPv6_MSS;
    oo_setsockopt(s, IPPROTO_TCP, TCP_MAXSEG, &val, sizeof(val));
#endif

    memcpy(&addr, dstaddr, addrlen);
    SET_PORT(&addr, port);

    if (oo_connect(s, (struct sockaddr*)&addr, addrlen) < 0) {
        int err = LastSocketError();
        Limelog("connect() failed: %d\n", err);
        closeSocket(s);
        SetLastSocketError(err);
        return INVALID_SOCKET;
    }

    return s;
}

int sendMtuSafe(SOCKET s, char* buffer, int size) {
    int offset = 0;

    while (offset < size) {
        int toSend = size - offset;
        if (toSend > 1024) {
            toSend = 1024;
        }

        if (oo_send(s, buffer + offset, toSend, 0) < 0) {
            return LastSocketFail();
        }

        offset += toSend;
    }

    return 0;
}

static void setSocketQos(SOCKET s, int socketQosType) {
    (void)s;
    (void)socketQosType;
    /* No-op for now on OpenOrbis */
}

SOCKET bindUdpSocket(int addressFamily, struct sockaddr_storage* localAddr, SOCKADDR_LEN addrLen, int bufferSize, int socketQosType) {
    SOCKET s;
    LC_SOCKADDR bindAddr;
    int err = 0;

    s = createSocket(addressFamily, SOCK_DGRAM, IPPROTO_UDP, false);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    if (localAddr && localAddr->ss_family != 0) {
        memcpy(&bindAddr, localAddr, addrLen);
        SET_PORT(&bindAddr, 0);
    }
    else {
        memset(&bindAddr, 0, sizeof(bindAddr));
        SET_FAMILY(&bindAddr, addressFamily);
#ifdef AF_INET6
        addrLen = (addressFamily == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
#else
        addrLen = sizeof(struct sockaddr_in);
#endif
    }

    if (oo_bind(s, (struct sockaddr*)&bindAddr, addrLen) == SOCKET_ERROR) {
        err = LastSocketError();
        Limelog("bind() failed: %d\n", err);
        closeSocket(s);
        SetLastSocketError(err);
        return INVALID_SOCKET;
    }

    if (socketQosType != SOCK_QOS_TYPE_BEST_EFFORT) {
        setSocketQos(s, socketQosType);
    }

    if (bufferSize != 0) {
        for (;;) {
            err = oo_setsockopt(s, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize));
            if (err == 0) {
                break;
            }
            else if (bufferSize <= RCV_BUFFER_SIZE_MIN) {
                Limelog("Set rcv buffer size failed: %d\n", LastSocketError());
                break;
            }
            else if (bufferSize - RCV_BUFFER_SIZE_STEP <= RCV_BUFFER_SIZE_MIN) {
                bufferSize = RCV_BUFFER_SIZE_MIN;
            }
            else {
                bufferSize -= RCV_BUFFER_SIZE_STEP;
            }
        }
    }

    return s;
}

int enableNoDelay(SOCKET s) {
    int val = 1;
    return oo_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
}

void closeSocket(SOCKET s) {
    if (s != INVALID_SOCKET) {
        oo_close(s);
    }
}

bool isPrivateNetworkAddress(struct sockaddr_storage* address) {
    if (address->ss_family == AF_INET) {
        uint32_t addr = ntohl(((struct sockaddr_in*)address)->sin_addr.s_addr);

        if ((addr & 0xFF000000U) == 0x0A000000U) return true;       /* 10.0.0.0/8 */
        if ((addr & 0xFFF00000U) == 0xAC100000U) return true;       /* 172.16.0.0/12 */
        if ((addr & 0xFFFF0000U) == 0xC0A80000U) return true;       /* 192.168.0.0/16 */
        if ((addr & 0xFFFF0000U) == 0xA9FE0000U) return true;       /* 169.254.0.0/16 */
    }
#ifdef AF_INET6
    else if (address->ss_family == AF_INET6) {
        const uint8_t* a = ((struct sockaddr_in6*)address)->sin6_addr.s6_addr;
        if ((a[0] & 0xFE) == 0xFC) return true;                     /* fc00::/7 */
        if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return true;    /* fe80::/10 */
    }
#endif
    return false;
}

bool isNat64SynthesizedAddress(struct sockaddr_storage* address) {
#ifdef AF_INET6
    if (address->ss_family == AF_INET6) {
        const uint8_t* a = ((struct sockaddr_in6*)address)->sin6_addr.s6_addr;
        static const uint8_t prefix[12] = {0x00,0x64,0xff,0x9b,0,0,0,0,0,0,0,0};
        return memcmp(a, prefix, 12) == 0;
    }
#endif
    return false;
}

int getLocalAddressByUdpConnect(const struct sockaddr_storage* targetAddr, SOCKADDR_LEN targetAddrLen, unsigned short targetPort,
                                struct sockaddr_storage* localAddr, SOCKADDR_LEN* localAddrLen)
{
    SOCKET udpSocket;
    LC_SOCKADDR remoteAddr;
    SOCKADDR_LEN addrLen;
    int err;
    int family;

    if (!targetAddr || !localAddr || !localAddrLen) {
        return -1;
    }

    family = targetAddr->ss_family;
    if (family == 0) {
        family = AF_INET;
    }

    udpSocket = createSocket(family, SOCK_DGRAM, IPPROTO_UDP, false);
    if (udpSocket == INVALID_SOCKET) {
        return LastSocketError();
    }

    memset(&remoteAddr, 0, sizeof(remoteAddr));
    memcpy(&remoteAddr, targetAddr, targetAddrLen);
    SET_FAMILY(&remoteAddr, family);
    SET_PORT(&remoteAddr, targetPort);

    err = oo_connect(udpSocket, (struct sockaddr*)&remoteAddr, targetAddrLen);
    if (err < 0) {
        err = LastSocketError();
        closeSocket(udpSocket);
        return err;
    }

    addrLen = sizeof(*localAddr);
    err = oo_getsockname(udpSocket, (struct sockaddr*)localAddr, &addrLen);
    if (err < 0) {
        err = LastSocketError();
        closeSocket(udpSocket);
        return err;
    }

    *localAddrLen = addrLen;
    closeSocket(udpSocket);
    return 0;
}

static int resolveNumericHost(const char* host, int family, struct sockaddr_storage* addr, SOCKADDR_LEN* addrLen) {
    memset(addr, 0, sizeof(*addr));

#ifdef AF_INET6
    if (family == AF_INET6 || family == AF_UNSPEC) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)addr;
        if (sceNetInetPton(AF_INET6, host, &sin6->sin6_addr) == 1) {
            sin6->sin6_family = AF_INET6;
            *addrLen = sizeof(*sin6);
            return 0;
        }
    }
#endif

    if (family == AF_INET || family == AF_UNSPEC) {
        struct sockaddr_in* sin = (struct sockaddr_in*)addr;
        if (sceNetInetPton(AF_INET, host, &sin->sin_addr) == 1) {
            sin->sin_family = AF_INET;
            *addrLen = sizeof(*sin);
            return 0;
        }
    }

    return -1;
}

int resolveHostName(const char* host, int family, int tcpTestPort, struct sockaddr_storage* addr, SOCKADDR_LEN* addrLen)
{
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* current;
    int gaiErr;

    if (!host || !addr || !addrLen) {
        return -1;
    }

    if (resolveNumericHost(host, family, addr, addrLen) == 0) {
        return 0;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;

    gaiErr = getaddrinfo(host, NULL, &hints, &result);
    if (gaiErr != 0 || !result) {
        SetLastSocketError(gaiErr != 0 ? gaiErr : -1);
        return LastSocketFail();
    }

    for (current = result; current != NULL; current = current->ai_next) {
        SOCKET testSocket;

        if ((size_t)current->ai_addrlen > sizeof(*addr)) {
            continue;
        }

        memcpy(addr, current->ai_addr, current->ai_addrlen);
        *addrLen = (SOCKADDR_LEN)current->ai_addrlen;

        if (tcpTestPort == 0) {
            freeaddrinfo(result);
            return 0;
        }

        testSocket = connectTcpSocket((struct sockaddr_storage*)current->ai_addr,
                                      (SOCKADDR_LEN)current->ai_addrlen,
                                      (unsigned short)(tcpTestPort & TCP_PORT_MASK),
                                      TEST_PORT_TIMEOUT_SEC);
        if (testSocket != INVALID_SOCKET) {
            closeSocket(testSocket);
            freeaddrinfo(result);
            return 0;
        }
    }

    freeaddrinfo(result);
    return LastSocketFail();
}

void enterLowLatencyMode(void) {
    /* No-op for now on OpenOrbis */
}

void exitLowLatencyMode(void) {
    /* No-op for now on OpenOrbis */
}

int initializePlatformSockets(void) {
    // The PS4 network stack is initialized by the application's NetworkManager
    // (sceNetInit / sceNetCtlInit). No additional initialization is required.
    return 0;
}

void cleanupPlatformSockets(void) {
}
