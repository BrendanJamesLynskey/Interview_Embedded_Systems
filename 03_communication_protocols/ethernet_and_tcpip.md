# Ethernet and TCP/IP for Embedded Systems

## Overview

Ethernet connectivity is increasingly required in embedded systems — from industrial Ethernet
(EtherCAT, PROFINET) to IoT edge devices and OTA update interfaces. Embedded engineers must
understand the TCP/IP stack not just conceptually but in the constrained context of a
microcontroller: limited RAM, no OS virtual memory, real-time constraints, and the requirement
to share CPU bandwidth with other tasks.

This file covers the MAC/PHY interface, lwIP (the de facto embedded TCP/IP stack), the socket
API, DHCP and DNS, and the system-level integration topics that come up in interviews for
connected embedded product roles.

---

## Fundamentals

### Q1. What is the Ethernet MAC/PHY split and what interface connects them?

**Question:** Explain the roles of the MAC and PHY in an Ethernet interface. What is MII/RMII
and how does data flow between them?

**Answer:**

**MAC (Media Access Control) — Layer 2:**
- Implemented inside the microcontroller or SoC.
- Handles: framing (preamble, SFD, source/destination addresses, EtherType, FCS), CSMA/CD
  (legacy), flow control (PAUSE frames), and the hardware DMA interface to system memory.
- Does NOT drive the physical wire.

**PHY (Physical Layer) — Layer 1:**
- A separate chip (e.g., LAN8720, DP83848, KSZ8081).
- Handles: signal encoding (MLT-3, PAM5 for Gigabit), PLL, link negotiation, RX signal
  equalisation, and the actual electrical interface to the twisted-pair cable.

**MII (Media Independent Interface):**

The standard interface between MAC and PHY, defined in IEEE 802.3:

```
MAC -> PHY (transmit):
  TX_CLK (25 MHz for 100BASE-TX, 2.5 MHz for 10BASE-T) — from PHY
  TXD[3:0]  — 4-bit nibble data
  TX_EN     — transmit enable

PHY -> MAC (receive):
  RX_CLK (25 MHz or 2.5 MHz) — from PHY
  RXD[3:0]  — 4-bit nibble data
  RX_DV     — receive data valid
  RX_ER     — receive error

MDC / MDIO — Management Data Clock / Interface (MIIM bus)
  MAC drives MDC; bidirectional MDIO accesses PHY registers.
  Used for: link status, speed, duplex, auto-negotiation control.
```

**RMII (Reduced MII):**

RMII uses a single 50 MHz reference clock and 2-bit data paths instead of 4-bit at 25 MHz.
This halves the pin count and is the standard for modern STM32/NXP MCUs with Ethernet:

```
Reference clock: 50 MHz (external oscillator, NOT from PHY in RMII)
TXD[1:0]   — 2-bit data, clocked at 50 MHz (100 Mbps = 50 MHz * 2 bits)
TX_EN      — transmit enable
RXD[1:0]   — 2-bit received data
CRS_DV     — carrier sense / receive data valid (multiplexed)
RX_ER      — receive error

Plus: MDC / MDIO for management (same as MII)
```

**RGMII (Reduced Gigabit MII):** used for Gigabit Ethernet; 4-bit data on both edges of
a 125 MHz clock, achieving 1000 Mbps.

**Data flow (transmit path):**

```
Application ─> TCP/IP stack ─> Ethernet driver ─> MAC DMA ─> MAC TX FIFO
               (adds headers)   (fills DMA desc.)  (reads mem)  -> MII/RMII -> PHY -> wire
```

---

### Q2. What is lwIP and what are its key architectural choices?

**Question:** Describe the lwIP (lightweight IP) stack architecture. What are its RAM modes
(NO_SYS vs RTOS)? What is the pbuf and why is it important?

**Answer:**

**lwIP (lightweight IP)** is an open-source TCP/IP stack designed for embedded systems with
8-64 KB of RAM. It implements IPv4, IPv6, TCP, UDP, ICMP, DHCP, DNS, and more.

**Operating modes:**

**1. NO_SYS mode (bare-metal):**

