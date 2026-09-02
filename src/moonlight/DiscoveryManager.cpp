// Copyright (C) 2026 fourletterworkshop.com
// This file is part of Moonlight DX for PlayStation 4.
// Moonlight DX for PlayStation 4 is free software: you can redistribute it
// and/or modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation, either version 3 of the License,
// or (at your option) any later version.
// Moonlight DX for PlayStation 4 is distributed in the hope that it will be
// useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
// You should have received a copy of the GNU General Public License along with
// Moonlight DX for PlayStation 4. If not, see <https://www.gnu.org/licenses/>.
//

// === DiscoveryManager.cpp ====================================================
//
// OpenOrbis Moonlight client — Host Discovery subsystem
//
// Implements the DiscoveryManager concrete Moonlight-specific host discovery
// subsystem:
//   - One UDP socket bound to an ephemeral local port (never 5353).
//   - One worker thread sending periodic mDNS browse queries for
//     _nvstream._tcp.local to 224.0.0.251:5353.
//   - Bounded receive wait via non-blocking recvfrom + short sleep.
//   - Minimum DNS/mDNS parser (header, question skip, RR iteration,
//     compressed names, PTR/SRV/A/TXT).
//   - Packet-local record resolution (PTR -> SRV -> A).
//   - Duplicate suppression cache.
//   - Bounded result queue.
//   - Deterministic shutdown.
//
// OpenOrbis adaptation: uses the native sceNet* network API directly
// (sceNetSocket/sceNetSendto/sceNetRecvfrom/sceNetSocketClose), matching the
// proven OpenOrbis reference (moonlight-ps4_2). The OpenOrbis POSIX socket
// wrappers are unreliable for UDP unicast reception on an ephemeral socket,
// which causes mDNS observations to stop arriving after the initial discovery.
// Threading uses POSIX pthread functions (pthread_create/join/mutex).
//
// Discovery reliability: this module relies purely on unicast mDNS responses
// (every query sets the unicast-response-preferred bit). It deliberately does
// NOT bind explicitly and does NOT join the mDNS multicast group, because the
// unicast response path is the reliable one on OpenOrbis hardware.
//
// OpenOrbis SDK includes are confined to this translation unit.
// ===============================================================================

#include "DiscoveryManager.h"

// --- Native PS4 network API (proven reliable OpenOrbis path) -----------------
// The OpenOrbis POSIX socket wrappers (socket/bind/sendto/recvfrom) are
// unreliable for UDP unicast reception on an ephemeral socket, which causes
// mDNS observations to stop arriving after the initial discovery. The proven
// OpenOrbis reference (moonlight-ps4_2) uses the native sceNet* API directly
// and relies purely on unicast responses (no multicast membership, no explicit
// bind). We use that same reliable path here.
#include <orbis/Net.h>         // sceNetSocket, sceNetSendto, sceNetRecvfrom, etc.
#include <orbis/libkernel.h>   // sceKernelGetProcessTime, sceKernelUsleep

// --- POSIX threading headers (OpenOrbis libc) --------------------------------
#include <pthread.h>           // pthread_create, pthread_join, pthread_mutex_*

#include <cstdio>              // printf, fflush, snprintf
#include <cstring>             // memset, memcpy
#include <string>
#include <deque>
#include <vector>
#include <atomic>

// ---------------------------------------------------------------------------
// Native network constants / types not provided by the SDK header
// ---------------------------------------------------------------------------

#ifndef ORBIS_NET_SOL_SOCKET
#define ORBIS_NET_SOL_SOCKET       0xFFFF
#endif

#ifndef ORBIS_NET_IPPROTO_IP
#define ORBIS_NET_IPPROTO_IP       0
#endif

#ifndef ORBIS_NET_MSG_DONTWAIT
#define ORBIS_NET_MSG_DONTWAIT     0x80
#endif

#ifndef ORBIS_NET_INADDR_ANY
#define ORBIS_NET_INADDR_ANY       0x00000000
#endif

#ifndef ORBIS_NET_INET_ADDRSTRLEN
#define ORBIS_NET_INET_ADDRSTRLEN  16
#endif

// sceNetRecvfrom returns full SCE error codes (0x8041xxxx), not raw errno.
// SCE_NET_ERROR_EAGAIN / SCE_NET_ERROR_EWOULDBLOCK = 0x80410123 (no data now).
// SCE_NET_ERROR_ETIMEDOUT = 0x8041013c.
#ifndef ORBIS_NET_ERROR_EAGAIN
#define ORBIS_NET_ERROR_EAGAIN     0x80410123
#endif

