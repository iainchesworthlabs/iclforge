# ---------------------------------------------------------------------------
# iclforge::adm - ADM BWF → JOC bridge, phase 2: maps the ADM object graph
# iclforge::adm (src/adm) parses onto iclforge::ac3::oba::AtmosEncoder's input shape.
#
# A NEW standalone module, not folded into either side it bridges, for two
# reasons that are each independently sufficient:
#
#   - src/adm is documentedly codec-blind (see its own CMakeLists.txt/
#     header comments) - it does not, and should not, gain a dependency on
#     iclforge::ac3/iclforge::oba just to know what an "object" or "bed" means in
#     JOC/Atmos terms.
#   - src/ac3 (-> iclforge::ac3, iclforge::ac3::oba::AtmosEncoder) is always built,
#     unconditionally, by every configuration of this project. It cannot
#     gain a dependency on the opt-in, Boost-requiring iclforge::adm without
#     breaking every default build - the exact reason iclforge::adm itself defaults
#     ICLFORGE_BUILD_ADM off (see src/adm/CMakeLists.txt's own header).
#
# Gated by the SAME ICLFORGE_BUILD_ADM option iclforge::adm itself uses, rather than
# a new ICLFORGE_BUILD_ADMBRIDGE of its own: this module is meaningless
# without iclforge::adm (there is nothing to bridge), and iclforge::ac3 is
# unconditionally available regardless, so a separate option would only ever
# take the same value ICLFORGE_BUILD_ADM already does - one flag, not two
# that must be kept in sync by hand. A future DAMF reader (out of scope for now)
# names this module as the "mapping layer" it plans to share, which
# is the other half of why this lives on its own rather than inside either
# neighbour: a future second ADM-graph producer should be able to depend on
# this module without dragging in BW64/RF64-specific parsing it does not
# need.
#
# Installed/exported via find_package(iclforge) (cmake/InstallLibrary.cmake's ICLFORGE_BUILD_ADM
# block) as iclforge::adm_shared ONLY - same shared-only deviation as iclforge::adm itself (see
# src/adm/CMakeLists.txt's header comment), and for a related reason: iclforge_adm_objects PUBLIC-
# links iclforge::adm below, a genuine usage requirement (this module's own public headers name
# iclforge::adm types), so admTargets must resolve it - and since only iclforge_adm_shared is ever exported,
# admbridge has to resolve to that specific variant too, not the ambient iclforge::adm alias
# (which would pick iclforge_adm_static whenever BUILD_SHARED_LIBS=OFF, breaking the export the same
# way an unpinned dependency broke iclforge::adm's own export before the fix described there). See the
# LINK_PUBLIC below.
# ---------------------------------------------------------------------------
# LINK_PUBLIC: this module's own public headers (bridge.hpp, coordinates.hpp, iab_bridge.hpp) name
# types from iclforge::oba (iclforge::ac3), iclforge::adm AND (IAB reader phase 3) iclforge::iab in
# their signatures (iclforge::oba::ObjectPath, iclforge::adm::AdmDocument, iclforge::iab::IABitstreamFrame, ...), so
# anything linking iclforge::adm needs all three transitively.
#
# iclforge::adm_shared explicitly, not the ambient iclforge::adm alias: this file's header
# comment explains why - the alias tracks BUILD_SHARED_LIBS and would resolve to iclforge_adm_static
# whenever that's OFF (the common case, including the coverage preset - the only place
# ICLFORGE_BUILD_ADM=ON is exercised in CI today), which is never in any export set under
# iclforge::adm's shared-only design. Pinning here mirrors src/capi/CMakeLists.txt's iclforge_c precedent
# of embedding one fixed variant regardless of the ambient default, just for a PUBLIC dependency
# instead of a PRIVATE one. iclforge::iab is NOT pinned the same way: it is default-ON with both
# variants in the install/export set, so the ambient alias is exactly right for it.
iclforge_add_library(admbridge
    NO_C4251_SUPPRESSION
    SOURCES
        src/coordinates.cpp
        src/bridge.cpp
        src/iab_bridge.cpp
    LINK_PUBLIC
        iclforge::objects
        iclforge::adm_shared
        iclforge::iab)
