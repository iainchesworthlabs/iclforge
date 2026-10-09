# ---------------------------------------------------------------------------
# IclforgeLibrary.cmake
#
# One function for the library shape every src/<lib> shares, in place of the 40-odd lines each of
# them repeated with the name swapped:
#
#   iclforge_add_library(<name>
#       SOURCES <file>...          # relative to the calling directory
#       [DEPENDS <lib>...]         # other iclforge libraries; the objects compile against them, and
#                                  # the static and shared wrappers link the matching variant PUBLIC
#       [EMBEDS <target>...]       # libraries the wrappers link PRIVATE and the objects compile
#                                  # against only: the shared library carries the part it uses, the
#                                  # static one passes them on as link-only (iclforge::c embeds the
#                                  # static codecs; the AC-4 decoder and encoder embed their core)
#       [LINK_PUBLIC <item>...]    # ordinary PUBLIC links of the objects (a pinned variant such as
#                                  # iclforge::adm_shared, which every consumer must link)
#       [LINK_PRIVATE <item>...]   # extra PRIVATE links of the objects (wrap dev-only ones in BUILD_INTERFACE)
#       [LINK_PRIVATE_FIRST <item>...] # PRIVATE links of the objects that come before every other
#       [BUILD_TREE_DEPENDS]       # DEPENDS' compile-time use in the build tree only
#       [NO_C4251_SUPPRESSION]     # the library exports no class with private standard-library
#                                  # members, so its users keep MSVC's C4251
#       [PUBLIC_INCLUDES <dir>...] # extra include directories the library's users see (build tree only)
#       [PRIVATE_INCLUDES <dir>...]
#       [STEM <stem>]              # the name of the aliases, files and exports (default <name>):
#                                  # iclforge::<stem>, libiclforge_<stem>, <stem>_static
#       [EXPORT_BASE <BASE>]       # generate_export_header's BASE_NAME (default ICLFORGE_<NAME>)
#       [EXPORT_HEADER <path>]     # below generated/ (default iclforge/<name>/export.hpp)
#       [EXPORT_CUSTOM_CONTENT_VAR <var>])
#
# It makes iclforge_<name>_objects (OBJECT), _static and _shared, the aliases iclforge::<stem>,
# iclforge::<stem>_static and iclforge::<stem>_shared (the bare one follows BUILD_SHARED_LIBS), the
# export header, the file names iclforge_<stem> and iclforge_<stem>_static, and the properties every
# hand-written copy set: PIC, hidden visibility, C++23, the warning and coverage interface targets.
#
# In the minimum-footprint profile (ICLFORGE_MINIMAL_DECODER or ICLFORGE_MINIMAL_ENCODER) a library
# that is built at all is the static archive alone: a bare-metal build has no dynamic loader and
# CMake refuses add_library(SHARED) for a target that has none (CMP0164). The archive takes the
# objects as sources and passes on only their compile-time requirements, because ESP-IDF's
# linker-script generator asks every library in an executable's link graph for its file, which an
# OBJECT library has none of. The objects take the profile's compile options PUBLIC
# (iclforge::minimal_profile: no exceptions, no RTTI, a section per function and datum; a consumer
# has to share whether a call can throw) and are not position-independent, which an archive linked
# into one firmware image has no use for and which costs a RISC-V part an indirection to each global.
#
# LINK_PRIVATE_FIRST, BUILD_TREE_DEPENDS and NO_C4251_SUPPRESSION reproduce what the libraries made
# by hand before planning/consolidation.md's C0 exported, so that the stage changed nothing installed:
# iclforge::ac3 linked the codec first and as an ordinary link, the AC-4 decoder and encoder took
# the inspector's compile requirements in the build tree only, and iclforge::adm, admbridge, iab and
# c set no C4251 suppression.
#
# iclforge_install_library(<name>) does the matching install and export rules.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)
include(GenerateExportHeader)

