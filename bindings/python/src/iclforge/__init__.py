"""Python bindings for iclforge - a clean-room AC-3/E-AC-3 encoder and decoder in C++23,
including the Atmos-in-DD+ object layer (OAMD + JOC).

Everything here is a thin wrapper over the compiled ``_iclforge`` extension - see
docs/library/python-api.md in the iclforge repository for the full surface and worked examples.
"""

from ._iclforge import (
    BLOCKS_PER_FRAME,
    KARAOKE_MAX_LEVEL_DB,
    KARAOKE_MIN_LEVEL_DB,
    MAX_AC3_CHANNELS,
    SAMPLES_PER_FRAME,
    TRANSFORM_DELAY_SAMPLES,
    Ac3DecodeError,
    Ac3EncodeError,
    Ac3Error,
    Ac3ScanError,
    Ac4DecodeError,
    Ac4EncodeError,
    Ac4Error,
    AccessUnitTiming,
    Acmod,
    AtmosConfig,
    AtmosEncoder,
    CentreMixLevel,
    DecodedAccessUnit,
    DecodedFrame,
    DecodedProgram,
    DecodedSubstream,
    DecodeError,
    DecoderConfig,
    DynamicObject,
    Eac3Decoder,
    EncoderConfig,
    FrameDecoder,
    FrameEncoder,
    FrameError,
    FrameHeader,
    HeavyConfig,
    KaraokeReproduction,
    KaraokeVocals,
    LatencyBudget,
    ObjectPlacement,
    Position,
    Profile,
    ProfileId,
    Program,
    SampleRate,
    ScanError,
    ScannedProgramme,
    ScannedStream,
    StreamKind,
    StreamType,
    SubstreamService,
    SurroundMixLevel,
    ac4,
    access_unit_at_sample,
    access_unit_at_seconds,
    access_unit_timing,
    build_codec_config_box,
    containers,
    describe,
    eac3,
    fullbw_channel_count,
    meta,
    profile_for,
    profile_name,
    read_frame_header,
    sample_rate_hz,
    scan,
    signing,
    split_access_units,
    split_frames,
    stream_bsid,
    stream_duration_samples,
    stream_duration_seconds,
    uniform_access_unit_samples,
    verify,
)

# The lyrics reader is a submodule the extension has only when it was built with iclforge::lyrics
# and a <filesystem> to read files through (bindings/python/CMakeLists.txt). Every wheel has it but
# the Intel macOS one, whose deployment target (10.9) has no std::filesystem, and imported with the
# names above an absent submodule took `import iclforge` with it there. Absent, `iclforge.lyrics`
# is an AttributeError.
try:
    from ._iclforge import lyrics
except ImportError:
    _HAS_LYRICS = False
else:
    _HAS_LYRICS = True

# The TrueHD and MLP codec is a submodule the extension has only when it was built with
# iclforge::truehd (ICLFORGE_BUILD_TRUEHD). Every wheel has it; a trimmed developer build may not,
# and importing it with the names above would take `import iclforge` with it there. Absent,
# `iclforge.truehd` is an AttributeError.
try:
    from ._iclforge import TruehdError, truehd
except ImportError:
    _HAS_TRUEHD = False
else:
    _HAS_TRUEHD = True

try:
    from importlib.metadata import PackageNotFoundError
    from importlib.metadata import version as _pkg_version

    __version__ = _pkg_version("iclforge")
except PackageNotFoundError:  # pragma: no cover - an editable/unbuilt checkout
    __version__ = "0.0.0"

__all__ = [
    "BLOCKS_PER_FRAME",
    "KARAOKE_MAX_LEVEL_DB",
    "KARAOKE_MIN_LEVEL_DB",
    "MAX_AC3_CHANNELS",
    "SAMPLES_PER_FRAME",
    "TRANSFORM_DELAY_SAMPLES",
    "Ac3DecodeError",
    "Ac3EncodeError",
    "Ac3Error",
    "Ac3ScanError",
    "Ac4DecodeError",
    "Ac4EncodeError",
    "Ac4Error",
    "AccessUnitTiming",
    "Acmod",
    "AtmosConfig",
    "AtmosEncoder",
    "CentreMixLevel",
    "DecodeError",
    "DecodedAccessUnit",
    "DecodedFrame",
    "DecodedProgram",
    "DecodedSubstream",
    "DecoderConfig",
    "DynamicObject",
    "Eac3Decoder",
    "EncoderConfig",
    "FrameDecoder",
    "FrameEncoder",
    "FrameError",
    "FrameHeader",
    "HeavyConfig",
    "KaraokeReproduction",
    "KaraokeVocals",
    "LatencyBudget",
    "ObjectPlacement",
    "Position",
    "Profile",
    "ProfileId",
    "Program",
    "SampleRate",
    "ScanError",
    "ScannedProgramme",
    "ScannedStream",
    "StreamKind",
    "StreamType",
    "SubstreamService",
    "SurroundMixLevel",
    "TruehdError",
    "__version__",
    "ac4",
    "access_unit_at_sample",
    "access_unit_at_seconds",
    "access_unit_timing",
    "build_codec_config_box",
    "containers",
    "describe",
    "eac3",
    "fullbw_channel_count",
    "lyrics",
    "meta",
    "profile_for",
    "profile_name",
    "read_frame_header",
    "sample_rate_hz",
    "scan",
    "signing",
    "split_access_units",
    "split_frames",
    "stream_bsid",
    "stream_duration_samples",
    "stream_duration_seconds",
    "truehd",
    "uniform_access_unit_samples",
    "verify",
]
if not _HAS_LYRICS:
    __all__.remove("lyrics")
if not _HAS_TRUEHD:
    __all__.remove("truehd")
    __all__.remove("TruehdError")