#ifndef ORBIS_NET_ERROR_ETIMEDOUT
#define ORBIS_NET_ERROR_ETIMEDOUT  0x8041013c
#endif

/// IPv4 socket address (native layout, matching the proven OpenOrbis path).
struct OrbisNetSockaddrIn {
    uint8_t  sin_len;
    uint8_t  sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
};

// ---------------------------------------------------------------------------
// Diagnostics helper (minimal; consistent with other platform modules)
// ---------------------------------------------------------------------------

static void discDiagPrint(const char* msg) {
    printf("%s\n", msg);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

namespace {

/// mDNS multicast destination address.
constexpr const char* kMdnsMulticastAddr = "224.0.0.251";
/// mDNS multicast destination port.
constexpr uint16_t kMdnsPort = 5353;

/// Receive burst window in milliseconds (how long to collect responses after a query).
constexpr uint64_t kReceiveBurstWindowMs = 750;

/// Receive poll sleep in microseconds (between non-blocking receive attempts).
constexpr uint64_t kReceivePollSleepUs = 25 * 1000;

/// Worker idle sleep in microseconds (between query cycles).
constexpr uint64_t kWorkerIdleSleepUs = 50 * 1000;

/// Query interval in milliseconds.
constexpr uint64_t kQueryIntervalMs = 3000;

/// Refresh interval for re-emitting a known observation (ms).
/// Matches the query interval so a host that comes back Online is re-emitted
/// promptly (within one query cycle) rather than being suppressed for up to
/// 10 s. This keeps newly-online hosts appearing without a long delay.
constexpr uint64_t kRefreshIntervalMs = 3000;

/// Result queue capacity.
constexpr size_t kQueueCapacity = 16;

/// Duplicate cache capacity.
constexpr size_t kCacheCapacity = 32;

/// UDP receive buffer size.
constexpr size_t kReceiveBufferSize = 4096;

/// Maximum DNS name length (RFC 1035).
constexpr size_t kMaxDnsNameLen = 255;

/// Maximum compression pointer jumps.
constexpr int kMaxPointerJumps = 16;

/// Maximum number of packet-local records to collect.
constexpr size_t kMaxPacketRecords = 128;

/// DNS record types.
constexpr uint16_t kDnsTypePTR = 12;
constexpr uint16_t kDnsTypeTXT = 16;
constexpr uint16_t kDnsTypeSRV = 33;
constexpr uint16_t kDnsTypeA   = 1;

/// DNS class IN.
constexpr uint16_t kDnsClassIN = 1;
/// Unicast-response-preferred bit (mDNS).
constexpr uint16_t kDnsClassUnicastResponse = 0x8000;

/// The service we browse for.
constexpr const char* kServiceName = "_nvstream._tcp.local";

} // namespace

// ---------------------------------------------------------------------------
// Private implementation state
// ---------------------------------------------------------------------------

namespace ml {

/// Private implementation of DiscoveryManager. Holds all socket state,
/// worker-thread state, and synchronization.
struct DiscoveryManager::Impl {
    // --- Lifecycle ----------------------------------------------------------
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};

    // --- Socket -------------------------------------------------------------
    OrbisNetId socketId = -1;   ///< Native sceNet socket id (negative = not open).

    // --- Worker thread ------------------------------------------------------
    pthread_t workerThread;     ///< Worker thread handle.
    bool threadCreated = false; ///< Whether workerThread is valid.

    // --- Result queue -------------------------------------------------------
    pthread_mutex_t queueMutex;
    bool queueMutexInit = false;
    std::deque<HostDiscoveryUpdate> queue;

    // --- Duplicate cache (worker-local; no lock needed) ---------------------
    struct ObservationKey {
        std::string instanceFqdn;
        std::string ipv4;
        uint16_t    port;
    };
    std::deque<std::pair<ObservationKey, uint64_t>> cache; // key -> lastEmitMs

    // --- Rate-limited error logging -----------------------------------------
    uint64_t lastSendErrorLogMs = 0;
    uint64_t lastRecvErrorLogMs = 0;

    // --- Diagnostics --------------------------------------------------------
    uint16_t ephemeralPort = 0;
};

// ---------------------------------------------------------------------------
// DNS parser helpers (private to this translation unit)
// ---------------------------------------------------------------------------

namespace {

/// Read a big-endian uint16 from a packet at `offset`.
/// Returns false if `offset + 2` exceeds `packetLen`.
bool readU16(const uint8_t* p, size_t len, size_t offset, uint16_t& out) {
    if (offset + 2 > len) {
        return false;
    }
    out = static_cast<uint16_t>((static_cast<uint16_t>(p[offset]) << 8) |
                                static_cast<uint16_t>(p[offset + 1]));
    return true;
}

/// Read a big-endian uint32 from a packet at `offset`.
bool readU32(const uint8_t* p, size_t len, size_t offset, uint32_t& out) {
    if (offset + 4 > len) {
        return false;
    }
    out = (static_cast<uint32_t>(p[offset]) << 24) |
          (static_cast<uint32_t>(p[offset + 1]) << 16) |
          (static_cast<uint32_t>(p[offset + 2]) << 8) |
          static_cast<uint32_t>(p[offset + 3]);
    return true;
}

/// Decode a DNS name starting at `offset` in `packet`.
///
/// Handles DNS name compression (pointers). Iterative, no recursion.
/// Protections:
///   - Every byte read is bounds-checked against `packetLen`.
///   - Compression pointers are validated (offset < packetLen, offset >= 12).
///   - A bounded jump counter prevents pointer loops and excessive indirection.
///   - Label lengths are validated (<= 63).
///
/// On success, `out` receives the decoded name (dot-separated labels) and
/// `nextOffset` receives the offset just past the name in the original
/// (non-compressed) stream.
bool decodeDnsName(const uint8_t* packet, size_t packetLen,
                   size_t offset, std::string& out, size_t& nextOffset) {
    out.clear();
    size_t pos = offset;
    int jumps = 0;
    bool jumped = false;
    size_t jumpTarget = 0;

    while (true) {
        if (pos >= packetLen) {
            return false;   // out of bounds
        }

        uint8_t len = packet[pos];

        // Compression pointer (top two bits 11).
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= packetLen) {
                return false;   // pointer needs 2 bytes
            }
            uint16_t ptr = static_cast<uint16_t>(((len & 0x3F) << 8) |
                                                 packet[pos + 1]);
            if (ptr >= packetLen || ptr < 12) {
                return false;   // invalid pointer (into header or out of bounds)
            }
            if (!jumped) {
                // Record where the name continues after the pointer.
                nextOffset = pos + 2;
                jumped = true;
            }
            if (++jumps > kMaxPointerJumps) {
                return false;   // excessive indirection / loop
            }
            pos = ptr;
            continue;
        }

        // Reserved label types (top two bits 01 or 10) are invalid.
        if ((len & 0xC0) != 0) {
            return false;
        }

        // Zero-length label terminates the name.
        if (len == 0) {
            if (!jumped) {
                nextOffset = pos + 1;
            }
            // Trim trailing dot.
            if (!out.empty() && out.back() == '.') {
                out.pop_back();
            }
            return true;
        }

        // Validate label length.
        if (len > 63) {
            return false;
        }
        if (pos + 1 + len > packetLen) {
            return false;   // label extends past packet
        }

        // Append label.
        if (!out.empty()) {
            out.push_back('.');
        }
        for (uint8_t i = 0; i < len; ++i) {
            out.push_back(static_cast<char>(packet[pos + 1 + i]));
        }
        if (out.size() > kMaxDnsNameLen) {
            return false;   // name too long
        }

        pos += 1 + len;
    }
}