```c
/* No OS — all lwIP processing driven by polling */
void main_loop(void)
{
    lwip_init();
    netif_add(&netif, &ipaddr, &netmask, &gw,
              NULL, ethernetif_init, ethernet_input);
    netif_set_default(&netif);
    netif_set_up(&netif);

    for (;;) {
        /* Must call regularly to service timers and process received packets */
        sys_check_timeouts();

        /* Check for received Ethernet frames and pass to lwIP */
        ethernetif_input(&netif);

        /* Application polling (no blocking) */
        app_poll();
    }
}
```

**2. RTOS mode:**

lwIP runs in a dedicated thread. Received packets are passed via a mailbox from the
Ethernet interrupt to the lwIP thread. The application uses blocking socket calls.

```
[Ethernet ISR] -> posts pbuf to mailbox -> [lwIP thread] -> processes TCP/IP
                                         -> [App thread]  -> recv() blocks until data
```

**pbuf (packet buffer):**

The pbuf is lwIP's core data structure for managing network packet memory. It avoids
copying by using a chain of pbuf structures, each pointing to a region of memory:

```c
struct pbuf {
    struct pbuf *next;   /* next pbuf in chain (for fragmented packets) */
    void        *payload; /* pointer to data */
    u16_t        tot_len; /* total length of this + all following pbufs */
    u16_t        len;     /* length of this pbuf's data */
    u8_t         type;    /* PBUF_RAM, PBUF_ROM, PBUF_REF, PBUF_POOL */
    u8_t         flags;
    u16_t        ref;     /* reference count */
};
```

**pbuf types:**

| Type | Memory location | Use case |
|------|----------------|---------|
| PBUF_RAM | Heap (malloc) | Outgoing packets assembled by TCP/IP |
| PBUF_POOL | Fixed-size pool | Incoming packets (fast, no malloc) |
| PBUF_ROM | ROM/const | Zero-copy transmit of constant data |
| PBUF_REF | Application memory | Zero-copy transmit of application buffer |

**Why pbufs matter for performance:**

The pbuf chain allows zero-copy stack operation. An Ethernet frame arrives into a DMA
buffer; the driver creates a PBUF_REF pointing directly to that buffer. TCP/IP headers
are stripped by adjusting the payload pointer without copying data. The application reads
directly from the original DMA buffer. This is critical for high-throughput embedded
Ethernet on MCUs without cache coherency support.

---

### Q3. What is the BSD socket API and how does TCP connection establishment work?

**Question:** Write the server-side and client-side socket sequences for a TCP connection.
What happens at the protocol level during connect() and accept()?

**Answer:**

**Server-side sequence:**

```c
#include "lwip/sockets.h"  /* lwIP's POSIX socket compatibility layer */

int create_tcp_server(uint16_t port)
{
    /* 1. Create socket: IPv4, TCP */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;

    /* 2. Allow address reuse (avoid TIME_WAIT blocking re-bind) */
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 3. Bind to local address and port */
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),      /* network byte order */
        .sin_addr.s_addr = htonl(INADDR_ANY) /* accept on all interfaces */
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto error;

    /* 4. Start listening (backlog = 4: queue up to 4 pending connections) */
    if (listen(listen_fd, 4) < 0) goto error;

    /* 5. Accept a connection (blocks until a client connects) */
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    /* conn_fd is a new socket for this specific connection */

    return conn_fd;

error:
    close(listen_fd);
    return -1;
}
```

**Client-side sequence:**

```c
int connect_to_server(const char *server_ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port   = htons(port)
    };
    inet_aton(server_ip, &server.sin_addr);   /* "192.168.1.100" -> binary */

    /* connect() triggers the TCP three-way handshake and blocks until complete */
    if (connect(fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
```

**TCP three-way handshake (what happens inside connect/accept):**

```
Client                          Server
  |                               |
  |--- SYN (seq=ISN_c) ---------->|   connect() sends SYN
  |                               |   accept() wakes when SYN+ACK received
  |<-- SYN+ACK (seq=ISN_s,       |   Server sends SYN+ACK
  |            ack=ISN_c+1) ------|
  |--- ACK (ack=ISN_s+1) -------->|   connect() returns (connection established)
  |                               |   accept() returns conn_fd
  |                               |

ISN = Initial Sequence Number (randomly chosen to prevent spoofing)
```

