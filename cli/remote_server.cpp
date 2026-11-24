// SPDX-License-Identifier: BSD-3-Clause
// insiliscope_server: the viewer's engine on a remote Linux machine (docs/remote.md).
//
// The viewer's engine workers (web/index.html iscEngine) send their heavy jobs here instead of running them in the
// browser: live frames, movies and Records, ground truth, setups and the Microscope view's panels. This is the same C
// ABI as the viewer's WebAssembly module (scope_wasm.cpp, built natively), with the job glue of iscEngine.handle in
// C++: the replies are the messages the browser's own engine would post, so the page cannot tell them apart.
//
// Messages (both ways; web/live/remote_codec.js is the other end): u32 LE n, n bytes of JSON, then binary blobs, each
// starting on a multiple of 8 bytes from the message's start. The JSON's "$blobs" lists the blobs' sizes; a typed
// array is {"$b": index, "$t": "Uint16Array"}. Transports:
//   --stdio            each message framed by a u32 LE size on stdin / stdout (Micro-Manager, scripts)
//   --socket PATH      a Unix socket (0600, in a 0700 directory) taking WebSocket connections (ssh -L to it)
//   --listen HOST:PORT the same on TCP (a token is required)
// Each WebSocket connection is a process of its own (fork), as each browser worker is an engine of its own; closing
// the connection (a worker terminated: a Record cancelled) ends it at once.
#include "json_lite.h"
#include "module_hooks.h"

#include "insiliscope/insiliscope.h"
#include "insiliscope/version.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ISC_SERVER_COMMIT
#define ISC_SERVER_COMMIT ""
#endif

// The protocol's version: a page and a server that differ here cannot talk (the page says so and stays local).
static const int kProtocol = 1;

extern "C" {
int32_t isc_scope_options(char* out, int32_t cap);
int32_t isc_bfp_map_file(const char* name, const uint8_t* data, int32_t n);
int32_t isc_scope_movie(const char* spec, uint16_t* out, int32_t capPixels, int32_t* info, char* errOut, int32_t errCap);
void isc_movie_stream(int32_t on);
void isc_movie_strips(int32_t rows);
int32_t isc_scope_setup(const char* spec, char* out, int32_t cap, char* errOut, int32_t errCap);
int32_t isc_scope_panels(char* out, int32_t cap);
int32_t isc_scope_panel(const char* spec, const char* id, const char* params, int32_t scale, char* header, int32_t hcap,
                        float* out, int32_t fcap, int32_t* sizes, char* errOut, int32_t errCap);
void isc_set_threads(int32_t n);
int32_t isc_live_frames(const char* spec, int32_t n, int32_t countLight, double* info, char* errOut, int32_t errCap);
const uint16_t* isc_live_image();
const char* isc_live_paths();
void isc_live_reset();
int32_t isc_live_advance(const char* spec, int32_t n, int32_t countLight, char* errOut, int32_t errCap);
double isc_live_time();
void isc_live_profile(int32_t on);
const char* isc_live_profile_json();
const char* isc_memory_json();
int32_t isc_scope_gt_run(const char* settingsJson, double runMin, int32_t dyeCsv);
double isc_scope_gt_files(double minCore, double volLateralNm, double volAxialNm, int32_t dyeCsv, int32_t keep);
double isc_scope_gt(const char* settingsJson, double minCore, double volLateralNm, double volAxialNm, int32_t dyeCsv);
const char* isc_scope_gt_error();
}

namespace {

using jsonlite::Json;
using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0)
{
   return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

bool g_quiet = false;
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...)
{
   if (g_quiet) return;
   va_list ap;
   va_start(ap, fmt);
   std::vfprintf(stderr, fmt, ap);
   va_end(ap);
   std::fputc('\n', stderr);
   std::fflush(stderr);
}

// ---- JSON out ----
std::string JStr(const std::string& s)
{
   std::string o = "\"";
   for (unsigned char c : s)
   {
      switch (c)
      {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
         if (c < 0x20)
         {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
         }
         else
            o += static_cast<char>(c);
      }
   }
   return o + "\"";
}

std::string JNum(double v)
{
   if (!std::isfinite(v)) return "null";
   if (v == std::floor(v) && std::fabs(v) < 9e15)
   {
      char b[32];
      std::snprintf(b, sizeof b, "%.0f", v);
      return b;
   }
   char b[32];
   std::snprintf(b, sizeof b, "%.17g", v);
   return b;
}

std::string Serialize(const Json& v)
{
   switch (v.type)
   {
   case Json::Null: return "null";
   case Json::Bool: return v.b ? "true" : "false";
   case Json::Number: return JNum(v.num);
   case Json::String: return JStr(v.str);
   case Json::Array:
   {
      std::string o = "[";
      for (size_t i = 0; i < v.arr.size(); ++i) o += (i ? "," : "") + Serialize(v.arr[i]);
      return o + "]";
   }
   case Json::Object:
   {
      std::string o = "{";
      for (size_t i = 0; i < v.obj.size(); ++i) o += (i ? "," : "") + JStr(v.obj[i].first) + ":" + Serialize(v.obj[i].second);
      return o + "}";
   }
   }
   return "null";
}

// A JSON text from the engine (setup, panel header, paths, profile) as it is when it parses, else null (a reply never
// carries a broken document).
std::string RawOrNull(const char* text)
{
   if (!text || !*text) return "null";
   const std::string s(text);
   jsonlite::Json v;
   jsonlite::Parser p{ s };
   if (!p.Value(v, 0)) return "null";
   return s;
}

// ---- messages ----
struct InMsg
{
   std::string payload;
   Json j;
   std::vector<std::pair<size_t, size_t>> blobs;   // offset, size in payload

   const Json* Get(const char* k) const { return j.Get(k); }
   std::string Str(const char* k, const std::string& def = "") const
   {
      const Json* v = Get(k);
      return v && v->type == Json::String ? v->str : def;
   }
   double Num(const char* k, double def = 0) const
   {
      const Json* v = Get(k);
      if (!v) return def;
      if (v->type == Json::Number) return v->num;
      if (v->type == Json::Bool) return v->b ? 1 : 0;
      return def;
   }
   bool Truthy(const char* k) const
   {
      const Json* v = Get(k);
      if (!v) return false;
      if (v->type == Json::Bool) return v->b;
      if (v->type == Json::Number) return v->num != 0;
      if (v->type == Json::String) return !v->str.empty();
      return v->type == Json::Array || v->type == Json::Object;
   }
   // A typed array's bytes ({"$b": i}).
   bool Blob(const Json* v, const uint8_t*& p, size_t& n) const
   {
      if (!v || v->type != Json::Object) return false;
      const Json* b = v->Get("$b");
      if (!b || b->type != Json::Number) return false;
      const size_t i = static_cast<size_t>(b->num);
      if (i >= blobs.size()) return false;
      p = reinterpret_cast<const uint8_t*>(payload.data()) + blobs[i].first;
      n = blobs[i].second;
      return true;
   }
};