/// A single parsed record relevant to discovery.
struct PacketRecord {
    enum class Type { PTR, SRV, A, TXT } type;
    std::string ownerName;       ///< Record owner (decoded).
    std::string targetName;      ///< PTR target / SRV target.
    uint16_t    port = 0;        ///< SRV port.
    uint32_t    ipv4 = 0;        ///< A record address (network byte order).
    std::vector<std::string> txtStrings;  ///< TXT strings.
};

/// Parse a DNS/mDNS packet, collecting relevant records into `records`.
/// Returns false if the packet is malformed (caller ignores it safely).
bool parsePacket(const uint8_t* packet, size_t packetLen,
                 std::vector<PacketRecord>& records) {
    records.clear();

    // Minimum DNS header: 12 bytes.
    if (packetLen < 12) {
        return false;
    }

    // Header fields.
    uint16_t qdcount, ancount, nscount, arcount;
    if (!readU16(packet, packetLen, 4, qdcount) ||
        !readU16(packet, packetLen, 6, ancount) ||
        !readU16(packet, packetLen, 8, nscount) ||
        !readU16(packet, packetLen, 10, arcount)) {
        return false;
    }

    size_t pos = 12;

    // Skip question section.
    for (uint16_t q = 0; q < qdcount; ++q) {
        std::string qname;
        size_t next;
        if (!decodeDnsName(packet, packetLen, pos, qname, next)) {
            return false;
        }
        pos = next;
        // QTYPE (2) + QCLASS (2).
        if (pos + 4 > packetLen) {
            return false;
        }
        pos += 4;
    }

    // Iterate record sections (answer, authority, additional).
    uint16_t totalRecords = static_cast<uint16_t>(ancount + nscount + arcount);
    for (uint16_t r = 0; r < totalRecords; ++r) {
        if (records.size() >= kMaxPacketRecords) {
            break;   // bound temporary storage
        }

        // Record owner name.
        std::string owner;
        size_t next;
        if (!decodeDnsName(packet, packetLen, pos, owner, next)) {
            return false;
        }
        pos = next;

        // Type, class, TTL, RDLENGTH.
        uint16_t rtype, rclass, rdlength;
        uint32_t ttl;
        if (!readU16(packet, packetLen, pos, rtype) ||
            !readU16(packet, packetLen, pos + 2, rclass) ||
            !readU32(packet, packetLen, pos + 4, ttl) ||
            !readU16(packet, packetLen, pos + 8, rdlength)) {
            return false;
        }
        (void)rclass;
        (void)ttl;
        size_t rdataOffset = pos + 10;
        if (rdataOffset + rdlength > packetLen) {
            return false;   // RDATA extends past packet
        }

        // Parse only relevant record types.
        switch (rtype) {
        case kDnsTypePTR: {
            std::string target;
            size_t targetNext;
            if (!decodeDnsName(packet, packetLen, rdataOffset, target, targetNext)) {
                return false;
            }
            PacketRecord rec;
            rec.type = PacketRecord::Type::PTR;
            rec.ownerName = owner;
            rec.targetName = target;
            records.push_back(std::move(rec));
            break;
        }
        case kDnsTypeSRV: {
            // SRV RDATA: priority(2) + weight(2) + port(2) + target(name).
            if (rdlength < 6) {
                return false;   // SRV too short
            }
            uint16_t priority, weight, port;
            if (!readU16(packet, packetLen, rdataOffset, priority) ||
                !readU16(packet, packetLen, rdataOffset + 2, weight) ||
                !readU16(packet, packetLen, rdataOffset + 4, port)) {
                return false;
            }
            (void)priority;
            (void)weight;
            std::string target;
            size_t targetNext;
            if (!decodeDnsName(packet, packetLen, rdataOffset + 6, target, targetNext)) {
                return false;
            }
            PacketRecord rec;
            rec.type = PacketRecord::Type::SRV;
            rec.ownerName = owner;
            rec.targetName = target;
            rec.port = port;
            records.push_back(std::move(rec));
            break;
        }
        case kDnsTypeA: {
            if (rdlength != 4) {
                return false;   // A record must be exactly 4 bytes
            }
            // Read the 4 address bytes as big-endian (network byte order), so
            // the first packet byte is the most significant. ipv4ToDottedString
            // then produces the correct dotted-quad string. (A raw memcpy into
            // a uint32_t on a little-endian machine would reverse the bytes.)
            uint32_t addr = 0;
            if (!readU32(packet, packetLen, rdataOffset, addr)) {
                return false;
            }
            PacketRecord rec;
            rec.type = PacketRecord::Type::A;
            rec.ownerName = owner;
            rec.ipv4 = addr;
            records.push_back(std::move(rec));
            break;
        }
        case kDnsTypeTXT: {
            // TXT RDATA: sequence of length-prefixed strings.
            PacketRecord rec;
            rec.type = PacketRecord::Type::TXT;
            rec.ownerName = owner;
            size_t txtPos = rdataOffset;
            size_t txtEnd = rdataOffset + rdlength;
            while (txtPos < txtEnd) {
                uint8_t slen = packet[txtPos];
                if (txtPos + 1 + slen > txtEnd) {
                    return false;   // TXT string extends past RDATA
                }
                std::string s(reinterpret_cast<const char*>(packet + txtPos + 1), slen);
                rec.txtStrings.push_back(std::move(s));
                txtPos += 1 + slen;
            }
            records.push_back(std::move(rec));
            break;
        }
        default:
            // Ignore unrelated record types.
            break;
        }

        pos = rdataOffset + rdlength;
    }

    return true;
}

