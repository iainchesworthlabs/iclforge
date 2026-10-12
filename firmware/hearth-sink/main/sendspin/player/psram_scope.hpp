#pragma once

// Allocations made by one task for a while, taken from PSRAM when the part has it (planning/
// karaoke-phase2.md, W7d: a lyric document).
//
// A parsed lyric document is many small blocks (a line, a word, a string each), which a part's
// allocator keeps in internal RAM whatever the total: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL sends
// everything under 16 KB there. Measured on boards that is about ten times the document's text at the
// peak, from the heap Wi-Fi's receive buffers and the decoder come out of: an S3 with 2,000 bytes of
// lyrics was left 7.8 KB, with 4,000 bytes 0.4 KB.
//
// So the document is read and held with this scope open: the global operator new in psram_scope.cpp
// takes PSRAM first for the task that opened it, and internal RAM when there is none (or none left).
// Blocks are freed as any are, with free(), wherever and whenever. Only one task has a scope open at
// a time (the caller's own lock); another task's allocations are not touched.

namespace iclforge::player {

class PsramScope {
public:
    PsramScope();
    ~PsramScope();
    PsramScope(const PsramScope&) = delete;
    PsramScope& operator=(const PsramScope&) = delete;
    PsramScope(PsramScope&&) = delete;
    PsramScope& operator=(PsramScope&&) = delete;

private:
    void* previous_;
};

}  // namespace iclforge::player
