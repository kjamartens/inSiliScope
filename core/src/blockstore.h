// Persistent cache of packed blocks (spec/PORT.md 6.6): one small file per
// world key (seed, normalized parameters, ISC_WORLD_VERSION) holding the
// packed pose of every cell of every block this process packed -- cx, cy,
// x, y, packRot, the five numbers Relax/Prune produce; everything else of a
// cell is recomputed from its address when a block is taken back
// (World::SetPackedBlock's rule), so a stale or foreign file can only cost a
// repack, never a wrong cell. A cache only: every answer stays a pure
// function of (seed, params, window).
//
// The file is rewritten whole (temp file + rename) and capped at
// BLOCK_STORE_MAX blocks (FIFO), a few MB at most. One file per directory:
// a world of another key overwrites it ("keep the last one").
// No OS dependencies beyond <filesystem>/<fstream>; a stub under Emscripten
// (the viewer keeps its own copy in the browser's local storage).
//
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace isc {

constexpr int BLOCK_STORE_STRIDE = 5;        // cx, cy, x, y, packRot per cell
constexpr size_t BLOCK_STORE_MAX = 4096;     // blocks kept (FIFO)
constexpr const char* BLOCK_STORE_FILE = "packed_blocks.bin";

class BlockStore {
public:
   // Opens dir/packed_blocks.bin for the world `key`: loads it when it holds
   // that key, starts empty otherwise (the next Flush overwrites it). False
   // (and no store) if the directory cannot be made or is not writable.
   bool Open(const std::string& dir, uint64_t key);
   const std::string& Path() const { return path_; }
   // The stored rows of a block (BLOCK_STORE_STRIDE doubles per cell), or null.
   const std::vector<double>* Find(int32_t bx, int32_t by) const;
   // Records a block (replacing any stored rows); the oldest blocks go when
   // over BLOCK_STORE_MAX.
   void Put(int32_t bx, int32_t by, std::vector<double> rows);
   void Erase(int32_t bx, int32_t by);
   size_t Pending() const { return dirty_; }
   size_t Size() const { return blocks_.size(); }
   // Writes the file if anything changed since the last write. False on an
   // I/O error (the pending count is kept for a later try).
   bool Flush();

private:
   using Key = std::pair<int32_t, int32_t>;
   std::string path_;
   uint64_t key_ = 0;
   std::map<Key, std::vector<double>> blocks_;
   std::vector<Key> order_;   // insertion order, for the FIFO cap
   size_t dirty_ = 0;
};

// The world key: a 64-bit FNV-1a of the version, the seed and the
// normalized parameters' bit patterns.
uint64_t BlockStoreKey(uint32_t seed, uint64_t paramsFingerprint);

} // namespace isc
