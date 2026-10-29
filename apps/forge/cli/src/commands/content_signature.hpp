#pragma once

#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

#include "../support.hpp"

// AC-4 content signing at the command line: the standalone `ac4-sign` and `ac4-verify`, the
// sign-content step an encoder runs on the file it has just written, and the verify-content /
// check-content step `decode` runs before it plays a signed stream. All of it is opt-in and none of
// it holds a key: the operator names theirs (content-key=, content-chain=, or the
// ICLFORGE_CONTENT_KEY[_FILE] and ICLFORGE_CONTENT_CHAIN_FILE variables) and the certificates they
// trust (trust-anchor=, pin=). docs/concepts/object-signing.md, "AC-4 content signing".
namespace forge_cli::commands {

// Signs a raw .ac4 file into another one.
int run_ac4_sign(std::string_view in_path, std::string_view out_path,
                 const forge_cli::Options& meta);

// Checks a raw .ac4 file's content signature. Strict (exit 6 unless it is valid and anchored)
// unless check-content asks only for the report.
int run_ac4_verify(std::string_view in_path, const forge_cli::Options& meta);

// sign-content on an encoder, in two steps so that a missing key is found before the encode and
// not after it. check_ returns kExitOk when nothing was asked or all is in order, and otherwise
// the exit code to stop with (it has printed why); output_is_ac4 is false where the command is
// about to write something other than AC-4 (an atmos command without codec=ac4). sign_ signs the
// file the encoder wrote, in place.
[[nodiscard]] int check_sign_content_request(std::string_view out_path,
                                             const forge_cli::Options& meta,
                                             bool output_is_ac4 = true);
[[nodiscard]] int sign_encoded_file(std::string_view out_path, const forge_cli::Options& meta);

// verify-content / check-content on a decode of an AC-4 stream. kExitOk means carry on (nothing was
// asked, it checked out, or only a report was wanted); anything else is the exit code to stop with.
[[nodiscard]] int check_content_before_decode(std::span<const std::byte> stream,
                                              std::string_view in_path,
                                              const forge_cli::Options& meta, FILE* status);

}  // namespace forge_cli::commands