size_t Align8(size_t n) { return (n + 7) & ~size_t(7); }

uint32_t Le32(const char* p)
{
   const unsigned char* u = reinterpret_cast<const unsigned char*>(p);
   return u[0] | (u[1] << 8) | (u[2] << 16) | (static_cast<uint32_t>(u[3]) << 24);
}

bool Decode(std::string&& payload, InMsg& m, std::string& err)
{
   m.payload = std::move(payload);
   const std::string& s = m.payload;
   if (s.size() < 4) { err = "a message too short"; return false; }
   const size_t jl = Le32(s.data());
   if (4 + jl > s.size()) { err = "a message's JSON past its end"; return false; }
   const std::string text = s.substr(4, jl);
   jsonlite::Parser p{ text };
   if (!p.Value(m.j, 0) || m.j.type != Json::Object) { err = "a message's JSON: " + (p.err.empty() ? std::string("not an object") : p.err); return false; }
   size_t at = Align8(4 + jl);
   if (const Json* b = m.j.Get("$blobs"))
      for (const Json& n : b->arr)
      {
         const size_t len = static_cast<size_t>(n.num);
         if (at + len > s.size()) { err = "a message's blob past its end"; return false; }
         m.blobs.emplace_back(at, len);
         at += Align8(len);
      }
   return true;
}

// A reply: its JSON fields in order and its blobs (pointers that stay valid until it is sent; Keep() for data that
// does not).
struct OutMsg
{
   std::string body;
   std::vector<std::pair<const void*, size_t>> blobs;
   std::vector<std::string> kept;

   OutMsg(const std::string& type, const std::string& idJson)
   {
      body = "\"type\":" + JStr(type);
      if (!idJson.empty()) body += ",\"id\":" + idJson;
   }
   OutMsg& Raw(const char* k, const std::string& json) { body += ","; body += JStr(k); body += ":"; body += json; return *this; }
   OutMsg& Num(const char* k, double v) { return Raw(k, JNum(v)); }
   OutMsg& Str(const char* k, const std::string& v) { return Raw(k, JStr(v)); }
   OutMsg& Bool(const char* k, bool v) { return Raw(k, v ? "true" : "false"); }
   std::string Ref(const void* p, size_t n, const char* type)
   {
      blobs.emplace_back(p, n);
      return "{\"$b\":" + std::to_string(blobs.size() - 1) + ",\"$t\":\"" + type + "\"}";
   }
   OutMsg& Blob(const char* k, const void* p, size_t n, const char* type) { return Raw(k, Ref(p, n, type)); }
   std::string Header() const
   {
      std::string h = "{\"$blobs\":[";
      for (size_t i = 0; i < blobs.size(); ++i) h += (i ? "," : "") + std::to_string(blobs[i].second);
      return h + "]," + body + "}";
   }
};

// ---- transports ----
bool WriteAll(int fd, const void* p, size_t n)
{
   const char* c = static_cast<const char*>(p);
   while (n)
   {
      const ssize_t k = ::write(fd, c, std::min(n, size_t(1) << 30));
      if (k < 0)
      {
         if (errno == EINTR) continue;
         return false;
      }
      c += k;
      n -= static_cast<size_t>(k);
   }
   return true;
}

bool ReadAll(int fd, void* p, size_t n)
{
   char* c = static_cast<char*>(p);
   while (n)
   {
      const ssize_t k = ::read(fd, c, std::min(n, size_t(1) << 30));
      if (k == 0) return false;
      if (k < 0)
      {
         if (errno == EINTR) continue;
         return false;
      }
      c += k;
      n -= static_cast<size_t>(k);
   }
   return true;
}

const size_t kMaxIn = size_t(2) << 30;   // a message from a client (a BFP image, a settings file) is far below this

class Conn
{
public:
   virtual ~Conn() = default;
   // The next message's payload; false when the other end is gone.
   virtual bool Read(std::string& payload) = 0;
   void Send(const OutMsg& m)
   {
      const std::string h = m.Header();
      uint64_t total = 4 + h.size();
      total = Align8(total);
      for (const auto& b : m.blobs) total = Align8(total + b.second);
      std::lock_guard<std::mutex> lk(mu_);
      bool ok = Frame(total);
      const uint32_t jl = static_cast<uint32_t>(h.size());
      const unsigned char le[4] = { (unsigned char)jl, (unsigned char)(jl >> 8), (unsigned char)(jl >> 16), (unsigned char)(jl >> 24) };
      static const char zeros[8] = {};
      uint64_t at = 4 + h.size();
      ok = ok && WriteAll(wfd_, le, 4) && WriteAll(wfd_, h.data(), h.size()) && WriteAll(wfd_, zeros, Align8(at) - at);
      at = Align8(at);
      for (const auto& b : m.blobs)
      {
         ok = ok && WriteAll(wfd_, b.first, b.second);
         at += b.second;
         ok = ok && WriteAll(wfd_, zeros, Align8(at) - at);
         at = Align8(at);
      }
      if (!ok) _exit(0);   // the other end is gone: so is this engine
   }

protected:
   virtual bool Frame(uint64_t size) = 0;   // the transport's frame header for one message of `size` bytes
   int wfd_ = -1;
   std::mutex mu_;
};

class StdioConn : public Conn
{
public:
   StdioConn() { wfd_ = 1; }
   bool Read(std::string& payload) override
   {
      unsigned char le[4];
      if (!ReadAll(0, le, 4)) return false;
      const uint32_t n = le[0] | (le[1] << 8) | (le[2] << 16) | (static_cast<uint32_t>(le[3]) << 24);
      if (n > kMaxIn) return false;
      payload.resize(n);
      return ReadAll(0, &payload[0], n);
   }

protected:
   bool Frame(uint64_t size) override
   {
      if (size > 0xffffffffull) return false;
      const unsigned char le[4] = { (unsigned char)size, (unsigned char)(size >> 8), (unsigned char)(size >> 16), (unsigned char)(size >> 24) };
      return WriteAll(wfd_, le, 4);
   }
};