function(iclforge_add_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "BUILD_TREE_DEPENDS;NO_C4251_SUPPRESSION"
        "STEM;EXPORT_BASE;EXPORT_HEADER;EXPORT_CUSTOM_CONTENT_VAR"
        "SOURCES;DEPENDS;EMBEDS;LINK_PUBLIC;LINK_PRIVATE;LINK_PRIVATE_FIRST;PUBLIC_INCLUDES;PRIVATE_INCLUDES")
    string(TOUPPER "${name}" upper)
    if(NOT ARG_STEM)
        set(ARG_STEM "${name}")
    endif()
    if(NOT ARG_EXPORT_BASE)
        set(ARG_EXPORT_BASE "ICLFORGE_${upper}")
    endif()
    if(NOT ARG_EXPORT_HEADER)
        set(ARG_EXPORT_HEADER "iclforge/${name}/export.hpp")
    endif()
    set(stem "${ARG_STEM}")
    set(objects iclforge_${name}_objects)
    set(static iclforge_${name}_static)
    set(shared iclforge_${name}_shared)
    if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
        set(minimal TRUE)
    else()
        set(minimal FALSE)
    endif()

    add_library(${objects} OBJECT)
    if(minimal)
        add_library(${static} STATIC $<TARGET_OBJECTS:${objects}>)
        target_link_libraries(${static} PUBLIC $<COMPILE_ONLY:${objects}>)
    else()
        add_library(${static} STATIC)
        add_library(${shared} SHARED)
        target_link_libraries(${static} PUBLIC ${objects})
        target_link_libraries(${shared} PUBLIC ${objects})
    endif()

    add_library(iclforge::${stem}_static ALIAS ${static})
    if(NOT minimal)
        add_library(iclforge::${stem}_shared ALIAS ${shared})
    endif()
    if(BUILD_SHARED_LIBS AND NOT minimal)
        add_library(iclforge::${stem} ALIAS ${shared})
    else()
        add_library(iclforge::${stem} ALIAS ${static})
    endif()

    target_sources(${objects} PRIVATE ${ARG_SOURCES})
    target_include_directories(${objects}
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
            "$<INSTALL_INTERFACE:include>")
    foreach(dir IN LISTS ARG_PUBLIC_INCLUDES)
        target_include_directories(${objects} PUBLIC "$<BUILD_INTERFACE:${dir}>")
    endforeach()
    if(ARG_PRIVATE_INCLUDES)
        target_include_directories(${objects} PRIVATE ${ARG_PRIVATE_INCLUDES})
    endif()
    target_compile_features(${objects} PUBLIC cxx_std_23)

    if(ARG_LINK_PRIVATE_FIRST)
        target_link_libraries(${objects} PRIVATE ${ARG_LINK_PRIVATE_FIRST})
    endif()
    # Compile-time use of each dependency on the objects; the wrappers carry the link. Only the
    # compile side: an ordinary link here would put the dependency's shared variant (what the bare
    # alias is under BUILD_SHARED_LIBS) on the link line of the static wrapper too, and a library
    # that embeds the static one, as iclforge::c does, would need it at run time.
    foreach(dep IN LISTS ARG_DEPENDS)
        if(ARG_BUILD_TREE_DEPENDS)
            target_link_libraries(${objects} PRIVATE "$<BUILD_INTERFACE:$<COMPILE_ONLY:iclforge::${dep}>>")
        else()
            target_link_libraries(${objects} PRIVATE "$<COMPILE_ONLY:iclforge::${dep}>")
        endif()
        target_link_libraries(${static} PUBLIC iclforge_${dep}_static)
        if(NOT minimal)
            target_link_libraries(${shared} PUBLIC iclforge_${dep}_shared)
        endif()
    endforeach()
    # An embedded library is compiled against and linked by the wrappers, PRIVATE: the objects have
    # no link step to keep a PRIVATE dependency to themselves, and both wrappers link them PUBLIC for
    # their include path, so a dependency linked there would reach every consumer of either. The
    # $<BUILD_INTERFACE:...> because nothing that uses the installed package compiles these objects.
    foreach(dep IN LISTS ARG_EMBEDS)
        target_link_libraries(${objects} PRIVATE "$<BUILD_INTERFACE:$<COMPILE_ONLY:${dep}>>")
        target_link_libraries(${static} PRIVATE ${dep})
        if(NOT minimal)
            target_link_libraries(${shared} PRIVATE ${dep})
        endif()
    endforeach()
    if(ARG_LINK_PUBLIC)
        target_link_libraries(${objects} PUBLIC ${ARG_LINK_PUBLIC})
    endif()
    # $<BUILD_INTERFACE:...>: dev-only targets must not enter an installed export set.
    target_link_libraries(${objects} PRIVATE
        "$<BUILD_INTERFACE:iclforge::warnings>"
        "$<BUILD_INTERFACE:iclforge::coverage>"
        ${ARG_LINK_PRIVATE})

    if(minimal)
        target_link_libraries(${objects} PUBLIC iclforge::minimal_profile)
        set(pic OFF)
    else()
        set(pic ON)
    endif()
    set_target_properties(${objects} PROPERTIES
        POSITION_INDEPENDENT_CODE ${pic}
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    # C4251 fires for private STL members of exported classes; it is a consequence of exporting,
    # and it fires in every translation unit that parses the decorated class. C4275 is its
    # sibling for a base class: a shared library embeds the libraries it uses (EMBEDS, and
    # base's objects in every one), so in its translation units a class of base is not
    # dll-interface, and an exported class that derives from one (iclforge::ac3's LevelMeter and
    # LoudnessMeter, from base's) draws C4275 for it. The consumers call the base class's
    # members through the derived class's exports or link base themselves.
    if(NOT ARG_NO_C4251_SUPPRESSION)
        target_compile_options(${objects} PUBLIC
            "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>"
            "$<$<CXX_COMPILER_ID:MSVC>:/wd4275>")
    endif()

    set(custom "")
    if(ARG_EXPORT_CUSTOM_CONTENT_VAR)
        set(custom CUSTOM_CONTENT_FROM_VARIABLE ${ARG_EXPORT_CUSTOM_CONTENT_VAR})
    endif()
    # The header is the same whichever target it is made from: the profile's consumers define
    # <BASE>_STATIC_DEFINE, and it asks nothing of the target.
    if(minimal)
        set(export_target ${static})
    else()
        set(export_target ${shared})
        set_target_properties(${shared} PROPERTIES DEFINE_SYMBOL "${ARG_EXPORT_BASE}_BUILDING_SHARED")
    endif()
    generate_export_header(${export_target}
        BASE_NAME ${ARG_EXPORT_BASE}
        ${custom}
        EXPORT_MACRO_NAME ${ARG_EXPORT_BASE}_EXPORT
        EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/${ARG_EXPORT_HEADER}"
        DEFINE_NO_DEPRECATED
        STATIC_DEFINE ${ARG_EXPORT_BASE}_STATIC_DEFINE)
    target_compile_definitions(${objects} PRIVATE ${ARG_EXPORT_BASE}_BUILDING_SHARED)
    target_compile_definitions(${static} PUBLIC ${ARG_EXPORT_BASE}_STATIC_DEFINE)

    # Pre-1.0 there is no ABI promise between two releases, so the SONAME is the full version.
    if(NOT minimal)
        set_target_properties(${shared} PROPERTIES
            VERSION "${PROJECT_VERSION}"
            SOVERSION "${PROJECT_VERSION}"
            OUTPUT_NAME "iclforge_${stem}"
            EXPORT_NAME "${stem}_shared")
    endif()
    set_target_properties(${static} PROPERTIES
        OUTPUT_NAME "iclforge_${stem}_static"
        EXPORT_NAME "${stem}_static")
    set_target_properties(${objects} PROPERTIES
        EXPORT_NAME "${stem}_objects"
        ICLFORGE_DEPENDS "${ARG_DEPENDS}"
        ICLFORGE_STEM "${stem}"
        ICLFORGE_EXPORT_HEADER "${ARG_EXPORT_HEADER}")
endfunction()

# The installed headers of `target`: a HEADERS file set of every .hpp and .h under `include_dir`,
# less any detail/ directory (a library's own plumbing, which no installed header includes) and the
# directories named after it (a part this build left out, or plumbing kept in-tree). The export set
# then names each header, and a consumer's include path comes with it.
function(iclforge_header_set target include_dir)
    file(GLOB_RECURSE headers CONFIGURE_DEPENDS RELATIVE "${include_dir}"
        "${include_dir}/*.hpp" "${include_dir}/*.h")
    list(FILTER headers EXCLUDE REGEX "(^|/)detail/")
    foreach(dir IN LISTS ARGN)
        list(FILTER headers EXCLUDE REGEX "(^|/)${dir}/")
    endforeach()
    list(TRANSFORM headers PREPEND "${include_dir}/")
    target_sources(${target} PUBLIC FILE_SET HEADERS BASE_DIRS "${include_dir}" FILES ${headers})
endfunction()

# The install and export rules of a library iclforge_add_library() made:
#
#   iclforge_install_library(<name>
#       [DESCRIPTION <text>]           # the .pc file's
#       [SHARED_ONLY]                  # install the shared variant whatever the linkage options say
#       [EXPORT_SET <set>]             # an export set several libraries share; the caller installs
#                                      # it once, after the last of them (default <name>Targets,
#                                      # installed here)
#       [REQUIRES <pc>...]             # the .pc file's Requires (default: iclforge-<dep> for each of
#                                      # DEPENDS)
#       [STATIC_REQUIRES <pc>...]      # its Requires.private, for what a static archive calls into
#       [GENERATED_HEADERS <path>...]  # more generated headers to install, below generated/
#       [EXCLUDE <dir>...])            # directories of include/ that are not installed
#
# `ICLFORGE_INSTALL_BOTH_LINKAGES` and BUILD_SHARED_LIBS choose which variants are installed, as
# cmake/InstallLibrary.cmake always did. The export set is under the iclforge:: namespace, in the
# package directory find_package(iclforge) reads; the package config includes it when the file
# exists. The library also gets a pkg-config file, iclforge-<stem>.pc, that requires the .pc files
# of the libraries it links: a static-only install has to name every archive on a link line, since
# nothing in an archive records what it needs (cmake/PkgConfig.cmake). The headers are the library
# directory's include/ tree, by iclforge_header_set() below, and the generated export header.
function(iclforge_install_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "SHARED_ONLY" "DESCRIPTION;EXPORT_SET"
        "REQUIRES;STATIC_REQUIRES;GENERATED_HEADERS;EXCLUDE")
    set(objects iclforge_${name}_objects)
    get_target_property(stem ${objects} ICLFORGE_STEM)
    get_target_property(export_header ${objects} ICLFORGE_EXPORT_HEADER)
    get_target_property(source_dir ${objects} SOURCE_DIR)
    get_target_property(binary_dir ${objects} BINARY_DIR)
    if(ARG_SHARED_ONLY)
        set(targets ${objects} iclforge_${name}_shared)
    elseif(ICLFORGE_INSTALL_BOTH_LINKAGES)
        set(targets ${objects} iclforge_${name}_static iclforge_${name}_shared)
    elseif(BUILD_SHARED_LIBS)
        set(targets ${objects} iclforge_${name}_shared)
    else()
        set(targets ${objects} iclforge_${name}_static)
    endif()
    if(ARG_EXPORT_SET)
        set(export_set "${ARG_EXPORT_SET}")
    else()
        set(export_set "${name}Targets")
    endif()
    iclforge_header_set(${objects} "${source_dir}/include" ${ARG_EXCLUDE})
    install(TARGETS ${targets}
        EXPORT ${export_set}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library
        FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT library)
    foreach(header IN ITEMS "${export_header}" ${ARG_GENERATED_HEADERS})
        get_filename_component(header_dir "${header}" DIRECTORY)
        install(FILES "${binary_dir}/generated/${header}"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${header_dir}"
            COMPONENT library)
    endforeach()
    if(NOT ARG_EXPORT_SET)
        install(EXPORT ${export_set}
            FILE ${export_set}.cmake
            NAMESPACE iclforge::
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
            COMPONENT library)
    endif()

    iclforge_pkgconfig_libname(pc_libname iclforge_${name}_shared iclforge_${stem}
        iclforge_${stem}_static "${targets}")
    if(DEFINED ARG_REQUIRES OR "REQUIRES" IN_LIST ARG_KEYWORDS_MISSING_VALUES)
        set(pc_requires ${ARG_REQUIRES})
    else()
        get_target_property(deps ${objects} ICLFORGE_DEPENDS)
        set(pc_requires "")
        if(deps)
            foreach(dep IN LISTS deps)
                list(APPEND pc_requires iclforge-${dep})
            endforeach()
        endif()
    endif()
    iclforge_install_pkgconfig(
        NAME iclforge-${stem}
        DESCRIPTION "${ARG_DESCRIPTION}"
        LIBNAME "${pc_libname}"
        REQUIRES ${pc_requires}
        STATIC_REQUIRES ${ARG_STATIC_REQUIRES})
    # What the package config and the pkg-config files of the libraries that embed this one read.
    set_property(GLOBAL PROPERTY ICLFORGE_INSTALL_TARGETS_${name} "${targets}")
endfunction()