**Why the three-way handshake?**

Two-way handshake would not guarantee the server's ISN is received by the client. The third
ACK (from client to server) confirms that the client received the server's SYN+ACK, meaning
both sides have confirmed the sequence numbers they will use for ordering and retransmission.

---

### Q4. Explain DHCP. What messages are exchanged and what is a DHCP lease?

**Question:** Describe the DORA sequence of DHCP. What happens when a DHCP lease expires?
How does an embedded device handle the case where no DHCP server is available?

**Answer:**

**DHCP (Dynamic Host Configuration Protocol)** automatically assigns IP addresses, subnet
masks, default gateways, and DNS server addresses to clients.

**The DORA sequence (UDP broadcasts):**

```
Client                              DHCP Server
  |                                     |
  |--- DHCP DISCOVER (broadcast) ------>|  Client has no IP; uses 0.0.0.0 src,
  |    src=0.0.0.0, dst=255.255.255.255 |  255.255.255.255 dst
  |    "I need an IP address"           |
  |                                     |
  |<-- DHCP OFFER (broadcast) ----------|  Server offers an IP
  |    Offered IP: 192.168.1.50         |  with lease time (e.g., 86400s)
  |    Subnet: 255.255.255.0            |
  |    Gateway: 192.168.1.1             |
  |    DNS: 8.8.8.8                     |
  |                                     |
  |--- DHCP REQUEST (broadcast) ------->|  Client formally requests the offered IP
  |    "I accept IP 192.168.1.50        |  (broadcast so other servers know)
  |     from server 192.168.1.1"        |
  |                                     |
  |<-- DHCP ACK (unicast or broadcast) -|  Server confirms the lease
  |    Lease time: 86400 seconds        |
  |    T1 (renew at): 43200s            |
  |    T2 (rebind at): 75600s           |
```

**Lease renewal (what happens at T1 and T2):**

- **At T1 (50% of lease):** Client unicasts a DHCP REQUEST to the same server to renew.
  Server responds with DHCP ACK (new lease start) or DHCP NAK (lease revoked).
- **At T2 (87.5% of lease):** If T1 renewal failed, client broadcasts DHCP REQUEST to
  ANY DHCP server (rebind phase).
- **At lease expiry:** Client must stop using the IP address and restart the DORA sequence.
  Any open TCP connections are terminated.

**lwIP DHCP integration:**

```c
#include "lwip/dhcp.h"

void start_dhcp(struct netif *netif)
{
    /* Set IP to 0.0.0.0 (DHCP will assign) */
    ip4_addr_set_zero(&netif->ip_addr);
    ip4_addr_set_zero(&netif->netmask);
    ip4_addr_set_zero(&netif->gw);

    dhcp_start(netif);   /* initiates DORA sequence */
}

void dhcp_wait_for_address(struct netif *netif)
{
    uint32_t timeout = 0;
    while (!dhcp_supplied_address(netif)) {
        sys_msleep(100);
        if (++timeout > 100) {   /* 10 second timeout */
            /* DHCP failed — fall back to link-local (APIPA) or static IP */
            dhcp_stop(netif);
            configure_fallback_ip(netif);   /* e.g., 169.254.x.x */
            return;
        }
    }
    /* IP assigned */
    printf("DHCP: got %s\n", ip4addr_ntoa(&netif->ip_addr));
}
```

**When no DHCP server is available:**

Options in order of preference:
1. **Link-local addressing (APIPA / RFC 3927):** The device randomly selects an address in
   169.254.0.0/16, uses ARP to check for conflicts, and assigns it. Works for direct
   device-to-device connections without a server.
2. **Static fallback IP:** The device uses a pre-configured static IP stored in non-volatile
   memory. Common in industrial devices that are always on a known subnet.
3. **Retry loop:** The device continues retrying DHCP indefinitely, perhaps with exponential
   backoff, until a server responds. Suitable for always-connected IoT devices.

---

## Intermediate

### Q5. How does TCP flow control and the receive window work?

**Question:** Explain TCP's sliding window flow control. What is the window size, how does it
get advertised, and what happens when the window reaches zero?