/// Extract the first label of a dotted FQDN (the instance/display name).
std::string firstLabel(const std::string& fqdn) {
    size_t dot = fqdn.find('.');
    if (dot == std::string::npos) {
        return fqdn;
    }
    return fqdn.substr(0, dot);
}

/// Format a network-order IPv4 address (as stored in an A record) into a
/// dotted-quad string. Avoids sceNetInetNtop (an SDK stub) and matches the
/// proven OpenOrbis reference (moonlight-ps4_2).
bool ipv4ToDottedString(uint32_t beAddr, char* out, size_t outLen) {
    if (out == nullptr || outLen < 16) {
        return false;
    }
    const unsigned int b1 = (beAddr >> 24) & 0xFF;
    const unsigned int b2 = (beAddr >> 16) & 0xFF;
    const unsigned int b3 = (beAddr >> 8) & 0xFF;
    const unsigned int b4 = beAddr & 0xFF;
    const int written = snprintf(out, outLen, "%u.%u.%u.%u", b1, b2, b3, b4);
    return written > 0 && static_cast<size_t>(written) < outLen;
}

} // namespace

// ---------------------------------------------------------------------------
// Worker thread entry
// ---------------------------------------------------------------------------

namespace {

/// Build an mDNS query packet for _nvstream._tcp.local.
/// `unicastPreferred` selects whether the unicast-response-preferred bit is
/// set in the question class.
void buildQueryPacket(uint8_t* buf, size_t bufSize, size_t& outLen,
                      bool unicastPreferred) {
    (void)bufSize;
    size_t pos = 0;

    // DNS header (12 bytes).
    buf[pos++] = 0x00;   // ID high
    buf[pos++] = 0x00;   // ID low (mDNS uses transaction ID 0)
    buf[pos++] = 0x00;   // Flags high
    buf[pos++] = 0x00;   // Flags low (standard query)
    buf[pos++] = 0x00;   // QDCOUNT high
    buf[pos++] = 0x01;   // QDCOUNT low (1 question)
    buf[pos++] = 0x00;   // ANCOUNT high
    buf[pos++] = 0x00;   // ANCOUNT low
    buf[pos++] = 0x00;   // NSCOUNT high
    buf[pos++] = 0x00;   // NSCOUNT low
    buf[pos++] = 0x00;   // ARCOUNT high
    buf[pos++] = 0x00;   // ARCOUNT low

    // QNAME: _nvstream._tcp.local
    // "_nvstream" (9 chars)
    buf[pos++] = 9;
    memcpy(buf + pos, "_nvstream", 9);
    pos += 9;
    // "_tcp" (4 chars)
    buf[pos++] = 4;
    memcpy(buf + pos, "_tcp", 4);
    pos += 4;
    // "local" (5 chars)
    buf[pos++] = 5;
    memcpy(buf + pos, "local", 5);
    pos += 5;
    // Root label.
    buf[pos++] = 0x00;

    // QTYPE: PTR (12).
    buf[pos++] = 0x00;
    buf[pos++] = 12;

    // QCLASS: IN (1) | unicast-response-preferred (0x8000).
    uint16_t qclass = kDnsClassIN;
    if (unicastPreferred) {
        qclass |= kDnsClassUnicastResponse;
    }
    buf[pos++] = static_cast<uint8_t>(qclass >> 8);
    buf[pos++] = static_cast<uint8_t>(qclass & 0xFF);

    outLen = pos;
}

} // namespace

