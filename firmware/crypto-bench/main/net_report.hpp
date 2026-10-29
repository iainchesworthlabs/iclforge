#pragma once

#include <cstddef>

// A board with no USB cannot be read back over the console and cannot be put right with a cable. The
// P4X kit is one: it sits on Ethernet and PoE, and takes an update over OTA (planning/esp32-board-bench.md).
// Two files implement this header, and main/CMakeLists.txt picks one by -DCRYPTO_BENCH_REPORT:
//
//   net_report_none.cpp      the default: nothing happens, the bench prints to the console and repeats.
//   net_report_ethernet.cpp  the kit: after one pass the bench brings up the Ethernet port, serves the
//                            pass on TCP port 8080 and then returns the board to the image it came from.
//
// The way back does not depend on the network. The Hearth sink's bootloader is built with app rollback
// (firmware/hearth-sink/partitions.csv), and an image put on the board by OTA is on trial until it marks
// itself valid; the bench never does, so ANY reset - a crash, a hang, the deadline below, a power cut -
// boots the image the board was running before.
namespace bench {

// Starts a deadline (15 minutes) after which the board goes back to its previous image whatever else
// is happening.
void arm_return_to_previous_image();

// Hands `text` (the pass, as printed) to whoever asks on the network, then goes back to the previous
// image and does not return. With no network in the build it returns at once.
void report_over_network(const char* text, std::size_t size);

}  // namespace bench