**Answer:**

TCP flow control prevents the sender from overwhelming the receiver's buffer. The mechanism
is the **receive window (rwnd)**: the receiver advertises how many bytes of buffer space it
currently has available.

**Window in the TCP header:**

```
TCP Header (partial):
  Sequence number:  32 bits  (byte offset of first byte in this segment)
  ACK number:       32 bits  (next expected byte from sender)
  Window size:      16 bits  (max bytes sender may have unacknowledged)
  Window scale opt: 3 bits   (shifts window left by 0-14 bits, max 1 GB window)
```

**Sliding window operation:**

```
Sender's view:
  |-- acknowledged -- | -- in-flight (unacked) -- | -- sendable -- | -- blocked --|
  0        SND.UNA    SND.NXT             SND.UNA + rwnd             ...
                      |----- send window ----------|

Sender may transmit bytes from SND.NXT up to (SND.UNA + rwnd - 1).
As ACKs arrive, SND.UNA advances, sliding the window forward.
```

**What happens when the window reaches zero:**

When `rwnd == 0`, the sender must stop sending data. However, the sender continues to send
**zero-window probes** (1-byte segments) at intervals to check if the window has reopened:

```c
/* In lwIP's tcp_timer handling: */
/* After tcp_persist_timer fires (when in zero-window state): */
void tcp_zero_window_probe(struct tcp_pcb *pcb)
{
    if (pcb->snd_wnd == 0) {
        /* Send 1-byte probe to force an ACK with the current window */
        tcp_output_segment(pcb, pcb->snd_nxt - 1, 1, 0);
        pcb->persist_backoff++;   /* exponential backoff on probes */
    }
}
```

The receiver responds to zero-window probes with an ACK containing the current (potentially
non-zero) window. When the receiver processes its buffer and frees space, its next ACK
advertises the new larger window, and the sender resumes.

**Silly window syndrome:** Occurs when the receiver opens the window by only a few bytes
(e.g., 1-2 bytes). The sender wastes a full TCP segment (40+ bytes of headers) for 1-2 bytes
of data. Nagle's algorithm (on the sender) and Clark's solution (on the receiver — don't
advertise small windows) mitigate this.

---

### Q6. Explain the Ethernet DMA descriptor ring and how the driver interacts with it.

**Question:** Describe the DMA descriptor ring mechanism used in Ethernet controllers
(e.g., STM32 or TI Sitara). How does the driver know when a received frame is ready?

**Answer:**

Ethernet controllers use a ring of DMA descriptors in memory. Each descriptor points to
a data buffer and contains status/control bits that are owned by either the CPU or the DMA.

**Descriptor structure (simplified STM32 EMAC-compatible):**

```c
/* Hardware-compatible descriptor — must be 4-byte aligned */
typedef struct __attribute__((aligned(4))) {
    volatile uint32_t status;   /* owned by DMA when bit 31 = 1 */
    uint32_t          ctrl;     /* buffer size, next descriptor address */
    uint8_t          *buf1;     /* pointer to first data buffer           */
    uint8_t          *buf2;     /* pointer to second buffer (or next desc) */
} EthDmaDesc;

/* Status bit definitions */
#define ETH_RDES0_OWN     (1u << 31)  /* 1 = DMA owns; 0 = CPU owns */
#define ETH_RDES0_FS      (1u <<  9)  /* First Segment of frame */
#define ETH_RDES0_LS      (1u <<  8)  /* Last Segment of frame */
#define ETH_RDES0_ES      (1u << 15)  /* Error Summary */
#define ETH_RDES0_FL_MASK (0x3FFF << 16) /* Frame Length [29:16] */
```

**Ring operation:**

