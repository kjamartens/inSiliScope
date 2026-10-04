// Persistent cache of packed blocks -- see blockstore.h.
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#include "blockstore.h"

#include "insiliscope/insiliscope.h"

#include <algorithm>
#include <cstring>

#if !defined(__EMSCRIPTEN__)
#include <filesystem>
#include <fstream>
#include <system_error>
#endif

namespace isc {

namespace {

uint64_t Fnv1a(uint64_t h, const void* data, size_t n)
{
   const unsigned char* b = static_cast<const unsigned char*>(data);
   for (size_t i = 0; i < n; i++) {
      h ^= b[i];
      h *= 1099511628211ull;
   }
   return h;
}

constexpr char kMagic[8] = { 'I', 'S', 'C', 'B', 'L', 'K', '0', '1' };

} // namespace

uint64_t BlockStoreKey(uint32_t seed, uint64_t paramsFingerprint)
{
   uint64_t h = 14695981039346656037ull;
   h = Fnv1a(h, kMagic, sizeof kMagic);
   h = Fnv1a(h, ISC_WORLD_VERSION, std::strlen(ISC_WORLD_VERSION));
   h = Fnv1a(h, &seed, sizeof seed);
   h = Fnv1a(h, &paramsFingerprint, sizeof paramsFingerprint);
   return h;
}

const std::vector<double>* BlockStore::Find(int32_t bx, int32_t by) const
{
   auto it = blocks_.find(Key(bx, by));
   return it == blocks_.end() ? nullptr : &it->second;
}

void BlockStore::Put(int32_t bx, int32_t by, std::vector<double> rows)
{
   const Key k(bx, by);
   auto it = blocks_.find(k);
   if (it != blocks_.end()) {
      if (it->second == rows) return;
      it->second = std::move(rows);
   } else {
      blocks_.emplace(k, std::move(rows));
      order_.push_back(k);
      while (blocks_.size() > BLOCK_STORE_MAX && !order_.empty()) {
         blocks_.erase(order_.front());
         order_.erase(order_.begin());
      }
   }
   dirty_++;
}

void BlockStore::Erase(int32_t bx, int32_t by)
{
   const Key k(bx, by);
   if (blocks_.erase(k)) {
      order_.erase(std::remove(order_.begin(), order_.end(), k), order_.end());
      dirty_++;
   }
}

#if defined(__EMSCRIPTEN__)

bool BlockStore::Open(const std::string&, uint64_t key)
{
   key_ = key;   // no file system in the viewer's WASM: never a store
   return false;
}
bool BlockStore::Flush() { return false; }

#else

bool BlockStore::Open(const std::string& dir, uint64_t key)
{
   namespace fs = std::filesystem;
   std::error_code ec;
   blocks_.clear();
   order_.clear();
   dirty_ = 0;
   key_ = key;
   path_.clear();
   if (dir.empty()) return false;
   fs::create_directories(fs::u8path(dir), ec);
   if (ec && !fs::is_directory(fs::u8path(dir), ec)) return false;
   path_ = (fs::u8path(dir) / BLOCK_STORE_FILE).u8string();
   std::ifstream in(fs::u8path(path_), std::ios::binary);
   if (!in) return true;   // nothing stored yet
   // Header: magic, key, block count. Anything odd: start empty.
   char magic[8];
   uint64_t fileKey = 0;
   uint32_t n = 0;
   in.read(magic, 8);
   in.read(reinterpret_cast<char*>(&fileKey), 8);
   in.read(reinterpret_cast<char*>(&n), 4);
   if (!in || std::memcmp(magic, kMagic, 8) != 0 || fileKey != key || n > BLOCK_STORE_MAX) return true;
   for (uint32_t i = 0; i < n; i++) {
      int32_t bx = 0, by = 0;
      uint32_t cells = 0;
      in.read(reinterpret_cast<char*>(&bx), 4);
      in.read(reinterpret_cast<char*>(&by), 4);
      in.read(reinterpret_cast<char*>(&cells), 4);
      if (!in || cells > 4096) break;
      std::vector<double> rows((size_t)cells * BLOCK_STORE_STRIDE);
      if (cells) in.read(reinterpret_cast<char*>(rows.data()), (std::streamsize)(rows.size() * sizeof(double)));
      if (!in) break;
      const Key k(bx, by);
      if (blocks_.emplace(k, std::move(rows)).second) order_.push_back(k);
   }
   return true;
}

bool BlockStore::Flush()
{
   namespace fs = std::filesystem;
   if (path_.empty() || dirty_ == 0) return !path_.empty();
   const std::string tmp = path_ + ".tmp";
   {
      std::ofstream out(fs::u8path(tmp), std::ios::binary | std::ios::trunc);
      if (!out) return false;
      const uint32_t n = (uint32_t)blocks_.size();
      out.write(kMagic, 8);
      out.write(reinterpret_cast<const char*>(&key_), 8);
      out.write(reinterpret_cast<const char*>(&n), 4);
      for (const Key& k : order_) {
         auto it = blocks_.find(k);
         if (it == blocks_.end()) continue;
         const uint32_t cells = (uint32_t)(it->second.size() / BLOCK_STORE_STRIDE);
         out.write(reinterpret_cast<const char*>(&k.first), 4);
         out.write(reinterpret_cast<const char*>(&k.second), 4);
         out.write(reinterpret_cast<const char*>(&cells), 4);
         if (cells) out.write(reinterpret_cast<const char*>(it->second.data()), (std::streamsize)(it->second.size() * sizeof(double)));
      }
      if (!out) return false;
   }
   std::error_code ec;
   fs::rename(fs::u8path(tmp), fs::u8path(path_), ec);
   if (ec) {
      fs::remove(fs::u8path(tmp), ec);
      return false;
   }
   dirty_ = 0;
   return true;
}

#endif

} // namespace isc
