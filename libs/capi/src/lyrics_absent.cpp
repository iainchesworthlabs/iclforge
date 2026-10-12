// The "absent" half of iclforge_c's lyrics reader (libs/capi/CMakeLists.txt compiles this file
// instead of lyrics.cpp and libs/lyrics when the compiler's standard library has no
// <filesystem>): every iclforge_lyrics_* function iclforge.h declares, with a body that names no
// iclforge::lyrics type. The header declares them unconditionally either way, so a caller sees the
// same symbols whatever this library was built with; here iclforge_lyrics_parse() returns
// ICLFORGE_ERROR_UNSUPPORTED and leaves its out-parameter alone, and everything else reads an empty
// lyrics, as it does for a NULL handle - which is the only handle there can be.
//
// The reader's header names std::filesystem::path in its file-reading signature, and a macOS
// deployment target below 10.15 (what cibuildwheel's Intel wheels and a Rust crate built for
// x86_64 default to) refuses to compile a declaration that does - the reader never links it in a
// build like that (the root CMakeLists.txt's note on libs/lyrics says the same of the Python
// wheel).

#include <cstddef>
#include <cstdint>

#include "iclforge_c/iclforge.h"

extern "C" {

iclforge_status_t iclforge_lyrics_parse(const uint8_t*, size_t, int64_t, iclforge_lyrics_t**) {
    return ICLFORGE_ERROR_UNSUPPORTED;
}

void iclforge_lyrics_destroy(iclforge_lyrics_t*) {}

const char* iclforge_lyrics_error_message(void) { return "lyrics are not built into this library"; }

const char* iclforge_lyrics_title(const iclforge_lyrics_t*) { return ""; }
const char* iclforge_lyrics_artist(const iclforge_lyrics_t*) { return ""; }
const char* iclforge_lyrics_album(const iclforge_lyrics_t*) { return ""; }
const char* iclforge_lyrics_author(const iclforge_lyrics_t*) { return ""; }
const char* iclforge_lyrics_first_singer_name(const iclforge_lyrics_t*) { return ""; }
const char* iclforge_lyrics_second_singer_name(const iclforge_lyrics_t*) { return ""; }

int iclforge_lyrics_length_ms(const iclforge_lyrics_t*, int64_t*) { return 0; }
int64_t iclforge_lyrics_offset_ms(const iclforge_lyrics_t*) { return 0; }
int iclforge_lyrics_timed(const iclforge_lyrics_t*) { return 0; }
int iclforge_lyrics_has_singers(const iclforge_lyrics_t*) { return 0; }

size_t iclforge_lyrics_line_count(const iclforge_lyrics_t*) { return 0; }
int64_t iclforge_lyrics_line_start_ms(const iclforge_lyrics_t*, size_t) { return 0; }
int64_t iclforge_lyrics_line_end_ms(const iclforge_lyrics_t*, size_t) { return 0; }
const char* iclforge_lyrics_line_text(const iclforge_lyrics_t*, size_t) { return ""; }
iclforge_lyrics_singer_t iclforge_lyrics_line_singer(const iclforge_lyrics_t*, size_t) {
    return ICLFORGE_LYRICS_SINGER_UNMARKED;
}
size_t iclforge_lyrics_line_word_count(const iclforge_lyrics_t*, size_t) { return 0; }
int64_t iclforge_lyrics_word_start_ms(const iclforge_lyrics_t*, size_t, size_t) { return 0; }
int64_t iclforge_lyrics_word_end_ms(const iclforge_lyrics_t*, size_t, size_t) { return 0; }
const char* iclforge_lyrics_word_text(const iclforge_lyrics_t*, size_t, size_t) { return ""; }

void iclforge_lyrics_position(const iclforge_lyrics_t*, int64_t, int64_t,
                              iclforge_lyrics_position_t*) {}

iclforge_lyrics_word_state_t iclforge_lyrics_word_state(const iclforge_lyrics_t*, size_t, size_t,
                                                         int64_t) {
    return ICLFORGE_LYRICS_WORD_NOT_YET_SUNG;
}

}  // extern "C"