```
Initial state (all descriptors owned by DMA):
  [DMA_OWN | buf=rx_buf[0]] -> [DMA_OWN | buf=rx_buf[1]] -> ... -> [CHAINED/RING]
         ^                                                                    |
         |___________________________________________________________________|
         DMA start pointer

When frame arrives:
  DMA writes frame data into rx_buf[N], clears OWN bit in descriptor N.
  DMA advances to descriptor N+1.

CPU polling or interrupt handler:
  for (int i = 0; i < NUM_RX_DESC; i++) {
      if (!(rx_desc[rx_idx].status & ETH_RDES0_OWN)) {
          /* CPU owns this descriptor — frame is ready */
          uint32_t len = (rx_desc[rx_idx].status & ETH_RDES0_FL_MASK) >> 16;
          process_received_frame(rx_desc[rx_idx].buf1, len);
          /* Return descriptor to DMA */
          rx_desc[rx_idx].status = ETH_RDES0_OWN;
          rx_idx = (rx_idx + 1) % NUM_RX_DESC;
      }
  }
```

**Cache coherency issue (critical for ARM Cortex-A):**

On processors with data caches, the DMA descriptor and buffer memory must be in a
non-cacheable region, or explicit cache maintenance operations must be performed:

```c
/* Before DMA writes: invalidate cache for the receive buffer region */
SCB_InvalidateDCache_by_Addr((uint32_t *)rx_buf[idx], sizeof(rx_buf[idx]));

/* After CPU writes descriptor: clean cache to push changes to memory */
SCB_CleanDCache_by_Addr((uint32_t *)&rx_desc[idx], sizeof(EthDmaDesc));
```

Failure to handle cache coherency causes the CPU to read stale data from its cache instead
of the DMA-written frame, producing random corruption that is extremely difficult to debug.

---

### Q7. How does DNS resolution work in an embedded context with lwIP?

**Question:** Describe how a DNS query is performed. What are the lwIP DNS API calls?
What happens when a name cannot be resolved?

**Answer:**

**DNS (Domain Name System)** translates hostnames (e.g., "api.example.com") to IP addresses.

**DNS query/response at the protocol level:**

```
Client (UDP src=random_port, dst=53):
  DNS Query: Type A, Class IN, Name "api.example.com"

DNS Server (UDP reply to client):
  DNS Response: 
    Answer: api.example.com -> 93.184.216.34 (TTL: 3600 seconds)
  (or: NXDOMAIN if name does not exist)
  (or: no response if server unreachable -> timeout)
```

DNS uses UDP port 53 for queries. Responses > 512 bytes use TCP, but this is rare for
typical A/AAAA record lookups.

**lwIP DNS API:**

```c
#include "lwip/dns.h"

/* Callback-based (asynchronous) DNS lookup */
static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    if (ipaddr != NULL) {
        printf("Resolved %s -> %s\n", name, ipaddr_ntoa(ipaddr));
        /* Store the IP and initiate TCP connection */
        struct ip_addr *result = (struct ip_addr *)arg;
        ip_addr_copy(*result, *ipaddr);
    } else {
        printf("DNS resolution failed for %s\n", name);
    }
}

void resolve_hostname(void)
{
    ip_addr_t resolved_ip;
    ip_addr_set_zero(&resolved_ip);

    err_t err = dns_gethostbyname("api.example.com",
                                  &resolved_ip,
                                  dns_callback,
                                  &resolved_ip);

    if (err == ERR_OK) {
        /* Already in cache — resolved_ip is valid immediately */
        connect_to_ip(&resolved_ip);
    } else if (err == ERR_INPROGRESS) {
        /* Query sent — dns_callback will be called when response arrives */
        /* Application must wait for callback before using the IP */
    } else {
        /* Immediate error — no DNS server configured, etc. */
    }
}
```

**DNS configuration in lwIP:**

```c
/* Set DNS servers (usually received from DHCP) */
ip_addr_t dns_server1, dns_server2;
IP4_ADDR(&dns_server1, 8, 8, 8, 8);      /* Google DNS */
IP4_ADDR(&dns_server2, 8, 8, 4, 4);
dns_setserver(0, &dns_server1);
dns_setserver(1, &dns_server2);
```

**Failure handling:**