/// Worker thread entry point.
void* DiscoveryManager::discoveryWorker(void* arg) {
    DiscoveryManager::Impl* impl =
        static_cast<DiscoveryManager::Impl*>(arg);

    discDiagPrint("[discovery] worker started");

    // --- Socket setup (done in worker so failures are contained) ------------
    // Create a UDP socket using the native sceNet* API. We deliberately do NOT
    // bind explicitly and do NOT join the mDNS multicast group: the proven
    // OpenOrbis path (moonlight-ps4_2) relies purely on unicast responses. The
    // first sendto auto-assigns an ephemeral source port, and responders send
    // a unicast response directly to that port (because every query sets the
    // unicast-response-preferred bit). This is the reliable path on OpenOrbis.
    OrbisNetId sock = sceNetSocket("moonlight-discovery",
                                   ORBIS_NET_AF_INET,
                                   ORBIS_NET_SOCK_DGRAM,
                                   0);
    if (sock < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[discovery] socket create failed (native %d)", sock);
        discDiagPrint(buf);
        impl->running = false;
        return nullptr;
    }
    impl->socketId = sock;

    // Resolve multicast destination address (the query is sent to the mDNS
    // multicast group; responses come back as unicast to our ephemeral port).
    OrbisNetInAddr mcastAddr;
    if (sceNetInetPton(ORBIS_NET_AF_INET, kMdnsMulticastAddr, &mcastAddr) != 1) {
        discDiagPrint("[discovery] inet_pton failed for multicast address");
        sceNetSocketClose(sock);
        impl->socketId = -1;
        impl->running = false;
        return nullptr;
    }

    OrbisNetSockaddrIn destAddr;
    memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sin_len    = sizeof(destAddr);
    destAddr.sin_family = ORBIS_NET_AF_INET;
    destAddr.sin_port   = sceNetHtons(kMdnsPort);
    destAddr.sin_addr   = mcastAddr.s_addr;

    // --- Main worker loop ----------------------------------------------------
    uint64_t nowMs = sceKernelGetProcessTime() / 1000;
    uint64_t nextQueryMs = nowMs;   // send first query immediately

    uint8_t recvBuf[kReceiveBufferSize];
    int rc = 0;   // sendto result

    discDiagPrint("[discovery] socket ready, entering loop");

    while (!impl->stopRequested.load()) {
        nowMs = sceKernelGetProcessTime() / 1000;

        // Send query if deadline reached.
        if (nowMs >= nextQueryMs) {
            // Send a burst of unicast-response-preference queries. This matches
            // the proven OpenOrbis implementation: responders are encouraged to
            // send a unicast response directly to our ephemeral source port.
            // Sending a plain (non-unicast) query can cause responders to reply
            // via multicast to port 5353, which an ephemeral socket cannot
            // receive, so we avoid it.
            for (int attempt = 0; attempt < 3 && !impl->stopRequested.load(); ++attempt) {
                uint8_t queryBuf[128];
                size_t queryLen = 0;

                // Send unicast-preference query.
                buildQueryPacket(queryBuf, sizeof(queryBuf), queryLen, true);
                rc = sceNetSendto(sock, queryBuf, queryLen, 0,
                                  reinterpret_cast<OrbisNetSockaddr*>(&destAddr),
                                  sizeof(destAddr));
                if (rc < 0) {
                    if (nowMs - impl->lastSendErrorLogMs > 10000) {
                        char buf[128];
                        snprintf(buf, sizeof(buf),
                                 "[discovery] send (unicast) failed (native %d)", rc);
                        discDiagPrint(buf);
                        impl->lastSendErrorLogMs = nowMs;
                    }
                } else if (nowMs - impl->lastSendErrorLogMs > 10000) {
                    // Rate-limit successful-send logging to once per 10 s.
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "[discovery] sent unicast query (%d bytes)", rc);
                    discDiagPrint(buf);
                    impl->lastSendErrorLogMs = nowMs;
                }

                // Receive responses for a burst window.
                const uint64_t burstEndMs = sceKernelGetProcessTime() / 1000 + kReceiveBurstWindowMs;
                while (!impl->stopRequested.load() &&
                       (sceKernelGetProcessTime() / 1000) < burstEndMs) {
                    receiveAndProcess(sock, recvBuf, sizeof(recvBuf), impl);
                    sceKernelUsleep(kReceivePollSleepUs);
                }
            }

            nextQueryMs = sceKernelGetProcessTime() / 1000 + kQueryIntervalMs;
        }

        // Drain any remaining packets between query cycles.
        receiveAndProcess(sock, recvBuf, sizeof(recvBuf), impl);

        sceKernelUsleep(kWorkerIdleSleepUs);
    }

    // --- Shutdown: close socket, mark not running ---------------------------
    if (impl->socketId >= 0) {
        sceNetSocketClose(impl->socketId);
        impl->socketId = -1;
    }
    impl->running = false;
    discDiagPrint("[discovery] worker stopped");
    return nullptr;
}

