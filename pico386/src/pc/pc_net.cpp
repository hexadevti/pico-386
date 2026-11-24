// pc_net.cpp -- the PC's network: a user-mode NAT behind the NE2000 (see pc_net.h).
//
// The P4's Wi-Fi is a station: it can't bridge frames from a second MAC, so the guest gets its own
// little LAN, as QEMU's slirp gives it, and its connections are carried on the host's sockets:
//   - ARP for the router (10.0.2.2) and DNS (10.0.2.3) is answered here, DHCP hands out 10.0.2.15.
//   - DNS queries for A records are resolved on a worker thread (getaddrinfo blocks).
//   - TCP is terminated here: each guest connection is a host socket. This side keeps the bytes
//     the guest has not acknowledged and resends them (go-back-N) when it times out; the guest's
//     NE2000 ring can overflow, so frames do get lost.
//   - UDP goes out through one host socket per guest port.
//   - ICMP echo is answered for the router and DNS only.
// With a proxy (Protoweb, https://protoweb.org), connections to port 80 go to the proxy and each
// request line is rewritten to the absolute form a proxy wants ("GET http://host/path"), using the
// Host header. DNS then answers every name with an address of its own (10.99.x.x) and keeps the
// name: Protoweb serves sites whose domains no longer exist, and the oldest browsers send no Host
// header at all. Other ports to those addresses resolve the name for real when they connect.
//
// Everything here runs on the emulator's thread (pcnet_poll, from each pc_step), except the
// resolver. The RP2350 has no radio and no room: it gets stubs.
//
// The browser (EMU_WEB) has no sockets. There only the proxy's connections go out, each through a
// WebSocket tunnel (hal_tun_*) to the worker, which reaches only Protoweb's proxies; other TCP is
// refused, UDP goes nowhere, and DNS answers only from the proxy's fake addresses.
#include "pc_net.h"
#include "../../board.h"
#include <stdio.h>
#include <string.h>

#if defined(PICO386_LOWMEM)

int pcnet_enabled(void) { return 0; }
void pcnet_guest_mac(uint8_t mac[6]) { memset(mac, 0, 6); }
void pcnet_guest_frame(const uint8_t *, int) {}
void pcnet_poll(void *, int (*)(void *), void (*)(void *, const uint8_t *, int)) {}
void pcnet_configure(int, const char *, int) {}
void pcnet_reset(void) {}
void pcnet_status(char *buf, size_t n) { if (n) snprintf(buf, n, "no network on this board"); }