class WsConn : public Conn
{
public:
   explicit WsConn(int fd) : fd_(fd) { wfd_ = fd; }
   bool Read(std::string& payload) override
   {
      payload.clear();
      for (;;)
      {
         unsigned char h[2];
         if (!ReadAll(fd_, h, 2)) return false;
         const bool fin = h[0] & 0x80;
         const int op = h[0] & 0x0f;
         const bool masked = h[1] & 0x80;
         uint64_t len = h[1] & 0x7f;
         if (len == 126)
         {
            unsigned char e[2];
            if (!ReadAll(fd_, e, 2)) return false;
            len = (e[0] << 8) | e[1];
         }
         else if (len == 127)
         {
            unsigned char e[8];
            if (!ReadAll(fd_, e, 8)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
         }
         unsigned char mask[4] = {};
         if (masked && !ReadAll(fd_, mask, 4)) return false;
         if (len > kMaxIn || payload.size() + len > kMaxIn) return false;
         std::string part(static_cast<size_t>(len), '\0');
         if (len && !ReadAll(fd_, &part[0], static_cast<size_t>(len))) return false;
         if (masked)
            for (size_t i = 0; i < part.size(); ++i) part[i] = static_cast<char>(part[i] ^ mask[i & 3]);
         if (op == 8) return false;   // close
         if (op == 9)                 // ping: pong
         {
            std::lock_guard<std::mutex> lk(mu_);
            unsigned char ph[2] = { 0x8a, static_cast<unsigned char>(std::min<size_t>(part.size(), 125)) };
            WriteAll(fd_, ph, 2);
            WriteAll(fd_, part.data(), ph[1]);
            continue;
         }
         if (op == 10) continue;      // pong
         payload += part;
         if (fin) return true;
      }
   }

protected:
   bool Frame(uint64_t size) override
   {
      unsigned char h[10];
      size_t n = 0;
      h[n++] = 0x82;   // FIN, binary
      if (size < 126) h[n++] = static_cast<unsigned char>(size);
      else if (size < 65536) { h[n++] = 126; h[n++] = (unsigned char)(size >> 8); h[n++] = (unsigned char)size; }
      else
      {
         h[n++] = 127;
         for (int i = 7; i >= 0; --i) h[n++] = (unsigned char)(size >> (8 * i));
      }
      return WriteAll(fd_, h, n);
   }

private:
   int fd_;
};

// ---- the WebSocket handshake (RFC 6455): SHA-1 and base64 of the key ----
struct Sha1
{
   uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
   static uint32_t Rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
   void Block(const unsigned char* p)
   {
      uint32_t w[80];
      for (int i = 0; i < 16; ++i) w[i] = (p[4 * i] << 24) | (p[4 * i + 1] << 16) | (p[4 * i + 2] << 8) | p[4 * i + 3];
      for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
      uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
      for (int i = 0; i < 80; ++i)
      {
         uint32_t f, k;
         if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
         else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
         else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
         else { f = b ^ c ^ d; k = 0xCA62C1D6; }
         const uint32_t t = Rol(a, 5) + f + e + k + w[i];
         e = d; d = c; c = Rol(b, 30); b = a; a = t;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
   }
   std::string Digest(const std::string& msg)
   {
      std::string m = msg;
      const uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
      m += static_cast<char>(0x80);
      while (m.size() % 64 != 56) m += '\0';
      for (int i = 7; i >= 0; --i) m += static_cast<char>((bits >> (8 * i)) & 0xff);
      for (size_t i = 0; i < m.size(); i += 64) Block(reinterpret_cast<const unsigned char*>(m.data()) + i);
      std::string out;
      for (uint32_t x : h)
         for (int i = 3; i >= 0; --i) out += static_cast<char>((x >> (8 * i)) & 0xff);
      return out;
   }
};

std::string Base64(const std::string& s)
{
   static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   std::string o;
   size_t i = 0;
   for (; i + 2 < s.size(); i += 3)
   {
      const uint32_t v = (uint8_t(s[i]) << 16) | (uint8_t(s[i + 1]) << 8) | uint8_t(s[i + 2]);
      o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
   }
   if (i + 1 == s.size())
   {
      const uint32_t v = uint8_t(s[i]) << 16;
      o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "==";
   }
   else if (i + 2 == s.size())
   {
      const uint32_t v = (uint8_t(s[i]) << 16) | (uint8_t(s[i + 1]) << 8);
      o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
   }
   return o;
}

// Constant-time comparison (the token).
bool SameText(const std::string& a, const std::string& b)
{
   if (a.size() != b.size()) return false;
   unsigned char d = 0;
   for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
   return d == 0;
}

std::string Lower(std::string s)
{
   for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   return s;
}

// The HTTP upgrade on fd: true when the WebSocket is open. A plain GET (curl, a health check) gets a line about this
// server; a wrong token a 403.
bool Handshake(int fd, const std::string& token)
{
   std::string req;
   char buf[2048];
   while (req.find("\r\n\r\n") == std::string::npos)
   {
      if (req.size() > 16384) return false;
      const ssize_t k = ::read(fd, buf, sizeof buf);
      if (k <= 0) return false;
      req.append(buf, static_cast<size_t>(k));
   }
   const size_t eol = req.find("\r\n");
   const std::string line = req.substr(0, eol);
   const size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
   const std::string path = sp1 == std::string::npos ? "" : line.substr(sp1 + 1, sp2 - sp1 - 1);
   std::string key, upgrade;
   size_t at = eol + 2;
   while (at < req.size())
   {
      const size_t e = req.find("\r\n", at);
      if (e == std::string::npos || e == at) break;
      const std::string h = req.substr(at, e - at);
      at = e + 2;
      const size_t c = h.find(':');
      if (c == std::string::npos) continue;
      const std::string name = Lower(h.substr(0, c));
      std::string val = h.substr(c + 1);
      while (!val.empty() && val.front() == ' ') val.erase(val.begin());
      if (name == "sec-websocket-key") key = val;
      if (name == "upgrade") upgrade = Lower(val);
   }
   std::string got;
   const size_t q = path.find("t=");
   if (q != std::string::npos && (q == 0 || path[q - 1] == '?' || path[q - 1] == '&'))
   {
      const size_t end = path.find('&', q);
      got = path.substr(q + 2, end == std::string::npos ? std::string::npos : end - q - 2);
   }
   auto reply = [&](const std::string& status, const std::string& text) {
      const std::string r = "HTTP/1.1 " + status + "\r\nContent-Type: text/plain\r\nContent-Length: " +
                            std::to_string(text.size()) + "\r\nConnection: close\r\n\r\n" + text;
      WriteAll(fd, r.data(), r.size());
   };
   if (!token.empty() && !SameText(got, token))
   {
      reply("403 Forbidden", "inSiliScope server: wrong or missing token\n");
      return false;
   }
   if (upgrade != "websocket" || key.empty())
   {
      reply("200 OK", std::string("inSiliScope server ") + ISC_VERSION + "\n");
      return false;
   }
   const std::string accept = Base64(Sha1().Digest(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
   const std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
   return WriteAll(fd, r.data(), r.size());
}

// ---- the engine: iscEngine.handle's remote job types ----
struct Options
{
   int threads = 0;   // 0: all
   std::string token;
};

class Engine
{
public:
   Engine(Conn& c, const Options& o) : c_(c), o_(o) {}

   void Handle(InMsg& m)
   {
      const std::string type = m.Str("type");
      const Json* idv = m.Get("id");
      const std::string id = idv ? Serialize(*idv) : "";
      try
      {
         if (type == "hello") return Hello(m, id);
         if (type == "bfp-file") return BfpFile(m);
         if (type == "live") return Live(m, id);
         if (type == "live-replay") return LiveReplay(m, id);
         if (type == "movie") return Movie(m, id);
         if (type == "setup") return Setup(m, id);
         if (type == "panel") return Panel(m, id);
         if (type == "options") return OptionsJob(id);
         if (type == "gt") return Gt(m, id, "gt");
         if (type == "gt-run") return GtRun(m, id);
         if (type == "gt-files") return Gt(m, id, "gt-files");
         Error(type, id, "the remote server does not take '" + type + "' jobs");
      }
      catch (const std::bad_alloc&)
      {
         Clear();
         Error(ReplyType(type), id, "out of memory on the server");
      }
      catch (const std::exception& e)
      {
         Clear();
         Error(ReplyType(type), id, std::string("server: ") + e.what());
      }
   }

private:
   Conn& c_;
   Options o_;
   bool profileOn_ = false;

   static std::string ReplyType(const std::string& t) { return t; }
   void Error(const std::string& type, const std::string& id, const std::string& e)
   {
      OutMsg o(type, id);
      o.Str("error", e);
      c_.Send(o);
   }
   static void Clear()
   {
      IscHooks() = IscModuleHooks();
      isc_movie_stream(0);
      isc_movie_strips(0);
   }

   void Hello(const InMsg& m, const std::string& id)
   {
      char host[256] = {};
      gethostname(host, sizeof host - 1);
      const long pages = sysconf(_SC_PHYS_PAGES), psize = sysconf(_SC_PAGE_SIZE);
      OutMsg o("hello", id);
      o.Num("protocol", kProtocol).Str("version", ISC_VERSION).Str("commit", ISC_SERVER_COMMIT)
         .Num("abi", isc_abi_version()).Str("world", isc_world_version()).Str("host", host)
         .Num("cpus", std::thread::hardware_concurrency()).Num("threads", o_.threads > 0 ? o_.threads : std::thread::hardware_concurrency())
         .Num("memGB", pages > 0 && psize > 0 ? static_cast<double>(pages) * psize / 1073741824.0 : 0);
      (void)m;
      c_.Send(o);
   }

   void BfpFile(const InMsg& m)
   {
      const uint8_t* p = nullptr;
      size_t n = 0;
      if (!m.Blob(m.Get("bytes"), p, n)) return;
      isc_bfp_map_file(m.Str("name").c_str(), p, static_cast<int32_t>(n));
   }

   // The movie's progress as 'movie-progress' messages (iscEngine progressTo).
   void ProgressTo(const std::string& id)
   {
      IscHooks().progress = [this, id](const char* stage, double frac, const char* detail) {
         OutMsg o("movie-progress", id);
         o.Str("stage", stage ? stage : "").Num("frac", frac).Raw("info", RawOrNull(detail));
         c_.Send(o);
      };
   }

   void Live(const InMsg& m, const std::string& id)
   {
      const int nF = std::max(1, static_cast<int>(m.Num("n", 1)));
      if (m.Truthy("reset")) isc_live_reset();
      const bool prof = m.Truthy("profile");
      if (prof != profileOn_) { profileOn_ = prof; isc_live_profile(prof ? 1 : 0); }
      const auto tw = Clock::now();
      const std::string spec = m.Str("spec");
      const Json* litv = m.Get("lit");
      const int lit = litv && litv->type == Json::Bool && !litv->b ? 0 : 1;
      double v[17] = {};
      char err[512] = {};
      const int32_t px = isc_live_frames(spec.c_str(), nF, lit, v, err, sizeof err);
      if (px < 0) return Error("live", id, err);
      double exposure = 50;
      for (size_t at = 0; (at = spec.find("exposure-ms=", at)) != std::string::npos; at += 12)
         if (at == 0 || spec[at - 1] == ' ')
         {
            exposure = std::strtod(spec.c_str() + at + 12, nullptr);
            break;
         }
      OutMsg o("live", id);
      o.Str("spec", spec).Blob("frames", isc_live_image(), static_cast<size_t>(px) * 2, "Uint16Array")
         .Bool("disk", false).Str("diskError", "").Num("w", v[0]).Num("h", v[1]).Num("n", nF)
         .Num("t", v[2] + nF * (exposure / 1000)).Num("tStart", v[2]).Num("clock", v[3]).Bool("lit", v[4] != 0)
         .Bool("epi", v[5] != 0).Bool("trans", v[6] != 0).Num("blinks", v[7]).Num("dyes", v[8])
         .Raw("drift", "[" + JNum(v[9]) + "," + JNum(v[10]) + "," + JNum(v[11]) + "]").Num("renderMs", v[12])
         .Str("engine", "live").Num("frame", v[13]);
      if (v[16] > 0)
         o.Raw("pattern", "{\"frame\":" + JNum(v[13]) + ",\"angle\":" + JNum(v[14]) + ",\"phase\":" + JNum(v[15]) +
                          ",\"period\":" + JNum(v[16]) + "}");
      else
         o.Raw("pattern", "null");
      o.Raw("paths", RawOrNull(isc_live_paths()));
      if (prof)
      {
         o.Raw("profile", RawOrNull(isc_live_profile_json()));
         o.Raw("memory", RawOrNull(isc_memory_json()));
      }
      o.Num("workerMs", MsSince(tw)).Bool("remote", true);
      c_.Send(o);
   }

   void LiveReplay(const InMsg& m, const std::string& id)
   {
      isc_live_reset();
      std::string error;
      if (const Json* recs = m.Get("records"))
         for (const Json& r : recs->arr)
         {
            const Json* sv = r.Get("spec");
            const Json* nv = r.Get("n");
            const Json* lv = r.Get("lit");
            const std::string spec = sv && sv->type == Json::String ? sv->str : "";
            const int n = std::max(1, nv && nv->type == Json::Number ? static_cast<int>(nv->num) : 1);
            const int lit = lv && lv->type == Json::Bool && !lv->b ? 0 : 1;
            char err[512] = {};
            if (isc_live_advance(spec.c_str(), n, lit, err, sizeof err) < 0) { error = err; break; }
         }
      OutMsg o("live-replay", id);
      o.Num("t", isc_live_time()).Raw("error", error.empty() ? "null" : JStr(error));
      c_.Send(o);
   }

   void Movie(const InMsg& m, const std::string& id)
   {
      const std::string spec = m.Str("spec");
      const Json* rectv = m.Get("rect");
      const std::string rect = rectv ? Serialize(*rectv) : "null";
      const bool prepare = m.Truthy("prepare"), stream = m.Truthy("stream");
      int32_t info[6] = {};
      char err[512] = {};
      const auto t0 = Clock::now();
      if (!stream)
      {
         const int32_t need = isc_scope_movie(spec.c_str(), nullptr, 0, info, err, sizeof err);
         if (need < 0) return Error("movie", id, err);
         std::vector<uint16_t> out(static_cast<size_t>(std::max(1, need)));
         if (!prepare) ProgressTo(id);
         const int32_t got = isc_scope_movie(spec.c_str(), out.data(), need, info, err, sizeof err);
         Clear();
         if (got < 0) return Error("movie", id, err);
         OutMsg o("movie", id);
         o.Str("spec", spec).Raw("rect", rect).Blob("frames", out.data(), static_cast<size_t>(need) * 2, "Uint16Array")
            .Num("w", info[0]).Num("h", info[1]).Num("n", info[2]).Num("blinks", info[3]).Num("dyes", info[4])
            .Num("halfMs", info[5]).Num("ms", MsSince(t0)).Bool("remote", true);
         if (prepare) o.Bool("prepared", true);
         c_.Send(o);
         return;
      }
      // Streamed (a Record): whole frames in parts of ~16 M samples ('movie-part'), or with rowsOut, strips' rows
      // ('movie-rows') when the movie needs strips (iscEngine streamFrames).
      struct S
      {
         std::vector<uint16_t> part;
         size_t px = 0, n = 0;
         long first = 0, frames = 0;
         unsigned y0 = 0, rows = 0;
         bool rowsMode = false;
         std::string stopped;
      } s;
      auto flush = [&]() {
         if (!s.n) return;
         OutMsg o(s.rowsMode ? "movie-rows" : "movie-part", id);
         o.Num("first", static_cast<double>(s.first)).Num("n", static_cast<double>(s.n));
         if (s.rowsMode)
         {
            o.Num("y0", s.y0).Num("rows", s.rows).Num("w", info[0]).Num("h", info[1]);
            o.Blob("frames", s.part.data(), s.n * s.rows * static_cast<size_t>(info[0]) * 2, "Uint16Array");
         }
         else
            o.Blob("frames", s.part.data(), s.n * s.px * 2, "Uint16Array");
         c_.Send(o);
         s.n = 0;
      };
      ProgressTo(id);
      const bool rowsOut = m.Truthy("rowsOut");
      if (rowsOut)
      {
         const double sr = m.Num("stripRows", 0);
         isc_movie_strips(sr > 0 ? static_cast<int32_t>(sr) : -1);
         IscHooks().movieRows = [&](const uint16_t* px, size_t n, long f, unsigned y0, unsigned rows) {
            const size_t w = static_cast<size_t>(info[0]), h = static_cast<size_t>(info[1]);
            const size_t k = static_cast<size_t>(rows) * w;
            s.px = w * h;
            if (s.n && (!s.rowsMode || s.y0 != y0 || s.rows != rows)) flush();
            if (!s.n)
            {
               s.rowsMode = true;
               s.first = f;
               s.y0 = y0;
               s.rows = rows;
               s.part.resize(std::max<size_t>(1, static_cast<size_t>(16e6) / std::max<size_t>(1, k)) * k);
            }
            std::memcpy(s.part.data() + s.n * k, px, std::min(n, k) * 2);
            s.n++;
            if (y0 + rows >= h) s.frames = f + 1;
            if ((s.n + 1) * k > s.part.size()) flush();
            return true;
         };
      }
      IscHooks().movieFrame = [&](const uint16_t* px, size_t n, long f) {
         if (s.rowsMode && s.n) flush();
         s.rowsMode = false;
         if (!s.n)
         {
            s.px = n;
            s.first = f;
            s.part.resize(std::max<size_t>(1, static_cast<size_t>(16e6) / std::max<size_t>(1, n)) * n);
         }
         std::memcpy(s.part.data() + s.n * s.px, px, n * 2);
         s.n++;
         s.frames = f + 1;
         if ((s.n + 1) * s.px > s.part.size()) flush();
         return true;
      };
      isc_movie_stream(1);
      const int32_t got = isc_scope_movie(spec.c_str(), nullptr, 0, info, err, sizeof err);
      flush();
      Clear();
      const std::string error = got < 0 ? err : "";
      if (!error.empty() && !s.frames) return Error("movie", id, error);
      OutMsg o("movie", id);
      o.Str("spec", spec).Raw("rect", rect).Bool("streamed", true).Num("got", static_cast<double>(s.frames))
         .Bool("disk", false).Raw("last", "null").Str("diskError", "").Str("stopped", !s.stopped.empty() ? s.stopped : error)
         .Num("w", info[0]).Num("h", info[1]).Num("n", info[2]).Num("blinks", info[3]).Num("dyes", info[4])
         .Num("halfMs", info[5]).Num("ms", MsSince(t0)).Bool("remote", true);
      c_.Send(o);
   }

   void Setup(const InMsg& m, const std::string& id)
   {
      const std::string spec = m.Str("spec");
      char err[512] = {};
      const int32_t need = isc_scope_setup(spec.c_str(), nullptr, 0, err, sizeof err);
      if (need < 0) return Error("setup", id, err);
      std::string b(static_cast<size_t>(need) + 1, '\0');
      isc_scope_setup(spec.c_str(), &b[0], need + 1, err, sizeof err);
      b.resize(static_cast<size_t>(need));
      OutMsg o("setup", id);
      o.Raw("setup", RawOrNull(b.c_str()));
      c_.Send(o);
   }

   void Panel(const InMsg& m, const std::string& id)
   {
      if (!m.Get("panel") || m.Str("panel").empty())
      {
         const int32_t n = isc_scope_panels(nullptr, 0);
         std::string b(static_cast<size_t>(n) + 1, '\0');
         isc_scope_panels(&b[0], n + 1);
         b.resize(static_cast<size_t>(n));
         OutMsg o("panel", id);
         o.Raw("list", RawOrNull(b.c_str()));
         c_.Send(o);
         return;
      }
      const std::string spec = m.Str("spec"), pid = m.Str("panel"), params = m.Str("params");
      const int scale = static_cast<int>(m.Num("scale", 1)) > 0 ? static_cast<int>(m.Num("scale", 1)) : 1;
      int32_t sizes[2] = {};
      char err[512] = {};
      if (isc_scope_panel(spec.c_str(), pid.c_str(), params.c_str(), scale, nullptr, 0, nullptr, 0, sizes, err, sizeof err) < 0)
         return Error("panel", id, err);
      std::string hb(static_cast<size_t>(sizes[0]) + 1, '\0');
      std::vector<float> fb(static_cast<size_t>(std::max(1, sizes[1])));
      if (isc_scope_panel(spec.c_str(), pid.c_str(), params.c_str(), scale, &hb[0], sizes[0] + 1, fb.data(), sizes[1], sizes,
                          err, sizeof err) < 0)
         return Error("panel", id, err);
      hb.resize(static_cast<size_t>(sizes[0]));
      OutMsg o("panel", id);
      o.Raw("header", RawOrNull(hb.c_str())).Blob("data", fb.data(), static_cast<size_t>(sizes[1]) * 4, "Float32Array");
      c_.Send(o);
   }

   void OptionsJob(const std::string& id)
   {
      const int32_t n = isc_scope_options(nullptr, 0);
      std::string b(static_cast<size_t>(n) + 1, '\0');
      isc_scope_options(&b[0], n + 1);
      b.resize(static_cast<size_t>(n));
      std::string names = "[", defs = "[", parts = "[", vnames = "[", kinds = "[";
      size_t at = 0;
      bool first = true;
      while (at < b.size())
      {
         size_t e = b.find('\n', at);
         if (e == std::string::npos) e = b.size();
         const std::string line = b.substr(at, e - at);
         at = e + 1;
         if (line.empty()) continue;
         std::vector<std::string> f;
         for (size_t p = 0;;)
         {
            const size_t t = line.find('\t', p);
            f.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
            if (t == std::string::npos) break;
            p = t + 1;
         }
         while (f.size() < 6) f.emplace_back();
         const std::string sep = first ? "" : ",";
         first = false;
         names += sep + JStr(f[0]);
         defs += sep + (f[5] == "text" ? JStr(f[1]) : JNum(std::strtod(f[1].c_str(), nullptr)));
         parts += sep + JStr(f[3]);
         if (f[4].empty()) vnames += sep + "null";
         else
         {
            std::string a = "[";
            size_t p = 0;
            bool f1 = true;
            for (;;)
            {
               const size_t t = f[4].find('|', p);
               a += (f1 ? "" : ",") + JStr(f[4].substr(p, t == std::string::npos ? std::string::npos : t - p));
               f1 = false;
               if (t == std::string::npos) break;
               p = t + 1;
            }
            vnames += sep + a + "]";
         }
         kinds += sep + JStr(f[5]);
      }
      OutMsg o("options", id);
      o.Raw("names", names + "]").Raw("defaults", defs + "]").Raw("parts", parts + "]").Raw("valueNames", vnames + "]")
         .Raw("kinds", kinds + "]");
      c_.Send(o);
   }

   // The ground truth's files, [[name, bytes], ...] (the proxy writes them to the page's disk storage when asked).
   using Files = std::vector<std::pair<std::string, std::string>>;
   void SinkFiles(Files& files)
   {
      IscHooks().gtFile = [&files](const char* name, const char* data, size_t size) {
         files.emplace_back(name, std::string(data, size));
         return true;
      };
   }
   void SendFiles(const char* type, const std::string& id, Files& files, const std::string& extra)
   {
      OutMsg o(type, id);
      std::string list = "[";
      for (size_t i = 0; i < files.size(); ++i)
         list += (i ? "," : "") + std::string("[") + JStr(files[i].first) + "," +
                 o.Ref(files[i].second.data(), files[i].second.size(), "Uint8Array") + "]";
      o.Raw("files", list + "]").Raw("spool", "null").Str("diskError", "");
      if (!extra.empty()) o.body += extra;
      c_.Send(o);
   }
   static void Vol(const InMsg& m, const Json* o, double& a, double& b)
   {
      a = b = 0;
      const Json* v = o ? o->Get("vol") : m.Get("vol");
      if (v && v->arr.size() >= 2) { a = v->arr[0].num; b = v->arr[1].num; }
   }

   void Gt(const InMsg& m, const std::string& id, const char* type)
   {
      Files files;
      double a, b;
      Vol(m, nullptr, a, b);
      const int dye = m.Truthy("dyeCsv") ? 1 : 0;
      double size;
      if (std::string(type) == "gt")
      {
         ProgressTo(id);
         SinkFiles(files);
         size = isc_scope_gt(m.Str("text").c_str(), m.Num("min", 300), a, b, dye);
      }
      else
      {
         SinkFiles(files);
         size = isc_scope_gt_files(m.Num("min", 300), a, b, dye, 0);
      }
      Clear();
      if (size < 0) return Error(type, id, isc_scope_gt_error());
      SendFiles(type, id, files, "");
   }

   void GtRun(const InMsg& m, const std::string& id)
   {
      ProgressTo(id);
      isc_movie_strips(-1);
      const int32_t rc = isc_scope_gt_run(m.Str("text").c_str(), m.Num("minPhotons", 300), m.Truthy("dyeCsv") ? 1 : 0);
      Clear();
      if (rc < 0) return Error("gt-run", id, isc_scope_gt_error());
      const Json* fo = m.Get("files");
      if (!fo || fo->type != Json::Object)
      {
         OutMsg o("gt-run", id);
         o.Raw("error", "null");
         c_.Send(o);
         return;
      }
      Files files;
      double a, b;
      Vol(m, fo, a, b);
      const Json* mn = fo->Get("min");
      const Json* dc = fo->Get("dyeCsv");
      SinkFiles(files);
      const double size = isc_scope_gt_files(mn && mn->type == Json::Number ? mn->num : 300, a, b, dc && dc->type == Json::Bool && dc->b ? 1 : 0, 1);
      Clear();
      if (size < 0)
      {
         OutMsg o("gt-run", id);
         o.Raw("error", "null").Str("filesError", isc_scope_gt_error());
         c_.Send(o);
         return;
      }
      SendFiles("gt-run", id, files, ",\"error\":null,\"filesFor\":" + Serialize(*fo));
   }
};

// One connection's engine: a reader thread queues the messages (and ends the process when the other end is gone: a
// worker terminated, a Record cancelled), the main thread runs them in order, as a browser worker does.
int RunEngine(Conn& conn, const Options& o)
{
   if (o.threads > 0) isc_set_threads(o.threads);
   std::mutex mu;
   std::condition_variable cv;
   std::deque<std::string> q;
   std::thread reader([&] {
      for (;;)
      {
         std::string p;
         if (!conn.Read(p)) _exit(0);
         {
            std::lock_guard<std::mutex> lk(mu);
            q.push_back(std::move(p));
         }
         cv.notify_one();
      }
   });
   reader.detach();
   Engine eng(conn, o);
   for (;;)
   {
      std::string p;
      {
         std::unique_lock<std::mutex> lk(mu);
         cv.wait(lk, [&] { return !q.empty(); });
         p = std::move(q.front());
         q.pop_front();
      }
      InMsg m;
      std::string err;
      if (!Decode(std::move(p), m, err))
      {
         OutMsg e("error", "");
         e.Str("error", err);
         conn.Send(e);
         continue;
      }
      eng.Handle(m);
   }
}

// ---- listening: a Unix socket or TCP, a process per connection ----
volatile sig_atomic_t g_stop = 0;
void OnStop(int) { g_stop = 1; }

struct Listener
{
   int fd = -1;
   int port = 0;
   std::string relay;   // --relay HOST:PORT: each connection's bytes go there (no engine here)
   std::string sockPath, sockDir;
   bool madeDir = false;
};

bool ListenUnix(const std::string& path, Listener& L, std::string& err)
{
   if (path.size() >= sizeof(sockaddr_un::sun_path)) { err = "the socket path is too long (at most 107 characters)"; return false; }
   const size_t slash = path.rfind('/');
   const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
   struct stat st;
   if (::stat(dir.c_str(), &st) != 0)
   {
      if (::mkdir(dir.c_str(), 0700) != 0) { err = "cannot make " + dir + ": " + std::strerror(errno); return false; }
      L.madeDir = true;
   }
   else if (slash != std::string::npos && dir.compare(0, 5, "/tmp/") == 0)
   {
      // a directory in /tmp must be ours alone: someone else's could hold a socket of theirs
      if (st.st_uid != ::getuid() || (st.st_mode & 077)) { err = dir + " belongs to someone else or is open to others"; return false; }
   }
   ::chmod(dir.c_str(), 0700);
   int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
   sockaddr_un a = {};
   a.sun_family = AF_UNIX;
   std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
   // another server on it?
   if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0)
   {
      ::close(s);
      err = "another inSiliScope server is already serving " + path + ": close its window first";
      return false;
   }
   ::close(s);
   ::unlink(path.c_str());
   s = ::socket(AF_UNIX, SOCK_STREAM, 0);
   const mode_t old = ::umask(077);
   const int rc = ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
   ::umask(old);
   if (rc != 0 || ::listen(s, 16) != 0) { err = "cannot listen on " + path + ": " + std::strerror(errno); ::close(s); return false; }
   ::chmod(path.c_str(), 0600);
   L.fd = s;
   L.sockPath = path;
   L.sockDir = dir;
   return true;
}

bool ListenTcp(const std::string& hostPort, Listener& L, std::string& err)
{
   const size_t c = hostPort.rfind(':');
   const std::string host = c == std::string::npos ? "127.0.0.1" : hostPort.substr(0, c);
   const int port = std::atoi(c == std::string::npos ? hostPort.c_str() : hostPort.c_str() + c + 1);
   int s = ::socket(AF_INET, SOCK_STREAM, 0);
   const int one = 1;
   ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
   sockaddr_in a = {};
   a.sin_family = AF_INET;
   a.sin_port = htons(static_cast<uint16_t>(port));
   if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) { err = "not an IPv4 address: " + host; ::close(s); return false; }
   if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 16) != 0)
   {
      err = "cannot listen on " + hostPort + ": " + std::strerror(errno);
      ::close(s);
      return false;
   }
   sockaddr_in b = {};
   socklen_t bl = sizeof b;
   if (::getsockname(s, reinterpret_cast<sockaddr*>(&b), &bl) == 0) L.port = ntohs(b.sin_port);
   L.fd = s;
   return true;
}