/// Non-blocking receive + parse + resolve + enqueue.
void DiscoveryManager::receiveAndProcess(int sock, uint8_t* recvBuf,
                                         size_t recvBufSize, Impl* impl) {
    if (sock < 0 || recvBuf == nullptr || impl == nullptr) {
        return;
    }

    for (;;) {
        OrbisNetSockaddrIn fromAddr;
        memset(&fromAddr, 0, sizeof(fromAddr));
        OrbisNetSocklen_t fromLen = sizeof(fromAddr);

        int n = sceNetRecvfrom(sock, recvBuf, recvBufSize, ORBIS_NET_MSG_DONTWAIT,
                               reinterpret_cast<OrbisNetSockaddr*>(&fromAddr),
                               &fromLen);

        if (n < 0) {
            // Non-blocking: EAGAIN/ETIMEDOUT means no more data right now.
            if (n != ORBIS_NET_ERROR_EAGAIN && n != ORBIS_NET_ERROR_ETIMEDOUT) {
                uint64_t nowMs = sceKernelGetProcessTime() / 1000;
                if (nowMs - impl->lastRecvErrorLogMs > 10000) {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "[discovery] recv failed (native %d)", n);
                    discDiagPrint(buf);
                    impl->lastRecvErrorLogMs = nowMs;
                }
            }
            break;
        }

        if (n == 0) {
            break;
        }

        // Diagnostic: log received packet source (rate-limited).
        {
            uint64_t nowMs = sceKernelGetProcessTime() / 1000;
            if (nowMs - impl->lastRecvErrorLogMs > 10000) {
                char ipBuf[ORBIS_NET_INET_ADDRSTRLEN];
                // fromAddr.sin_addr is in network byte order. On the
                // little-endian PS4, reading it as a uint32 yields a
                // byte-swapped value, so convert it to host order (which
                // produces the big-endian form ipv4ToDottedString expects)
                // before formatting. Without this the dotted-quad is reversed.
                const uint32_t hostAddr = sceNetNtohl(fromAddr.sin_addr);
                if (ipv4ToDottedString(hostAddr, ipBuf, sizeof(ipBuf))) {
                    char buf[192];
                    snprintf(buf, sizeof(buf),
                             "[discovery] recv %d bytes from %s:%u",
                             n, ipBuf,
                             static_cast<unsigned>(sceNetNtohs(fromAddr.sin_port)));
                    discDiagPrint(buf);
                }
                impl->lastRecvErrorLogMs = nowMs;
            }
        }

        // Parse the received packet.
        std::vector<PacketRecord> records;
        if (!parsePacket(recvBuf, static_cast<size_t>(n), records)) {
            // Malformed packet — ignore safely.
            continue;
        }

        // --- Resolve PTR -> SRV -> A ---------------------------------------
        for (const auto& rec : records) {
            if (rec.type != PacketRecord::Type::PTR) {
                continue;
            }
            // Only PTR records whose owner is _nvstream._tcp.local.
            if (rec.ownerName != kServiceName) {
                continue;
            }

            const std::string& instanceFqdn = rec.targetName;

            // Find matching SRV record.
            const PacketRecord* srv = nullptr;
            for (const auto& r : records) {
                if (r.type == PacketRecord::Type::SRV &&
                    r.ownerName == instanceFqdn) {
                    srv = &r;
                    break;
                }
            }
            if (!srv) {
                continue;   // incomplete — no SRV for this instance
            }

            // Find matching A record for the SRV target hostname.
            const PacketRecord* aRec = nullptr;
            for (const auto& r : records) {
                if (r.type == PacketRecord::Type::A &&
                    r.ownerName == srv->targetName) {
                    aRec = &r;
                    break;
                }
            }
            if (!aRec) {
                continue;   // incomplete — no A record for SRV target
            }

            // Build the observation.
            HostDiscoveryUpdate update;
            update.serviceFqdn  = instanceFqdn;
            update.instanceName = firstLabel(instanceFqdn);
            update.hostname     = srv->targetName;
            update.port         = srv->port;

            // Convert IPv4 to dotted string.
            char addrBuf[ORBIS_NET_INET_ADDRSTRLEN];
            if (!ipv4ToDottedString(aRec->ipv4, addrBuf, sizeof(addrBuf))) {
                continue;   // conversion failed — skip
            }
            update.ipv4Address = addrBuf;

            // --- Duplicate suppression --------------------------------------
            uint64_t nowMs = sceKernelGetProcessTime() / 1000;
            DiscoveryManager::Impl::ObservationKey key;
            key.instanceFqdn = instanceFqdn;
            key.ipv4         = update.ipv4Address;
            key.port         = update.port;

            bool isDuplicate = false;
            bool isRefresh = false;
            for (const auto& entry : impl->cache) {
                if (entry.first.instanceFqdn == key.instanceFqdn &&
                    entry.first.ipv4 == key.ipv4 &&
                    entry.first.port == key.port) {
                    if (nowMs - entry.second < kRefreshIntervalMs) {
                        isDuplicate = true;
                    } else {
                        isRefresh = true;   // known host, refresh interval elapsed
                    }
                    break;
                }
            }

            if (isDuplicate) {
                continue;   // suppressed
            }

            // Update/add cache entry.
            bool found = false;
            for (auto& entry : impl->cache) {
                if (entry.first.instanceFqdn == key.instanceFqdn &&
                    entry.first.ipv4 == key.ipv4 &&
                    entry.first.port == key.port) {
                    entry.second = nowMs;
                    found = true;
                    break;
                }
            }
            if (!found) {
                impl->cache.push_back({key, nowMs});
                if (impl->cache.size() > kCacheCapacity) {
                    impl->cache.pop_front();   // evict oldest
                }
            }

            // --- Enqueue update ---------------------------------------------
            // Log only on first discovery of a host, not on periodic refresh,
            // to keep the klog quiet. The update is still enqueued on refresh
            // so a future HostManager can track freshness.
            if (!isRefresh) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "[discovery] resolved host: %s (%s:%u)",
                         update.instanceName.c_str(),
                         update.ipv4Address.c_str(),
                         static_cast<unsigned>(update.port));
                discDiagPrint(buf);
            }

            pthread_mutex_lock(&impl->queueMutex);
            if (impl->queue.size() >= kQueueCapacity) {
                impl->queue.pop_front();   // drop oldest redundant
            }
            impl->queue.push_back(std::move(update));
            pthread_mutex_unlock(&impl->queueMutex);
        }
    }
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