| Failure | lwIP behaviour | Application should |
|---------|---------------|-------------------|
| No DNS server configured | ERR_ARG or no response | Use static IP, or configure DNS before calling |
| DNS server unreachable | Timeout after ~3 retries | Retry with exponential backoff |
| NXDOMAIN | Callback with ipaddr=NULL | Log error, do not retry (name doesn't exist) |
| DNS cache full | Oldest entry evicted | Tune LWIP_DNS_MAX_ENTRIES in lwipopts.h |

**DNS TTL caching in lwIP:**

lwIP caches resolved names. Subsequent lookups return the cached IP immediately (ERR_OK
with valid IP). The cache entry expires after the TTL. For long-running embedded devices,
TTL-based cache expiry ensures they pick up IP address changes (e.g., after server
migrations).

---

## Advanced

### Q8. What is TCP keepalive and why is it important for embedded systems?

**Question:** Explain TCP keepalive. Why do embedded devices typically need shorter keepalive
intervals than desktop systems? How do you configure keepalive in lwIP?

**Answer:**

**TCP keepalive** is a mechanism to detect and close dead connections (where the remote host
has crashed or the network path has been silently broken without either side sending a
FIN or RST).

**Why connections silently die:**
- Firewall or NAT state table entry expires (common: 30 seconds to 5 minutes for idle TCP)
- The remote host crashes and is later restarted (no RST sent)
- A network path change drops existing connections
- A cable is unplugged and replugged (same IP, but TCP state is gone on remote side)

**Keepalive mechanics:**

After a connection has been idle for `SO_KEEPALIVE_IDLE` seconds, the kernel sends a keepalive
probe (a TCP segment with sequence number = SND.NXT - 1, which is technically retransmitting
old data at 1 byte before the current sequence number):

```
TCP Keepalive probe: ACK only, seq = SND.NXT - 1

Expected responses:
  ACK              -> connection is alive, reset timer
  RST              -> remote host says "I have no state for this connection" -> close
  No response      -> send another probe after SO_KEEPALIVE_INTVL seconds
  N probes with no response -> close connection (ETIMEDOUT)
```

**Why embedded systems need shorter intervals:**

On a desktop system, the default keepalive idle is 2 hours. This is appropriate for long-lived
user sessions. For embedded systems:

1. **NAT expiry:** Many home routers expire NAT entries for idle TCP connections after
   30-300 seconds. An embedded device connecting through a router must send keepalives
   more frequently than the NAT timeout to keep the connection alive.

2. **Resource recovery:** Embedded systems have limited socket resources. A dead connection
   holding a socket, lwIP PCB, and DMA buffers must be detected and freed promptly.

3. **Reconnection latency:** An IoT device must detect a broken connection quickly to
   re-establish it and resume sending data within its SLA.

**lwIP keepalive configuration:**

```c
/* Enable keepalive globally in lwipopts.h */
/* LWIP_TCP_KEEPALIVE must be 1 in lwipopts.h */

/* Configure per-socket keepalive parameters */
void configure_keepalive(int sock_fd)
{
    int enable = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));

    /* Idle time before first probe: 30 seconds (default is 7200 = 2 hours) */
    int idle = 30;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));

    /* Interval between probes: 10 seconds */
    int interval = 10;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));

    /* Number of probes before declaring connection dead: 3 */
    int count = 3;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));

    /* Connection is declared dead after: 30 + 3*10 = 60 seconds of silence */
}
```

**In lwIP's PCB (direct API, bypassing sockets):**

```c
/* Using lwIP raw API for RTOS-less operation */
void tcp_set_keepalive(struct tcp_pcb *pcb)
{
    pcb->so_options |= SOF_KEEPALIVE;   /* enable keepalive */
    pcb->keep_idle   = 30000;           /* ms: 30 seconds idle before first probe */
    pcb->keep_intvl  = 10000;           /* ms: 10 seconds between probes */
    pcb->keep_cnt    = 3;               /* 3 probes before giving up */
}
```

---

### Q9. How does the lwIP raw (callback) API work and when should you use it instead of sockets?

**Question:** Describe the lwIP raw API (tcp_new, tcp_connect, tcp_recv, tcp_write). When is
it preferable to the socket API? What are the constraints on callback functions?

**Answer:**

**Two APIs in lwIP:**

| API | Description | Use case |
|-----|-------------|---------|
| Socket API | POSIX-compatible (socket, bind, connect, send, recv) | RTOS environments; portability |
| Raw/callback API | Event-driven callbacks; no blocking | NO_SYS (bare-metal); memory efficiency |

**The raw API requires no OS.** All network processing happens in the lwIP context (either
the main loop timer in NO_SYS mode, or the lwIP thread in RTOS mode). Callbacks are invoked
synchronously within that context.

**Example: HTTP client using the raw API:**

```c
#include "lwip/tcp.h"

static struct tcp_pcb *http_pcb = NULL;
static const char *http_request = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";

/* Called when connection is established */
static err_t http_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    if (err != ERR_OK) { return err; }

    /* Send HTTP request */
    err = tcp_write(pcb,
                    http_request,
                    strlen(http_request),
                    TCP_WRITE_FLAG_COPY);   /* copy to lwIP's buffer */
    if (err == ERR_OK) {
        tcp_output(pcb);   /* flush immediately */
    }
    return err;
}

/* Called when data is received */
static err_t http_recv(void *arg, struct tcp_pcb *pcb,
                       struct pbuf *p, err_t err)
{
    if (p == NULL) {
        /* Connection closed by remote */
        tcp_close(pcb);
        http_pcb = NULL;
        return ERR_OK;
    }

    /* Process received data (p is a pbuf chain) */
    struct pbuf *q = p;
    while (q != NULL) {
        /* Process q->payload[0..q->len-1] */
        process_http_data((uint8_t *)q->payload, q->len);
        q = q->next;
    }

    /* Tell TCP we have consumed 'p->tot_len' bytes */
    tcp_recved(pcb, p->tot_len);

    /* Free the pbuf chain */
    pbuf_free(p);
    return ERR_OK;
}

/* Called when the connection is aborted (error) */
static void http_error(void *arg, err_t err)
{
    http_pcb = NULL;
    /* Schedule reconnect if needed */
}

void http_start_request(const ip_addr_t *server_ip)
{
    http_pcb = tcp_new();
    if (!http_pcb) return;

    tcp_arg(http_pcb, NULL);              /* user argument passed to callbacks */
    tcp_recv(http_pcb, http_recv);        /* register receive callback */
    tcp_err(http_pcb, http_error);        /* register error callback */

    tcp_connect(http_pcb, server_ip, 80, http_connected);
}
```

**Constraints on raw API callbacks:**

1. **Must not block.** Callbacks run in the lwIP context. Blocking (e.g., waiting for a
   mutex, calling osDelay) will stall the entire TCP/IP stack, including timers and ACKs.

2. **Must call tcp_recved() for every received byte.** Failing to call tcp_recved() prevents
   the receive window from advancing, eventually causing the remote side to stop sending.

3. **Must return ERR_OK** (or ERR_ABRT if you called tcp_abort). Returning ERR_MEM causes
   lwIP to retry the delivery.

4. **Must not call tcp_write with more bytes than tcp_sndbuf().** Check available send
   buffer space before writing:
   ```c
   if (tcp_sndbuf(pcb) >= data_len) {
       tcp_write(pcb, data, data_len, TCP_WRITE_FLAG_COPY);
   }
   ```

5. **Callback for tcp_sent:** To know when lwIP has successfully delivered a segment and
   freed its buffer, register a tcp_sent callback. This is needed for flow control in
   streaming scenarios.

---

### Q10. How do you diagnose and fix common embedded Ethernet/lwIP problems?

**Question:** List five common failure modes in embedded Ethernet/lwIP systems. For each,
describe the symptoms, root cause, and fix.

**Answer:**

**1. Intermittent packet loss at high throughput — Rx descriptor ring underrun**

*Symptoms:* Transfers work at low speed but drop packets at high speed. Wireshark shows
retransmissions. The MAC's missed frame counter (MFC in ETH_DMASR) increments.

*Root cause:* The Rx DMA descriptor ring is exhausted before the CPU can return descriptors.
The MAC must drop incoming frames.

*Fix:* Increase the number of Rx descriptors. Move descriptor ring processing to a higher
interrupt priority. Use DMA interrupts (not polling) to return descriptors promptly.

```c
/* Increase ring size (each 1500-byte descriptor uses ~1.5 KB of RAM) */
#define NUM_RX_DESC  8    /* minimum */
#define NUM_RX_DESC  24   /* better for high throughput */
```

**2. Stale data / corruption — cache coherency failure**

*Symptoms:* On Cortex-M7 or Cortex-A processors, received data is correct in Wireshark but
arrives corrupted in the application. Often appears as constant byte values (0x00 or 0xFF)
in the buffer.

*Root cause:* DMA writes to uncached memory, but the CPU's D-cache has stale data for that
address range (from before the DMA write). The CPU reads its cache, not the new data.

*Fix:* Declare DMA buffers and descriptors as non-cacheable (MPU region), or add explicit
cache invalidation after DMA completes.

```c
/* Place Ethernet buffers in non-cacheable memory (STM32H7) */
__attribute__((section(".noncacheable"))) EthDmaDesc rx_descs[NUM_RX_DESC];
__attribute__((section(".noncacheable"))) uint8_t rx_bufs[NUM_RX_DESC][ETH_FRAME_SIZE];
```

**3. DHCP fails silently — no default gateway**

*Symptoms:* Device gets an IP address but cannot reach hosts outside the local subnet.
Ping to gateway fails.

*Root cause:* DHCP response received, IP and netmask set, but gateway not applied to netif.
lwIP's `netif_set_default()` was not called, or the gateway IP was set before the netif was
added.

*Fix:* Verify the gateway is applied and the interface is set as default:
```c
dhcp_wait_for_address(&netif);
printf("IP:  %s\n", ip4addr_ntoa(&netif.ip_addr));
printf("GW:  %s\n", ip4addr_ntoa(&netif.gw));        /* should be non-zero */
netif_set_default(&netif);                             /* must be called */
```

**4. TCP connection hangs — Nagle's algorithm**

*Symptoms:* Small data writes take 200 ms to arrive at the receiver. Throughput for
request/response protocols (MQTT, HTTP with small messages) is unexpectedly low.

*Root cause:* Nagle's algorithm buffers small segments until either a full-size segment
accumulates or all outstanding data is acknowledged. For interactive or small-message
protocols, this adds up to one RTT of delay per message.

*Fix:* Disable Nagle's algorithm for latency-sensitive connections:
```c
int nodelay = 1;
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
/* Raw API: */
tcp_nagle_disable(pcb);
```

**5. Memory exhaustion — pbuf pool depletion**

*Symptoms:* After running for hours, new TCP connections fail. ERR_MEM returned from
tcp_new() or accept(). Existing connections continue working.

*Root cause:* pbuf pool or lwIP memory pool is exhausted. Pbufs are not being freed
(tcp_recved() not called, or pbuf_free() missed on error paths).

*Fix:*
```c
/* Monitor pbuf pool in lwipopts.h */
#define PBUF_POOL_SIZE     24   /* increase pool */
#define MEM_SIZE           (16 * 1024)  /* increase heap */

/* In application: always free pbufs on all code paths */
err_t recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (p == NULL) { tcp_close(pcb); return ERR_OK; }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);   /* MUST free, including on error paths */
    return ERR_OK;
}
```

---

## Summary Reference Table

| Topic | Key Parameter | Typical Embedded Value | Notes |
|-------|--------------|----------------------|-------|
| RMII reference clock | 50 MHz | External oscillator | PHY may provide it |
| Rx descriptor ring | NUM_RX_DESC | 8-24 | Each = 1 MTU buffer (~1.5 KB) |
| lwIP heap (MEM_SIZE) | Bytes | 8-64 KB | Depends on simultaneous connections |
| pbuf pool | PBUF_POOL_SIZE | 16-32 | Each = PBUF_POOL_BUFSIZE bytes |
| TCP max connections | MEMP_NUM_TCP_PCB | 4-16 | Each PCB = ~200 bytes |
| DNS cache | LWIP_DNS_MAX_ENTRIES | 4-8 | Increase for multi-host clients |
| DHCP T1 | 50% of lease time | ~12 hours for 24h lease | Server configures |
| TCP keepalive idle | TCP_KEEPIDLE | 30-60 s embedded | 7200 s desktop default |
| Nagle delay | One RTT | 1-200 ms | Disable with TCP_NODELAY |
| MTU | 1500 bytes | 1500 bytes (Ethernet) | Reduce for slow/lossy links |
