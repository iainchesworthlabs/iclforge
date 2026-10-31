// Asks every render endpoint the library can see whether it will take an AC-3, E-AC-3 or TrueHD
// bitstream, and streams a file to one of them, so a receiver's own display can say what it locked
// to. That display is the only oracle for "did the bitstream arrive intact": nothing on this side
// can see it.
//
//   crucible-probe                              list endpoints and what they accept
//   crucible-probe <id> <file> [seconds]        bitstream <file> to it, looping (default 25 s)
//
// <file> is a raw AC-3 or E-AC-3 stream (.ac3, .ec3) or a raw TrueHD stream (.thd), by what it
// holds, not by its name. A TrueHD stream goes out as MAT bursts on the eight-channel link
// (docs/library/muxing-and-sinks.md, "TrueHD"): `testdata/truehd` has small ones, and
// `forge truehd-encode` makes a 5.1 one from a WAV. At 192 kHz and eight channels a burst is
// 20 ms, so the stream has to be fed ahead of real time and a gap is a dropped lock.
//
// enumerate_render_devices() is the same call Crucible's output stage makes, so a "yes" here is
// the "yes" it would get. On PipeWire that answer comes from the sink's iec958.codecs, the session
// manager's reading of the display's EDID, and never from a connect alone: the adapter accepts an
// IEC 958 stream on a headphone jack as readily as on a receiver. On Windows it is WASAPI's
// IsFormatSupported in exclusive mode.
//
// It was tools/checks/passthrough_probe.cpp, built by hand against a PipeWire build of a library
// that has since been split; it is a target now, so it builds wherever Crucible does.
//
// If the device goes away mid-stream (the cable pulled, the receiver switched off), the sink stops
// itself, the probe stops with it, says so, and exits 5.

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/audio/passthrough.hpp"
#include "iclforge/containers/iec61937/iec61937.hpp"
#include "iclforge/containers/iec61937/mat.hpp"
#include "iclforge/truehd/io/elementary.hpp"

