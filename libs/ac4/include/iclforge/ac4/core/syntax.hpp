#pragma once

#include "iclforge/base/syntax_trace.hpp"

// The syntax trace both directions share: the AC-4 decoder (src/ac4/src/decoder) emits
// one record per syntax element it reads, and the AC-4 encoder one per element
// it writes, in the same shape, so that the two, and the Python reference
// parser (tools/references/ac4_syntax.py), can be compared record for record.
// The types are iclforge::base's (iclforge/base/syntax_trace.hpp); a record's
// substream is the index into the frame's substream_index_table.
//
// A syntax element is one entry with a bit count in a syntax table of either
// part (ETSI TS 103 190-1 V1.4.1, TS 103 190-2 V1.3.1). Its record carries the
// bit offset where it starts within its substream, how many bits it took, and
// its value:
//
//   - a fixed-width field: its width and its value as an unsigned integer;
//   - variable_bits(n) (Part 1 clause 4.2.2): one record for the whole
//     element, with the total bits and the decoded value;
//   - a Huffman codeword: its length and the index of the codeword in its
//     codebook, before any cb_off is subtracted;
//   - quad_sign_bits and pair_sign_bits: one record for the group, with as
//     many bits as there were nonzero lines and the bits as an integer;
//   - ext_code: one record for the escape, with its total length and the
//     decoded magnitude;
//   - a field whose width the stream sets and whose bits the syntax does not
//     interpret (add_data, extensions_bits, drc2_bits): one record of its
//     width, valued at its last 64 bits, split into 65535-bit records when
//     longer.
//
// byte_align, fill_bits and fill_area are not recorded. docs/verification.md
// states the whole contract.

namespace iclforge::ac4 {

using base::SyntaxRecord;
using base::SyntaxSink;
using base::SyntaxTrace;
using base::sink_of;

}  // namespace iclforge::ac4
