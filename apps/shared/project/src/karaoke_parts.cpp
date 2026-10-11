#include "karaoke_parts.hpp"

namespace iclforge::apps::project {

const KaraokeMode* karaoke_mode(bool melody, int vocals) {
    // Table C.2.1's five modes, in coded order.
    static constexpr std::array<KaraokeMode, 5> kModes{{
        {"3/0", {{{"L", "L"}, {"C", "M"}, {"R", "R"}, {"", ""}, {"", ""}}}, 3},
        {"2/1", {{{"L", "L"}, {"R", "R"}, {"Cs", "V1"}, {"", ""}, {"", ""}}}, 3},
        {"3/1", {{{"L", "L"}, {"C", "M"}, {"R", "R"}, {"Cs", "V1"}, {"", ""}}}, 4},
        {"2/2", {{{"L", "L"}, {"R", "R"}, {"Ls", "V1"}, {"Rs", "V2"}, {"", ""}}}, 4},
        {"3/2", {{{"L", "L"}, {"C", "M"}, {"R", "R"}, {"Ls", "V1"}, {"Rs", "V2"}}}, 5},
    }};
    if (vocals < 0 || vocals > 2) {
        return nullptr;
    }
    if (vocals == 0) {
        return melody ? &kModes[0] : nullptr;
    }
    if (vocals == 1) {
        return melody ? &kModes[2] : &kModes[1];
    }
    return melody ? &kModes[4] : &kModes[3];
}

}  // namespace iclforge::apps::project