namespace {

using iclforge::audio::BitstreamFormat;

bool submit_paced(iclforge::audio::PassthroughSink& sink, std::span<const std::byte> burst,
                  std::chrono::steady_clock::time_point until) {
    // submit() returns false when the sink is ahead of real time, and the caller waits, exactly as
    // the sink documents. It also returns false once the sink has stopped itself because its
    // device went away, which no wait changes, so that ends the wait; so does `until`, which a
    // sink that takes nothing would otherwise hold this past.
    while (!sink.submit(burst)) {
        if (!sink.running() || std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// Each unit of the stream as a burst, handed to `send` when one is complete; false when `send`
// says stop. A burst is a unit's own for AC-3, gathered for E-AC-3 and for TrueHD (24 to a MAT
// burst).
struct Source {
    BitstreamFormat format = BitstreamFormat::kAc3;
    std::uint32_t sample_rate = 48000;
    std::string name;
    std::size_t units = 0;
    // One pass over the stream.
    bool (*pass)(const Source&, iclforge::audio::PassthroughSink&,
                 std::chrono::steady_clock::time_point, std::size_t&) = nullptr;
    std::vector<std::byte> data;
    std::vector<std::vector<std::byte>> split;  // AC-3 and E-AC-3 units
};

bool pass_ac3(const Source& source, iclforge::audio::PassthroughSink& sink,
              std::chrono::steady_clock::time_point until, std::size_t& bursts) {
    iclforge::containers::iec61937::Eac3BurstPacker packer;
    for (const auto& unit : source.split) {
        if (std::chrono::steady_clock::now() >= until) {
            return true;
        }
        if (source.format == BitstreamFormat::kEac3) {
            const auto burst = packer.push(unit);
            if (!burst) {
                std::printf("packer refused an access unit\n");
                return false;
            }
            if (!*burst) {
                continue;  // the packer is still gathering this burst
            }
            if (!submit_paced(sink, **burst, until)) {
                return false;
            }
        } else {
            const auto burst = iclforge::containers::iec61937::wrap_frame(unit);
            if (!burst) {
                std::printf("wrap_frame refused a syncframe\n");
                return false;
            }
            if (!submit_paced(sink, *burst, until)) {
                return false;
            }
        }
        ++bursts;
    }
    return true;
}

bool pass_truehd(const Source& source, iclforge::audio::PassthroughSink& sink,
                 std::chrono::steady_clock::time_point until, std::size_t& bursts) {
    iclforge::containers::iec61937::MatPacker packer;
    iclforge::truehd::ElementaryReader reader(source.data);
    while (const auto unit = reader.next()) {
        if (std::chrono::steady_clock::now() >= until) {
            return true;
        }
        const auto burst = packer.push(*unit);
        if (!burst) {
            std::printf("MAT packer refused an access unit: %s\n",
                        std::string(iclforge::containers::iec61937::describe(burst.error())).c_str());
            return false;
        }
        if (!*burst) {
            continue;  // still gathering the 24 access units of this burst
        }
        if (!submit_paced(sink, **burst, until)) {
            return false;
        }
        ++bursts;
    }
    return true;
}

int open_source(const std::string& path, Source& source) {
    std::ifstream in(path, std::ios::binary);
    for (std::istreambuf_iterator<char> it(in), end; it != end; ++it) {
        source.data.push_back(static_cast<std::byte>(*it));
    }
    if (source.data.empty()) {
        std::printf("cannot read %s\n", path.c_str());
        return 2;
    }
    source.name = path;

    // TrueHD first: its major sync is not found in an AC-3 stream, and an AC-3 stream's syncword
    // is not found in a TrueHD one at a unit boundary.
    if (const auto info = iclforge::truehd::probe_stream(source.data)) {
        source.format = BitstreamFormat::kTrueHd;
        source.sample_rate = info->sample_rate;
        source.units = info->access_units;
        source.pass = &pass_truehd;
        return 0;
    }
    const auto bsid = iclforge::ac3::stream_bsid(source.data);
    if (!bsid) {
        std::printf("%s holds no TrueHD major sync and no AC-3 syncframe\n", path.c_str());
        return 2;
    }
    const bool eac3 = *bsid > 8;
    // Whole access units for E-AC-3 (an independent substream's frame plus any dependents), which
    // is the granularity the burst packer wants; single syncframes for AC-3, each its own burst.
    const auto units = eac3 ? iclforge::ac3::split_access_units(source.data)
                            : iclforge::ac3::split_frames(source.data);
    if (!units || units->empty()) {
        std::printf("could not split %s into frames\n", path.c_str());
        return 2;
    }
    for (const auto& unit : *units) {
        source.split.emplace_back(unit.begin(), unit.end());
    }
    source.format = eac3 ? BitstreamFormat::kEac3 : BitstreamFormat::kAc3;
    source.units = source.split.size();
    source.pass = &pass_ac3;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto devices = iclforge::audio::enumerate_render_devices(48000);
    if (!devices) {
        std::printf("enumerate_render_devices refused: %s\n",
                    std::string(iclforge::audio::describe(devices.error())).c_str());
        return 1;
    }
    std::printf("%zu render endpoint(s)\n", devices->size());
    for (const auto& d : *devices) {
        std::printf("  %s%-48s ac3=%-3s eac3=%-3s truehd=%-3s exclusive-pcm=%-3s  \"%s\"\n",
                    d.is_default ? "*" : " ", d.id.c_str(),
                    d.supports_ac3_passthrough ? "YES" : "no",
                    d.supports_eac3_passthrough ? "YES" : "no",
                    d.supports_truehd_passthrough ? "YES" : "no",
                    d.supports_exclusive_pcm ? "yes" : "no", d.name.c_str());
    }
    if (argc < 3) {
        return 0;
    }

    const std::string id = argv[1];
    const std::string path = argv[2];
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 25;

    Source source;
    if (const int bad = open_source(path, source); bad != 0) {
        return bad;
    }
    std::printf("\n%s: %s at %u Hz, %zu units; bitstreaming to %s for %d s ...\n", path.c_str(),
                std::string(iclforge::audio::format_name(source.format)).c_str(), source.sample_rate,
                source.units, id.c_str(), seconds);

    iclforge::audio::PassthroughSink sink;
    const auto started = sink.start(id, source.sample_rate, source.format);
    if (!started) {
        std::printf("start refused: %s\n",
                    std::string(iclforge::audio::describe(started.error())).c_str());
        return 3;
    }

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::size_t bursts = 0;
    std::size_t loops = 0;
    bool taking = true;
    while (taking && std::chrono::steady_clock::now() < until) {
        taking = source.pass(source, sink, until, bursts);
        if (taking) {
            ++loops;
        } else if (sink.running() && std::chrono::steady_clock::now() < until) {
            sink.stop();
            return 4;  // a packer refused: said above
        }
    }
    const auto stats = sink.stats();
    // Not running, having not been stopped: the device went away under the stream, and the sink
    // stopped itself.
    const bool lost = !sink.running();
    sink.stop();
    if (lost) {
        std::string endpoint = id;
        for (const auto& d : *devices) {
            if (d.id == id) {
                endpoint = "\"" + d.name + "\" (" + id + ")";
            }
        }
        std::printf("stopped: %s went away mid-stream (unplugged, switched off, disabled, or taken "
                    "by the system) after %zu bursts; sink reports %llu submitted, %llu rendered, "
                    "%llu underruns\n",
                    endpoint.c_str(), bursts, static_cast<unsigned long long>(stats.bursts_submitted),
                    static_cast<unsigned long long>(stats.bursts_rendered),
                    static_cast<unsigned long long>(stats.underruns));
        return 5;
    }
    std::printf("done: %zu bursts over %zu loop(s); sink reports %llu submitted, %llu rendered, "
                "%llu underruns\n",
                bursts, loops, static_cast<unsigned long long>(stats.bursts_submitted),
                static_cast<unsigned long long>(stats.bursts_rendered),
                static_cast<unsigned long long>(stats.underruns));
    return 0;
}