#else

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../hal/hal.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
static const sock_t kNoSock = INVALID_SOCKET;
static void sclose(sock_t s) { closesocket(s); }
static bool wouldBlock() {
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
static void setNonblock(sock_t s) { u_long one = 1; ioctlsocket(s, FIONBIO, &one); }
static void netInit() {
  static bool done;
  if (done) return;
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
  done = true;
}
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
typedef int sock_t;
static const sock_t kNoSock = -1;
#if defined(EMU_WEB)
static void sclose(sock_t s) { hal_tun_close(s); }   // a tunnel's id, never a socket
#else
static void sclose(sock_t s) { close(s); }
#endif
static bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
static void setNonblock(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static void netInit() {}
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#if defined(BOARD_PICOCALC_P4)
#include "esp_pthread.h"
#endif

namespace {

// ---- addresses (host byte order) ------------------------------------------------------------
constexpr uint32_t kNet = 0x0A000200;      // 10.0.2.0/24
constexpr uint32_t kMask = 0xFFFFFF00;
constexpr uint32_t kGwIp = 0x0A000202;     // 10.0.2.2, the router
constexpr uint32_t kDnsIp = 0x0A000203;    // 10.0.2.3
constexpr uint32_t kGuestIp = 0x0A00020F;  // 10.0.2.15, what DHCP gives
constexpr uint32_t kFakeBase = 0x0A630001; // 10.99.0.1: names DNS answered for the proxy
constexpr int kFakeMax = 4096;
const uint8_t kGwMac[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
const uint8_t kGuestMac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
const uint8_t kBcastMac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

// ---- limits ---------------------------------------------------------------------------------
constexpr int kQueue = 48;                 // frames waiting for the card's ring
constexpr int kMaxTcp = 12;                // lwIP has 16 sockets in all
constexpr int kMaxUdp = 4;
constexpr size_t kToHostCap = 16384;       // also the window offered to the guest
constexpr size_t kToGuestCap = 32768;
constexpr int kMss = 1460;
constexpr uint32_t kRto0 = 1000;           // ms
constexpr uint32_t kServiceMs = 4;

uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void put16(uint8_t *p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
bool seqLt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
bool seqLe(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
bool inNet(uint32_t ip) { return (ip & kMask) == kNet; }

std::string dotted(uint32_t ip) {
  char b[16];
  snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
  return b;
}

uint32_t sum16(const uint8_t *p, int n, uint32_t s) {
  for (; n > 1; p += 2, n -= 2) s += (uint32_t)(p[0] << 8 | p[1]);
  if (n) s += (uint32_t)p[0] << 8;
  return s;
}
uint16_t fold(uint32_t s) {
  while (s >> 16) s = (s & 0xffff) + (s >> 16);
  return (uint16_t)~s;
}

// ---- settings and state ---------------------------------------------------------------------
bool s_enabled;
std::string s_proxyHost;
int s_proxyPort;
uint8_t s_gmac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
uint16_t s_ipId;
uint32_t s_lastService;
uint32_t s_rxBytes, s_txBytes;             // guest payload through the sockets

struct Frame { uint16_t len; uint8_t d[1514]; };
Frame *s_q;
int s_qHead, s_qCount;
int qFree() { return s_q ? kQueue - s_qCount : 0; }

// The names behind the proxy's addresses (kFakeBase + index).
std::vector<std::string> s_fake;
int s_fakeNext;

uint32_t fakeFor(const std::string &name) {
  for (size_t i = 0; i < s_fake.size(); i++)
    if (s_fake[i] == name) return kFakeBase + (uint32_t)i;
  if ((int)s_fake.size() < kFakeMax) {
    s_fake.push_back(name);
    return kFakeBase + (uint32_t)(s_fake.size() - 1);
  }
  const int i = s_fakeNext;
  s_fakeNext = (s_fakeNext + 1) % kFakeMax;
  s_fake[i] = name;
  return kFakeBase + (uint32_t)i;
}
const std::string *fakeName(uint32_t ip) {
  if (ip < kFakeBase || ip >= kFakeBase + s_fake.size()) return nullptr;
  return &s_fake[ip - kFakeBase];
}

// ---- the resolver thread --------------------------------------------------------------------
struct Resolved { std::string name; int state; uint32_t ip; uint32_t at; };   // state 0 asked, 1 ok, -1 failed
std::mutex s_rm;
std::condition_variable s_rcv;
std::vector<Resolved> s_cache;
std::deque<std::string> s_todo;
bool s_resolverUp;

void resolverMain() {
  for (;;) {
    std::string name;
    {
      std::unique_lock<std::mutex> lk(s_rm);
      s_rcv.wait(lk, [] { return !s_todo.empty(); });
      name = s_todo.front();
      s_todo.pop_front();
    }
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    uint32_t ip = 0;
    const int r = getaddrinfo(name.c_str(), nullptr, &hints, &res);
    if (r == 0 && res) ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    if (res) freeaddrinfo(res);
    std::lock_guard<std::mutex> lk(s_rm);
    for (auto &e : s_cache)
      if (e.name == name) { e.state = ip ? 1 : -1; e.ip = ip; e.at = hal_millis(); }
  }
}

void resolverStart() {
#if defined(EMU_WEB)
  return;                                         // no getaddrinfo in a page
#endif
  if (s_resolverUp) return;
  netInit();
#if defined(BOARD_PICOCALC_P4)
  // getaddrinfo wants more than the default pthread stack; core 0 has the room (core 1 runs the PC)
  esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
  cfg.stack_size = 6144;
  cfg.pin_to_core = 0;
  cfg.thread_name = "pcnet-dns";
  esp_pthread_set_cfg(&cfg);
#endif
  std::thread(resolverMain).detach();
  s_resolverUp = true;
}

// 1: *ip is the address; -1: no such name; 0: asked, try again later.
int resolve(const std::string &name, uint32_t *ip) {
  struct in_addr a;
  if (inet_pton(AF_INET, name.c_str(), &a) == 1) { *ip = ntohl(a.s_addr); return 1; }
#if defined(EMU_WEB)
  return -1;
#endif
  resolverStart();
  const uint32_t now = hal_millis();
  std::lock_guard<std::mutex> lk(s_rm);
  for (size_t i = 0; i < s_cache.size(); i++) {
    Resolved &e = s_cache[i];
    if (e.name != name) continue;
    const uint32_t age = now - e.at;
    if (e.state == 1 && age < 600000) { *ip = e.ip; return 1; }
    if (e.state == -1 && age < 30000) return -1;
    if (e.state == 0) return 0;
    s_cache.erase(s_cache.begin() + i);           // stale: ask again
    break;
  }
  if (s_cache.size() >= 64) s_cache.erase(s_cache.begin());
  s_cache.push_back({ name, 0, 0, now });
  s_todo.push_back(name);
  s_rcv.notify_one();
  return 0;
}

// ---- frames to the guest --------------------------------------------------------------------
// The payload after the IPv4 header of a new frame at the queue's tail, or null when it is full.
uint8_t *beginIp(int l4len) {
  if (!qFree() || 34 + l4len > 1514) return nullptr;
  return s_q[(s_qHead + s_qCount) % kQueue].d + 34;
}

// Fills in the Ethernet and IPv4 headers (and the TCP/UDP checksum) of the frame beginIp gave,
// and queues it.
void endIp(uint32_t src, uint32_t dst, uint8_t proto, int l4len, const uint8_t *dmac = nullptr) {
  Frame &f = s_q[(s_qHead + s_qCount) % kQueue];
  uint8_t *e = f.d, *ip = f.d + 14, *l4 = f.d + 34;
  memcpy(e, dmac ? dmac : s_gmac, 6);
  memcpy(e + 6, kGwMac, 6);
  put16(e + 12, 0x0800);
  ip[0] = 0x45; ip[1] = 0;
  put16(ip + 2, 20 + l4len);
  put16(ip + 4, s_ipId++);
  put16(ip + 6, 0x4000);
  ip[8] = 64; ip[9] = proto;
  put16(ip + 10, 0);
  put32(ip + 12, src);
  put32(ip + 16, dst);
  put16(ip + 10, fold(sum16(ip, 20, 0)));
  const int co = proto == 6 ? 16 : proto == 17 ? 6 : -1;
  if (co >= 0) {
    put16(l4 + co, 0);
    uint32_t s = sum16(ip + 12, 8, 0) + proto + (uint32_t)l4len;
    uint16_t c = fold(sum16(l4, l4len, s));
    if (proto == 17 && !c) c = 0xffff;
    put16(l4 + co, c);
  } else if (proto == 1) {
    put16(l4 + 2, 0);
    put16(l4 + 2, fold(sum16(l4, l4len, 0)));
  }
  f.len = (uint16_t)(34 + l4len);
  s_qCount++;
}

bool sendUdp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, int n,
             const uint8_t *dmac = nullptr) {
  uint8_t *p = beginIp(8 + n);
  if (!p) return false;
  put16(p, sport); put16(p + 2, dport); put16(p + 4, 8 + n); put16(p + 6, 0);
  memcpy(p + 8, data, n);
  endIp(src, dst, 17, 8 + n, dmac);
  return true;
}

// ---- TCP ------------------------------------------------------------------------------------
enum { FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16 };
enum TcpState { T_FREE, T_RESOLVING, T_CONNECTING, T_SYNACK, T_EST };

struct HttpRw {
  enum { HDR, BODY, PASS } st = HDR;
  std::string hdr;
  long left = 0;
};

struct Tcp {
  TcpState state = T_FREE;
  uint32_t gip = 0, rip = 0;                // guest, and the address the guest thinks it talks to
  uint16_t gport = 0, rport = 0;
  sock_t s = kNoSock;
  std::string target;                       // the name to resolve, if any
  uint16_t targetPort = 0;
  uint32_t iss = 0, snd_una = 0, snd_nxt = 0, snd_max = 0, rcv_nxt = 0;
  uint32_t gwin = 0;
  int gmss = 536;
  std::vector<uint8_t> toGuest, toHost;
  bool hostEof = false, finSent = false, finAcked = false, guestFin = false, shutWr = false;
  bool needAck = false, probe = false, smallWin = false;
  uint32_t finSeq = 0;
  uint32_t rto = kRto0, rtoAt = 0, since = 0, last = 0;
  int retries = 0;
  HttpRw *rw = nullptr;                     // the proxy's request rewriter
  std::string hostName;                     // for requests with no Host header
};

Tcp s_tcp[kMaxTcp];

void tcpFree(Tcp &c) {
  if (c.s != kNoSock) sclose(c.s);
  delete c.rw;
  c = Tcp();
}

uint16_t tcpWindow(const Tcp &c) {
  const size_t used = c.toHost.size();
  return used >= kToHostCap ? 0 : (uint16_t)std::min<size_t>(kToHostCap - used, 65535);
}

bool tcpSend(Tcp &c, uint8_t flags, uint32_t seq, const uint8_t *data, int n, bool mss = false) {
  const int hl = mss ? 24 : 20;
  uint8_t *p = beginIp(hl + n);
  if (!p) return false;
  put16(p, c.rport); put16(p + 2, c.gport);
  put32(p + 4, seq);
  put32(p + 8, (flags & ACK) ? c.rcv_nxt : 0);
  p[12] = (uint8_t)((hl / 4) << 4);
  p[13] = flags;
  const uint16_t w = tcpWindow(c);
  c.smallWin = w < kMss;
  put16(p + 14, w);
  put16(p + 16, 0); put16(p + 18, 0);
  if (mss) { p[20] = 2; p[21] = 4; put16(p + 22, kMss); }
  if (n) memcpy(p + hl, data, n);
  endIp(c.rip, c.gip, 6, hl + n);
  return true;
}

// A reset for a segment that has no connection (or one that is being dropped).
void tcpRstFor(uint32_t gip, uint16_t gport, uint32_t rip, uint16_t rport, uint32_t seq, uint32_t ack,
               uint8_t fl, int dlen) {
  uint8_t *p = beginIp(20);
  if (!p) return;
  put16(p, rport); put16(p + 2, gport);
  if (fl & ACK) { put32(p + 4, ack); put32(p + 8, 0); p[13] = RST; }
  else {
    put32(p + 4, 0);
    put32(p + 8, seq + dlen + ((fl & SYN) ? 1 : 0) + ((fl & FIN) ? 1 : 0));
    p[13] = RST | ACK;
  }
  p[12] = 5 << 4;
  put16(p + 14, 0); put16(p + 16, 0); put16(p + 18, 0);
  endIp(rip, gip, 6, 20);
}

void tcpAbort(Tcp &c) {
  if (c.state == T_EST || c.state == T_SYNACK) tcpSend(c, RST | ACK, c.snd_nxt, nullptr, 0);
  else tcpRstFor(c.gip, c.gport, c.rip, c.rport, c.rcv_nxt - 1, 0, SYN, 0);
  tcpFree(c);
}

void tcpConnect(Tcp &c, uint32_t ip) {
  netInit();
  c.s = socket(AF_INET, SOCK_STREAM, 0);
  if (c.s == kNoSock) { hal_log("net: no socket"); tcpAbort(c); return; }
  setNonblock(c.s);
  int one = 1;
  setsockopt(c.s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(c.targetPort);
  a.sin_addr.s_addr = htonl(ip);
  hal_log("net: tcp %s:%u -> %s:%u", dotted(c.rip).c_str(), c.rport, dotted(ip).c_str(), c.targetPort);
  if (connect(c.s, (struct sockaddr *)&a, sizeof a) != 0 && !wouldBlock()) { hal_log("net: connect failed"); tcpAbort(c); return; }
  c.state = T_CONNECTING;
  c.since = hal_millis();
}

void tcpSynAck(Tcp &c) {
  tcpSend(c, SYN | ACK, c.iss, nullptr, 0, true);
  c.rtoAt = hal_millis() + c.rto;
}

// Guest bytes for the host: through the proxy's rewriter, or as they are.
void httpEmit(Tcp &c);
void tcpToHost(Tcp &c, const uint8_t *d, int n) {
  if (!c.rw) { c.toHost.insert(c.toHost.end(), d, d + n); return; }
  HttpRw &h = *c.rw;
  while (n > 0) {
    if (h.st == HttpRw::PASS) { c.toHost.insert(c.toHost.end(), d, d + n); return; }
    if (h.st == HttpRw::BODY) {
      const int k = (int)std::min<long>(n, h.left);
      c.toHost.insert(c.toHost.end(), d, d + k);
      d += k; n -= k; h.left -= k;
      if (!h.left) h.st = HttpRw::HDR;
      continue;
    }
    h.hdr.push_back((char)*d++);
    n--;
    if (h.hdr.back() != '\n') {
      if (h.hdr.size() > 16384) {                 // not HTTP after all
        c.toHost.insert(c.toHost.end(), h.hdr.begin(), h.hdr.end());
        h.hdr.clear();
        h.st = HttpRw::PASS;
      }
      continue;
    }
    if (h.hdr == "\r\n" || h.hdr == "\n") {       // a stray line between requests
      c.toHost.insert(c.toHost.end(), h.hdr.begin(), h.hdr.end());
      h.hdr.clear();
      continue;
    }
    const size_t nl = h.hdr.find('\n');
    const bool simple = nl == h.hdr.size() - 1 &&
                        std::count(h.hdr.begin(), h.hdr.end(), ' ') < 2;   // HTTP/0.9: "GET /x"
    const size_t L = h.hdr.size();
    const bool end = (L >= 4 && !h.hdr.compare(L - 4, 4, "\r\n\r\n")) || (L >= 2 && !h.hdr.compare(L - 2, 2, "\n\n"));
    if (simple || end) httpEmit(c);
  }
}

bool startsWithNoCase(const std::string &s, size_t at, const char *p) {
  return !strncasecmp(s.c_str() + at, p, strlen(p));
}

// One whole request head is in rw->hdr: rewrite its request line for the proxy and send it.
void httpEmit(Tcp &c) {
  HttpRw &h = *c.rw;
  std::string &r = h.hdr;
  const size_t eol = r.find('\n');
  std::string line = r.substr(0, eol);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::string rest = r.substr(eol + 1);
  // the headers: Host, Content-Length, Transfer-Encoding
  std::string host;
  long clen = 0;
  bool chunked = false;
  for (size_t at = 0; at < rest.size();) {
    size_t e = rest.find('\n', at);
    if (e == std::string::npos) e = rest.size();
    if (startsWithNoCase(rest, at, "host:")) {
      host = rest.substr(at + 5, e - at - 5);
      while (!host.empty() && isspace((unsigned char)host.back())) host.pop_back();
      while (!host.empty() && isspace((unsigned char)host[0])) host.erase(0, 1);
    } else if (startsWithNoCase(rest, at, "content-length:")) {
      clen = atol(rest.c_str() + at + 15);
    } else if (startsWithNoCase(rest, at, "transfer-encoding:")) {
      chunked = rest.find("chunked", at) < e;
    }
    at = e + 1;
  }
  const bool hadHost = !host.empty();
  if (host.empty()) host = c.hostName;
  const size_t s1 = line.find(' ');
  const size_t s2 = s1 == std::string::npos ? std::string::npos : line.find(' ', s1 + 1);
  if (s1 != std::string::npos) {
    const size_t te = s2 == std::string::npos ? line.size() : s2;
    std::string target = line.substr(s1 + 1, te - s1 - 1);
    if (!target.empty() && target[0] == '/') {
      line = line.substr(0, s1 + 1) + "http://" + host + target + line.substr(te);
    }
  }
  std::string out = line + "\r\n";
  const bool simple = rest.empty();
  if (!simple && !hadHost) out += "Host: " + host + "\r\n";
  out += rest;
  c.toHost.insert(c.toHost.end(), out.begin(), out.end());
  r.clear();
  if (simple || chunked) h.st = HttpRw::PASS;
  else if (clen > 0) { h.st = HttpRw::BODY; h.left = clen; }
  else h.st = HttpRw::HDR;
}

void tcpOutput(Tcp &c) {
  if (c.state != T_EST) return;
  const uint32_t now = hal_millis();
  const int mss = std::min(c.gmss, kMss);
  for (;;) {
    const uint32_t off = c.snd_nxt - c.snd_una;
    const int avail = (int)c.toGuest.size() - (int)off;
    if (avail <= 0 || qFree() < 4) break;
    int wnd = (int)(c.snd_una + c.gwin - c.snd_nxt);
    if (c.probe) { wnd = std::max(wnd, 1); c.probe = false; }
    if (wnd <= 0) {
      if (!c.rtoAt) c.rtoAt = now + c.rto;       // the persist timer
      break;
    }
    const int n = std::min(std::min(avail, mss), wnd);
    if (!tcpSend(c, ACK | (n == avail ? PSH : 0), c.snd_nxt, c.toGuest.data() + off, n)) break;
    c.snd_nxt += n;
    if (seqLt(c.snd_max, c.snd_nxt)) c.snd_max = c.snd_nxt;
    c.needAck = false;
    if (!c.rtoAt) c.rtoAt = now + c.rto;
  }
  if (c.hostEof && !c.finSent && c.snd_nxt == c.snd_una + c.toGuest.size() && qFree() >= 1) {
    tcpSend(c, FIN | ACK, c.snd_nxt, nullptr, 0);
    c.finSeq = c.snd_nxt;
    c.finSent = true;
    c.snd_nxt++;
    if (seqLt(c.snd_max, c.snd_nxt)) c.snd_max = c.snd_nxt;
    c.needAck = false;
    if (!c.rtoAt) c.rtoAt = now + c.rto;
  }
  if (c.needAck && qFree() >= 1) {
    tcpSend(c, ACK, c.snd_nxt, nullptr, 0);
    c.needAck = false;
  }
}

Tcp *tcpFind(uint32_t gip, uint16_t gport, uint32_t rip, uint16_t rport) {
  for (Tcp &c : s_tcp)
    if (c.state != T_FREE && c.gport == gport && c.rport == rport && c.rip == rip && c.gip == gip) return &c;
  return nullptr;
}

// A new connection from the guest: where it really goes.
void tcpOpen(Tcp &c) {
  const std::string *fake = fakeName(c.rip);
  c.hostName = fake ? *fake : dotted(c.rip);
#if defined(EMU_WEB)
  if (s_proxyHost.empty() || c.rport != 80) { tcpAbort(c); return; }
  c.rw = new HttpRw();
  c.s = hal_tun_open(s_proxyHost.c_str(), s_proxyPort);
  if (c.s == kNoSock) { hal_log("net: no tunnel free"); tcpAbort(c); return; }
  hal_log("net: tcp %s:80 -> %s:%d (tunnel)", c.hostName.c_str(), s_proxyHost.c_str(), s_proxyPort);
  c.state = T_CONNECTING;
  c.since = hal_millis();
  return;
#endif
  if (!s_proxyHost.empty() && c.rport == 80) {
    c.rw = new HttpRw();
    c.target = s_proxyHost;
    c.targetPort = (uint16_t)s_proxyPort;
  } else if (fake) {
    c.target = *fake;
    c.targetPort = c.rport;
  } else if (inNet(c.rip)) {
    tcpAbort(c);                                  // nothing listens on the guest's own LAN
    return;
  } else {
    c.targetPort = c.rport;
    tcpConnect(c, c.rip);
    return;
  }
  c.state = T_RESOLVING;
  c.since = hal_millis();
}

void tcpInput(uint32_t src, uint32_t dst, const uint8_t *t, int len) {
  if (len < 20) return;
  const int hl = (t[12] >> 4) * 4;
  if (hl < 20 || hl > len) return;
  const uint16_t sport = get16(t), dport = get16(t + 2);
  const uint32_t seq = get32(t + 4), ack = get32(t + 8);
  const uint8_t fl = t[13];
  const uint16_t win = get16(t + 14);
  const uint8_t *data = t + hl;
  const int dlen = len - hl;
  Tcp *cp = tcpFind(src, sport, dst, dport);
  if (!cp) {
    if (fl & RST) return;
    if ((fl & (SYN | ACK)) != SYN) { tcpRstFor(src, sport, dst, dport, seq, ack, fl, dlen); return; }
    for (Tcp &c : s_tcp) if (c.state == T_FREE) { cp = &c; break; }
    if (!cp) { tcpRstFor(src, sport, dst, dport, seq, ack, fl, dlen); return; }
    Tcp &c = *cp;
    c.gip = src; c.gport = sport; c.rip = dst; c.rport = dport;
    c.rcv_nxt = seq + 1;
    c.gwin = win;
    for (int i = 20; i + 1 < hl;) {               // the guest's MSS
      const uint8_t k = t[i];
      if (k == 0) break;
      if (k == 1) { i++; continue; }
      const int l = t[i + 1];
      if (l < 2) break;
      if (k == 2 && l == 4 && i + 4 <= hl) c.gmss = get16(t + i + 2);
      i += l;
    }
    if (c.gmss < 64) c.gmss = 536;
    c.iss = hal_micros() * 2654435761u;
    c.snd_una = c.snd_nxt = c.snd_max = c.iss;
    c.last = hal_millis();
    c.state = T_RESOLVING;                        // anything but T_FREE while tcpOpen runs
    tcpOpen(c);
    return;
  }
  Tcp &c = *cp;
  c.last = hal_millis();
  if (fl & RST) { tcpFree(c); return; }
  if (fl & SYN) {                                 // the guest sent its SYN again
    if (c.state == T_SYNACK) tcpSynAck(c);
    return;
  }
  if (!(fl & ACK)) return;
  if (c.state == T_RESOLVING || c.state == T_CONNECTING) return;
  if (c.state == T_SYNACK) {
    if (ack != c.iss + 1) return;
    c.state = T_EST;
    c.snd_una = c.snd_nxt = c.snd_max = c.iss + 1;
    c.rtoAt = 0;
    c.rto = kRto0;
  }
  if (seqLt(c.snd_una, ack) && seqLe(ack, c.snd_max)) {
    uint32_t n = ack - c.snd_una;
    if (c.finSent && ack == c.finSeq + 1) { c.finAcked = true; n--; }
    n = std::min<uint32_t>(n, (uint32_t)c.toGuest.size());
    c.toGuest.erase(c.toGuest.begin(), c.toGuest.begin() + n);
    c.snd_una = ack;
    if (seqLt(c.snd_nxt, ack)) c.snd_nxt = ack;
    c.rto = kRto0;
    c.retries = 0;
    c.rtoAt = c.snd_una == c.snd_max ? 0 : hal_millis() + c.rto;
  }
  c.gwin = win;
  if ((dlen > 0 || (fl & FIN)) && !c.guestFin) {
    if (seqLt(c.rcv_nxt, seq)) {
      c.needAck = true;                           // a gap: the guest will send it again
    } else {
      const uint32_t off = c.rcv_nxt - seq;
      int taken = 0;
      if ((int)off < dlen) {
        const size_t room = c.toHost.size() >= kToHostCap ? 0 : kToHostCap - c.toHost.size();
        taken = (int)std::min<size_t>((size_t)(dlen - (int)off), room);
        if (taken) tcpToHost(c, data + off, taken);
        c.rcv_nxt += taken;
      }
      if ((fl & FIN) && (int)off + taken >= dlen) { c.rcv_nxt++; c.guestFin = true; }
      c.needAck = true;
      s_txBytes += taken;
    }
  }
  tcpOutput(c);
}

// The sockets' side of the TCP connections, after select().
void tcpService(Tcp &c, fd_set *rd, fd_set *wr, fd_set *ex, uint32_t now) {
  if (c.state == T_RESOLVING) {
    uint32_t ip;
    const int r = resolve(c.target, &ip);
    if (r > 0) tcpConnect(c, ip);
    else if (r < 0 || now - c.since > 15000) {
      hal_log("net: cannot resolve %s", c.target.c_str());
      tcpAbort(c);
    }
    return;
  }
  if (c.state == T_CONNECTING) {
#if defined(EMU_WEB)
    const int st = hal_tun_state(c.s);
    if (st < 0) { hal_log("net: tunnel to %s failed", s_proxyHost.c_str()); tcpAbort(c); return; }
    if (st > 0) {
      c.state = T_SYNACK;
      c.rto = kRto0;
      c.retries = 0;
      tcpSynAck(c);
    } else if (now - c.since > 20000) {
      tcpAbort(c);
    }
    return;
#endif
    if (FD_ISSET(c.s, wr) || FD_ISSET(c.s, ex)) {
      int err = 0;
      socklen_t l = sizeof err;
      getsockopt(c.s, SOL_SOCKET, SO_ERROR, (char *)&err, &l);
      if (err || FD_ISSET(c.s, ex)) { hal_log("net: connect error %d", err); tcpAbort(c); return; }
      c.state = T_SYNACK;
      c.rto = kRto0;
      c.retries = 0;
      tcpSynAck(c);
    } else if (now - c.since > 20000) {
      tcpAbort(c);
    }
    return;
  }
  if (c.state == T_SYNACK) {
    if (c.rtoAt && seqLe(c.rtoAt, now)) {
      if (++c.retries > 6) { hal_log("net: no answer to SYN-ACK"); tcpAbort(c); return; }
      c.rto = std::min<uint32_t>(c.rto * 2, 16000);
      tcpSynAck(c);
    }
    return;
  }
  // T_EST
#if defined(EMU_WEB)
  if (!c.toHost.empty()) {
    const int n = hal_tun_write(c.s, c.toHost.data(), (int)c.toHost.size());
    if (n > 0) {
      const bool wasSmall = c.smallWin;
      c.toHost.erase(c.toHost.begin(), c.toHost.begin() + n);
      if (wasSmall && tcpWindow(c) >= kMss) c.needAck = true;
    } else if (n < 0 && !c.hostEof) {
      c.hostEof = true;                           // closed before it took them: FIN the guest
    }
  }
  if (c.guestFin && c.toHost.empty() && !c.shutWr) {
    hal_tun_shut(c.s);
    c.shutWr = true;
  }
  if (!c.hostEof && c.toGuest.size() < kToGuestCap) {
    uint8_t buf[4096];
    const int want = (int)std::min<size_t>(sizeof buf, kToGuestCap - c.toGuest.size());
    const int n = hal_tun_read(c.s, buf, want);
    if (n > 0) {
      c.toGuest.insert(c.toGuest.end(), buf, buf + n);
      s_rxBytes += n;
      c.last = now;
    } else if (n < 0) {
      c.hostEof = true;
    }
  }
#else
  if (!c.toHost.empty() && FD_ISSET(c.s, wr)) {
    const int n = send(c.s, (const char *)c.toHost.data(), (int)c.toHost.size(), MSG_NOSIGNAL);
    if (n > 0) {
      const bool wasSmall = c.smallWin;
      c.toHost.erase(c.toHost.begin(), c.toHost.begin() + n);
      if (wasSmall && tcpWindow(c) >= kMss) c.needAck = true;   // tell the guest it can go on
    } else if (n < 0 && !wouldBlock()) {
      tcpAbort(c);
      return;
    }
  }
  if (c.guestFin && c.toHost.empty() && !c.shutWr) {
    shutdown(c.s, SHUT_WR);
    c.shutWr = true;
  }
  if (!c.hostEof && c.toGuest.size() < kToGuestCap && FD_ISSET(c.s, rd)) {
    uint8_t buf[4096];
    const int want = (int)std::min<size_t>(sizeof buf, kToGuestCap - c.toGuest.size());
    const int n = recv(c.s, (char *)buf, want, 0);
    if (n > 0) {
      c.toGuest.insert(c.toGuest.end(), buf, buf + n);
      s_rxBytes += n;
      c.last = now;
    } else if (n == 0) {
      c.hostEof = true;
    } else if (!wouldBlock()) {
      c.hostEof = true;                           // a reset: send what came, then FIN
    }
  }
#endif
  // the retransmission and persist timer
  if (c.rtoAt && seqLe(c.rtoAt, now)) {
    if (c.snd_una != c.snd_max) {
      if (++c.retries > 10) { tcpAbort(c); return; }
      c.snd_nxt = c.snd_una;                      // go back N
      if (c.finSent && !c.finAcked) c.finSent = false;
    } else if (c.gwin == 0) {
      c.probe = true;
    }
    c.rto = std::min<uint32_t>(c.rto * 2, 16000);
    c.rtoAt = 0;
    tcpOutput(c);
    if (!c.rtoAt) c.rtoAt = now + c.rto;
    if (c.snd_una == c.snd_max && c.gwin) c.rtoAt = 0;
  }
  tcpOutput(c);
  // done, both ways; or the guest went quiet half-way through closing
  if (c.finAcked && c.guestFin && c.toHost.empty()) tcpFree(c);
  else if ((c.finAcked || c.guestFin) && now - c.last > 60000) tcpFree(c);
}

// ---- UDP ------------------------------------------------------------------------------------
struct Udp {
  bool used = false;
  uint32_t gip = 0;
  uint16_t gport = 0;
  sock_t s = kNoSock;
  uint32_t last = 0;
  uint32_t fakeIp = 0, realIp = 0;              // replies from realIp come from fakeIp
};
Udp s_udp[kMaxUdp];

void udpFree(Udp &u) {
  if (u.s != kNoSock) sclose(u.s);
  u = Udp();
}

struct DnsWait { uint32_t gip; uint16_t gport; std::string name; std::vector<uint8_t> q; uint32_t at; };
std::vector<DnsWait> s_dnsWait;

// Answers a query (its header and question, qlen bytes) with ip, or none and rcode.
void dnsReply(uint32_t gip, uint16_t gport, const uint8_t *q, int qlen, uint32_t ip, int rcode) {
  uint8_t b[512 + 16];
  if (qlen > 512) return;
  memcpy(b, q, qlen);
  put16(b + 2, (uint16_t)(0x8000 | (get16(q + 2) & 0x0100) | 0x0080 | rcode));   // QR, RD as asked, RA
  put16(b + 6, ip ? 1 : 0);
  put16(b + 8, 0); put16(b + 10, 0);
  int n = qlen;
  if (ip) {
    uint8_t *a = b + n;
    put16(a, 0xc00c); put16(a + 2, 1); put16(a + 4, 1); put32(a + 6, 60); put16(a + 10, 4); put32(a + 12, ip);
    n += 16;
  }
  sendUdp(kDnsIp, 53, gip, gport, b, n);
}

void dnsQuery(uint32_t gip, uint16_t gport, const uint8_t *q, int len) {
  if (len < 17 || get16(q + 4) != 1 || (q[2] & 0x80)) return;
  std::string name;
  int i = 12;
  while (i < len && q[i]) {
    const int l = q[i];
    if (l > 63 || i + 1 + l > len) return;
    if (!name.empty()) name += '.';
    for (int k = 0; k < l; k++) name += (char)tolower(q[i + 1 + k]);
    i += 1 + l;
  }
  if (i + 5 > len) return;
  const uint16_t qtype = get16(q + i + 1), qclass = get16(q + i + 3);
  const int qlen = i + 5;
  if (qtype != 1 || qclass != 1 || name.empty()) { dnsReply(gip, gport, q, qlen, 0, 0); return; }
  if (!s_proxyHost.empty()) { dnsReply(gip, gport, q, qlen, fakeFor(name), 0); return; }
  uint32_t ip;
  const int r = resolve(name, &ip);
  if (r > 0) dnsReply(gip, gport, q, qlen, ip, 0);
  else if (r < 0) dnsReply(gip, gport, q, qlen, 0, 3);
  else if (s_dnsWait.size() < 16) s_dnsWait.push_back({ gip, gport, name, std::vector<uint8_t>(q, q + qlen), hal_millis() });
}

void dhcpInput(const uint8_t *p, int len) {
  if (len < 240 || p[0] != 1 || get32(p + 236) != 0x63825363) return;
  int type = 0, nameAt = -1;
  for (int i = 240; i < len && p[i] != 255;) {
    if (p[i] == 0) { i++; continue; }
    if (i + 1 >= len) break;
    if (p[i] == 53 && p[i + 1] >= 1 && i + 2 < len) type = p[i + 2];
    if (p[i] == 12 && p[i + 1] >= 1 && p[i + 1] <= 32 && i + 2 + p[i + 1] <= len) nameAt = i;
    i += 2 + p[i + 1];
  }
  const int reply = type == 1 ? 2 : type == 3 || type == 8 ? 5 : 0;   // DISCOVER: OFFER; REQUEST/INFORM: ACK
  if (!reply) return;
  uint8_t b[340];
  memset(b, 0, sizeof b);
  b[0] = 2; b[1] = 1; b[2] = 6;
  memcpy(b + 4, p + 4, 4);                        // xid
  memcpy(b + 10, p + 10, 2);                      // flags
  if (type == 8) memcpy(b + 12, p + 12, 4);       // ciaddr
  else put32(b + 16, kGuestIp);                   // yiaddr
  put32(b + 20, kGwIp);
  memcpy(b + 28, p + 28, 16);                     // chaddr
  put32(b + 236, 0x63825363);
  uint8_t *o = b + 240;
  *o++ = 53; *o++ = 1; *o++ = (uint8_t)reply;
  *o++ = 54; *o++ = 4; put32(o, kGwIp); o += 4;
  if (type != 8) { *o++ = 51; *o++ = 4; put32(o, 86400); o += 4; }
  *o++ = 1; *o++ = 4; put32(o, kMask); o += 4;
  *o++ = 3; *o++ = 4; put32(o, kGwIp); o += 4;
  *o++ = 6; *o++ = 4; put32(o, kDnsIp); o += 4;
  if (nameAt >= 0) {                              // the name it asked for (mTCP warns without it)
    memcpy(o, p + nameAt, 2 + p[nameAt + 1]);
    o += 2 + p[nameAt + 1];
  }
  *o++ = 255;
  sendUdp(kGwIp, 67, 0xFFFFFFFF, 68, b, sizeof b, kBcastMac);
}

void udpInput(uint32_t src, uint32_t dst, const uint8_t *u, int len) {
  if (len < 8) return;
  const uint16_t sport = get16(u), dport = get16(u + 2);
  int ulen = get16(u + 4);
  if (ulen < 8 || ulen > len) ulen = len;
  const uint8_t *d = u + 8;
  const int n = ulen - 8;
  if (dport == 67) { dhcpInput(d, n); return; }
  if (dst == kDnsIp && dport == 53) { dnsQuery(src, sport, d, n); return; }
  if (dst == 0xFFFFFFFF || inNet(dst)) return;    // NetBIOS and other broadcasts stay on the LAN
#if defined(EMU_WEB)
  return;                                         // the page can't send UDP
#endif
  uint32_t real = dst;
  if (const std::string *name = fakeName(dst)) {
    if (resolve(*name, &real) <= 0) return;       // the guest asks again
  }
  Udp *up = nullptr;
  for (Udp &x : s_udp) if (x.used && x.gport == sport && x.gip == src) { up = &x; break; }
  if (!up) {
    for (Udp &x : s_udp) if (!x.used) { up = &x; break; }
    if (!up) {                                    // reuse the quietest
      up = &s_udp[0];
      for (Udp &x : s_udp) if (x.last - up->last > 0x80000000u) up = &x;
      udpFree(*up);
    }
    netInit();
    up->s = socket(AF_INET, SOCK_DGRAM, 0);
    if (up->s == kNoSock) { *up = Udp(); return; }
    setNonblock(up->s);
    up->used = true;
    up->gip = src;
    up->gport = sport;
  }
  up->last = hal_millis();
  up->fakeIp = dst != real ? dst : 0;
  up->realIp = real;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(dport);
  a.sin_addr.s_addr = htonl(real);
  sendto(up->s, (const char *)d, n, 0, (struct sockaddr *)&a, sizeof a);
  s_txBytes += n;
}

void udpService(Udp &u, fd_set *rd, uint32_t now) {
  if (FD_ISSET(u.s, rd) && qFree() >= 2) {
    uint8_t buf[1472];
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    const int n = recvfrom(u.s, (char *)buf, sizeof buf, 0, (struct sockaddr *)&a, &al);
    if (n > 0) {
      uint32_t from = ntohl(a.sin_addr.s_addr);
      if (u.fakeIp && from == u.realIp) from = u.fakeIp;
      sendUdp(from, ntohs(a.sin_port), u.gip, u.gport, buf, n);
      s_rxBytes += n;
      u.last = now;
    }
  }
  if (now - u.last > 60000) udpFree(u);
}

// ---- ARP, ICMP, IPv4 ------------------------------------------------------------------------
void arpInput(const uint8_t *p, int len) {
  if (len < 28 || get16(p) != 1 || get16(p + 2) != 0x0800 || get16(p + 6) != 1) return;
  const uint32_t tpa = get32(p + 24);
  if (tpa != kGwIp && tpa != kDnsIp) return;
  if (!qFree()) return;
  Frame &f = s_q[(s_qHead + s_qCount) % kQueue];
  uint8_t *e = f.d, *a = f.d + 14;
  memcpy(e, p + 8, 6);
  memcpy(e + 6, kGwMac, 6);
  put16(e + 12, 0x0806);
  put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, 2);
  memcpy(a + 8, kGwMac, 6);
  put32(a + 14, tpa);
  memcpy(a + 18, p + 8, 10);                      // their MAC and IP
  f.len = 42;
  s_qCount++;
}

void icmpInput(uint32_t src, uint32_t dst, const uint8_t *p, int len) {
  if (len < 8 || p[0] != 8 || (dst != kGwIp && dst != kDnsIp)) return;
  uint8_t *o = beginIp(len);
  if (!o) return;
  memcpy(o, p, len);
  o[0] = 0;
  endIp(dst, src, 1, len);
}

void ipInput(const uint8_t *p, int len) {
  if (len < 20 || (p[0] >> 4) != 4) return;
  const int ihl = (p[0] & 15) * 4;
  int tot = get16(p + 2);
  if (ihl < 20 || tot < ihl || tot > len) return;
  if (get16(p + 6) & 0x3fff) return;              // fragments: never needed by the guests we run
  const uint32_t src = get32(p + 12), dst = get32(p + 16);
  const uint8_t *l4 = p + ihl;
  const int l4len = tot - ihl;
  switch (p[9]) {
    case 1: icmpInput(src, dst, l4, l4len); break;
    case 6: tcpInput(src, dst, l4, l4len); break;
    case 17: udpInput(src, dst, l4, l4len); break;
  }
}

void service() {
  const uint32_t now = hal_millis();
  fd_set rd, wr, ex;
  FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
  int maxfd = -1, count = 0;
  auto add = [&](sock_t s, fd_set *set) {
    FD_SET(s, set);
    if ((int)s > maxfd) maxfd = (int)s;
    count++;
  };
#if !defined(EMU_WEB)                             // a tunnel's id is no fd: polled in tcpService
  for (Tcp &c : s_tcp) {
    if (c.s == kNoSock) continue;
    if (c.state == T_CONNECTING) { add(c.s, &wr); add(c.s, &ex); }
    else if (c.state == T_EST) {
      if (!c.hostEof && c.toGuest.size() < kToGuestCap) add(c.s, &rd);
      if (!c.toHost.empty()) add(c.s, &wr);
    }
  }
  for (Udp &u : s_udp) if (u.used) add(u.s, &rd);
#endif
  if (count) {
    struct timeval tv = { 0, 0 };
    if (select(maxfd + 1, &rd, &wr, &ex, &tv) < 0) { FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex); }
  }
  for (Tcp &c : s_tcp) if (c.state != T_FREE) tcpService(c, &rd, &wr, &ex, now);
  for (Udp &u : s_udp) if (u.used) udpService(u, &rd, now);
  for (size_t i = 0; i < s_dnsWait.size();) {
    DnsWait &w = s_dnsWait[i];
    uint32_t ip = 0;
    const int r = resolve(w.name, &ip);
    if (r == 0 && now - w.at < 10000) { i++; continue; }
    dnsReply(w.gip, w.gport, w.q.data(), (int)w.q.size(), r > 0 ? ip : 0, r > 0 ? 0 : r < 0 ? 3 : 2);
    s_dnsWait.erase(s_dnsWait.begin() + i);
  }
}

} // namespace

// ---- the C API ------------------------------------------------------------------------------
extern "C" {

int pcnet_enabled(void) { return s_enabled; }

void pcnet_guest_mac(uint8_t mac[6]) { memcpy(mac, kGuestMac, 6); }

void pcnet_configure(int enabled, const char *proxy_host, int proxy_port) {
  s_enabled = enabled != 0;
  s_proxyHost = proxy_host ? proxy_host : "";
  s_proxyPort = proxy_port > 0 && proxy_port < 65536 ? proxy_port : 80;
  if (s_enabled) resolverStart();
}

void pcnet_reset(void) {
  for (Tcp &c : s_tcp) if (c.state != T_FREE) tcpFree(c);
  for (Udp &u : s_udp) if (u.used) udpFree(u);
  s_dnsWait.clear();
  s_qHead = s_qCount = 0;
}

void pcnet_guest_frame(const uint8_t *buf, int len) {
  if (len < 14) return;
  if (!s_q) {
    s_q = (Frame *)malloc(sizeof(Frame) * kQueue);
    if (!s_q) return;
  }
  if (memcmp(buf, kGwMac, 6) && memcmp(buf, kBcastMac, 6)) return;
  memcpy(s_gmac, buf + 6, 6);
  const uint16_t type = get16(buf + 12);
  if (type == 0x0806) arpInput(buf + 14, len - 14);
  else if (type == 0x0800) ipInput(buf + 14, len - 14);
}

void pcnet_poll(void *nic, int (*can_rx)(void *), void (*rx)(void *, const uint8_t *, int)) {
  if (!s_q) return;                               // the guest has sent nothing yet
  while (s_qCount && can_rx(nic)) {
    const Frame &f = s_q[s_qHead];
    rx(nic, f.d, f.len);
    s_qHead = (s_qHead + 1) % kQueue;
    s_qCount--;
  }
  const uint32_t now = hal_millis();
  if (now - s_lastService < kServiceMs) return;
  s_lastService = now;
  service();
}

void pcnet_status(char *buf, size_t n) {
  int tcp = 0;
  for (Tcp &c : s_tcp) tcp += c.state != T_FREE;
  snprintf(buf, n, "%d conn, in %lu KB, out %lu KB", tcp, (unsigned long)(s_rxBytes >> 10),
           (unsigned long)(s_txBytes >> 10));
}

} // extern "C"

#endif