DiscoveryManager::DiscoveryManager()
    : m_impl(new Impl())
{
}

DiscoveryManager::~DiscoveryManager() {
    if (m_impl) {
        stop();
        delete m_impl;
        m_impl = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

DiscoveryResult DiscoveryManager::start() {
    if (m_impl->running.load()) {
        return DiscoveryResult(DiscoveryResultCategory::AlreadyRunning);
    }

    // Initialize queue mutex.
    if (!m_impl->queueMutexInit) {
        int rc = pthread_mutex_init(&m_impl->queueMutex, nullptr);
        if (rc != 0) {
            return DiscoveryResult(DiscoveryResultCategory::InvalidState,
                                   static_cast<uint32_t>(rc));
        }
        m_impl->queueMutexInit = true;
    }

    // Reset state.
    m_impl->stopRequested = false;
    m_impl->running = true;
    m_impl->socketId = -1;
    m_impl->ephemeralPort = 0;
    m_impl->queue.clear();
    m_impl->cache.clear();

    // Create the worker thread.
    int rc = pthread_create(&m_impl->workerThread, nullptr,
                            discoveryWorker, m_impl);
    if (rc != 0) {
        m_impl->running = false;
        return DiscoveryResult(DiscoveryResultCategory::ThreadStartFailure,
                               static_cast<uint32_t>(rc));
    }
    m_impl->threadCreated = true;

    discDiagPrint("[discovery] worker thread created");
    return DiscoveryResult();
}

// ---------------------------------------------------------------------------
// Stop / Shutdown
// ---------------------------------------------------------------------------

void DiscoveryManager::stop() {
    if (!m_impl) {
        return;
    }

    // Signal stop.
    m_impl->stopRequested = true;

    // Join the worker thread. The worker uses non-blocking sceNetRecvfrom with
    // short sleeps, so it will observe stopRequested promptly and exit.
    if (m_impl->threadCreated) {
        pthread_join(m_impl->workerThread, nullptr);
        m_impl->threadCreated = false;
    }

    // Ensure socket is closed (worker closes it, but be safe).
    if (m_impl->socketId >= 0) {
        sceNetSocketClose(m_impl->socketId);
        m_impl->socketId = -1;
    }

    // Clear queue.
    pthread_mutex_lock(&m_impl->queueMutex);
    m_impl->queue.clear();
    pthread_mutex_unlock(&m_impl->queueMutex);

    m_impl->running = false;
    m_impl->cache.clear();
}

void DiscoveryManager::shutdown() {
    stop();
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool DiscoveryManager::isRunning() const {
    return m_impl && m_impl->running.load();
}

// ---------------------------------------------------------------------------
// Poll update
// ---------------------------------------------------------------------------

bool DiscoveryManager::pollUpdate(HostDiscoveryUpdate& out) {
    if (!m_impl) {
        return false;
    }

    pthread_mutex_lock(&m_impl->queueMutex);
    bool hasUpdate = !m_impl->queue.empty();
    if (hasUpdate) {
        out = std::move(m_impl->queue.front());
        m_impl->queue.pop_front();
    }
    pthread_mutex_unlock(&m_impl->queueMutex);

    return hasUpdate;
}

} // namespace ml