// A relay's connection (--relay: a cluster's login node, the server in a job on a compute node): the bytes both ways
// between the client and HOST:PORT until either end closes.
int ConnectTcp(const std::string& hostPort)
{
   const size_t c = hostPort.rfind(':');
   if (c == std::string::npos) return -1;
   addrinfo hints = {}, *res = nullptr;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   if (::getaddrinfo(hostPort.substr(0, c).c_str(), hostPort.c_str() + c + 1, &hints, &res) != 0) return -1;
   int fd = -1;
   for (addrinfo* r = res; r; r = r->ai_next)
   {
      fd = ::socket(r->ai_family, r->ai_socktype, r->ai_protocol);
      if (fd < 0) continue;
      if (::connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
      ::close(fd);
      fd = -1;
   }
   ::freeaddrinfo(res);
   return fd;
}

void Pump(int a, int b)
{
   char buf[1 << 16];
   pollfd p[2] = { { a, POLLIN, 0 }, { b, POLLIN, 0 } };
   for (;;)
   {
      if (::poll(p, 2, -1) < 0)
      {
         if (errno == EINTR) continue;
         return;
      }
      for (int i = 0; i < 2; ++i)
         if (p[i].revents & (POLLIN | POLLHUP | POLLERR))
         {
            const ssize_t k = ::read(p[i].fd, buf, sizeof buf);
            if (k <= 0 || !WriteAll(p[1 - i].fd, buf, static_cast<size_t>(k))) return;
         }
   }
}

int Serve(Listener& L, const Options& o, int idleMin, bool watchStdin)
{
   std::signal(SIGINT, OnStop);
   std::signal(SIGTERM, OnStop);
   std::signal(SIGHUP, OnStop);
   std::set<pid_t> kids;
   auto last = Clock::now();
   while (!g_stop)
   {
      pollfd p[2] = { { L.fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
      const int np = watchStdin ? 2 : 1;
      const int r = ::poll(p, np, 1000);
      for (pid_t k; (k = ::waitpid(-1, nullptr, WNOHANG)) > 0;)
      {
         kids.erase(k);
         last = Clock::now();
         Log("inSiliScope server: a viewer engine closed (%zu open)", kids.size());
      }
      if (r < 0 && errno != EINTR) break;
      if (watchStdin && r > 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR)))
      {
         // the ssh session that started this server ended (its stdin): so does the server
         char b[256];
         if (::read(0, b, sizeof b) <= 0) break;
      }
      if (r > 0 && (p[0].revents & POLLIN))
      {
         const int cfd = ::accept(L.fd, nullptr, nullptr);
         if (cfd < 0) continue;
         const pid_t k = ::fork();
         if (k == 0)
         {
            ::close(L.fd);
            std::signal(SIGINT, SIG_DFL);
            std::signal(SIGTERM, SIG_DFL);
            std::signal(SIGHUP, SIG_DFL);
            const int nul = ::open("/dev/null", O_RDONLY);
            if (nul >= 0) { ::dup2(nul, 0); ::close(nul); }
            const int one = 1;
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   // (fails quietly on a Unix socket)
            if (!L.relay.empty())
            {
               const int up = ConnectTcp(L.relay);
               if (up < 0) _exit(0);
               ::setsockopt(up, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
               Pump(cfd, up);
               _exit(0);
            }
            if (!Handshake(cfd, o.token)) _exit(0);
            WsConn c(cfd);
            RunEngine(c, o);
            _exit(0);
         }
         ::close(cfd);
         if (k > 0)
         {
            kids.insert(k);
            Log("inSiliScope server: a viewer engine connected (%zu open)", kids.size());
         }
      }
      if (idleMin > 0 && kids.empty() && Clock::now() - last > std::chrono::minutes(idleMin))
      {
         Log("inSiliScope server: idle for %d minutes, stopping", idleMin);
         break;
      }
   }
   for (pid_t k : kids) ::kill(k, SIGTERM);
   for (pid_t k : kids) ::waitpid(k, nullptr, 0);
   if (!L.sockPath.empty())
   {
      ::unlink(L.sockPath.c_str());
      if (L.madeDir) ::rmdir(L.sockDir.c_str());
   }
   Log("inSiliScope server: stopped");
   return 0;
}

void Usage()
{
   std::fprintf(stderr,
                "insiliscope_server %s: the inSiliScope viewer's engine on this machine (docs/remote.md)\n"
                "  --stdio                 serve one client on stdin / stdout (u32-framed messages)\n"
                "  --socket PATH           serve WebSocket connections on a Unix socket (0600, its directory 0700)\n"
                "  --listen HOST:PORT      serve WebSocket connections on TCP (needs --token)\n"
                "  --token T               connections must give ?t=T\n"
                "  --threads N             threads per engine (default: all)\n"
                "  --memory-gb G           the memory budget (default: 80 %% of the RAM)\n"
                "  --idle-exit MIN         stop after MIN minutes without a connection (default 30, 0: never)\n"
                "  --no-stdin-watch        keep serving when stdin closes (default: stop with the ssh session)\n"
                "  --announce FILE         write HOST PORT there once listening (--listen HOST:0 picks a free port)\n"
                "  --relay HOST:PORT       with --socket: pass each connection's bytes to a server there (a cluster job)\n"
                "  --quiet                 no log lines\n"
                "  --info                  print this machine's facts as JSON and exit\n"
                "  --version               print the version and exit\n",
                ISC_VERSION);
}

} // namespace

int main(int argc, char** argv)
{
   std::signal(SIGPIPE, SIG_IGN);
   Options o;
   std::string mode, sock, listen, announce, relay;
   int idle = 30;
   bool watchStdin = true;
   for (int i = 1; i < argc; ++i)
   {
      const std::string a = argv[i];
      auto next = [&]() -> std::string {
         if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
         return argv[++i];
      };
      if (a == "--stdio") mode = "stdio";
      else if (a == "--socket") { mode = "socket"; sock = next(); }
      else if (a == "--listen") { mode = "listen"; listen = next(); }
      else if (a == "--token") o.token = next();
      else if (a == "--threads") o.threads = std::atoi(next().c_str());
      else if (a == "--memory-gb") ::setenv("ISC_MEMORY_BUDGET_GB", next().c_str(), 1);
      else if (a == "--idle-exit") idle = std::atoi(next().c_str());
      else if (a == "--no-stdin-watch") watchStdin = false;
      else if (a == "--announce") announce = next();
      else if (a == "--relay") relay = next();
      else if (a == "--quiet") g_quiet = true;
      else if (a == "--version") { std::printf("%s %s\n", ISC_VERSION, ISC_SERVER_COMMIT); return 0; }
      else if (a == "--info")
      {
         char host[256] = {};
         gethostname(host, sizeof host - 1);
         const long pages = sysconf(_SC_PHYS_PAGES), psize = sysconf(_SC_PAGE_SIZE);
         std::printf("{\"version\":%s,\"commit\":%s,\"protocol\":%d,\"abi\":%d,\"world\":%s,\"host\":%s,\"cpus\":%u,\"memGB\":%.1f}\n",
                     JStr(ISC_VERSION).c_str(), JStr(ISC_SERVER_COMMIT).c_str(), kProtocol, isc_abi_version(),
                     JStr(isc_world_version()).c_str(), JStr(host).c_str(), std::thread::hardware_concurrency(),
                     static_cast<double>(pages) * psize / 1073741824.0);
         return 0;
      }
      else { Usage(); return a == "--help" || a == "-h" ? 0 : 2; }
   }
   if (mode.empty()) { Usage(); return 2; }
   if (mode == "stdio")
   {
      StdioConn c;
      return RunEngine(c, o);
   }
   if (mode == "listen" && o.token.empty()) { std::fprintf(stderr, "--listen needs --token\n"); return 2; }
   // stdin a terminal or /dev/null: nothing to watch
   if (watchStdin && (::isatty(0) || ::lseek(0, 0, SEEK_CUR) >= 0)) watchStdin = false;
   Listener L;
   std::string err;
   if (!(mode == "socket" ? ListenUnix(sock, L, err) : ListenTcp(listen, L, err)))
   {
      std::fprintf(stderr, "insiliscope_server: %s\n", err.c_str());
      return 1;
   }
   char host[256] = {};
   gethostname(host, sizeof host - 1);
   // --announce FILE: "HOST PORT" (a TCP server's, a cluster job's: the launcher on the login node waits for it)
   if (!announce.empty())
   {
      const std::string tmp = announce + ".tmp";
      if (FILE* f = std::fopen(tmp.c_str(), "w"))
      {
         std::fprintf(f, "%s %d\n", host, L.port);
         std::fclose(f);
         std::rename(tmp.c_str(), announce.c_str());
      }
   }
   L.relay = relay;
   const unsigned cpus = std::thread::hardware_concurrency();
   if (!relay.empty()) Log("inSiliScope: relaying to the server at %s", relay.c_str());
   else
      Log("inSiliScope server %s on %s (%u CPUs, using %u per engine)", ISC_VERSION, host, cpus,
          o.threads > 0 ? std::min<unsigned>(o.threads, cpus) : cpus);
   Log("ready -- keep this window open; Ctrl+C stops the server");
   return Serve(L, o, idle, watchStdin);
}
