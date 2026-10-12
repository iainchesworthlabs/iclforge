//! Shared value types mirroring the enums and small structs `iclforge.h` uses across both
//! codecs (`iclforge::ac3::SampleRate`, `iclforge::ac3::Acmod`, the DRC presets, the mix-level tables, latency).

use iclforge_sys as sys;

/// A. `iclforge_sample_rate_t` — A/52 Table 5.6, plus Annex E's three `fscod2` reduced rates.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SampleRate {
    Hz48000,
    Hz44100,
    Hz32000,
    /// E-AC-3 only.
    Hz24000,
    /// E-AC-3 only.
    Hz22050,
    /// E-AC-3 only.
    Hz16000,
}

impl SampleRate {
    pub(crate) fn to_raw(self) -> sys::iclforge_sample_rate_t {
        match self {
            SampleRate::Hz48000 => sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_48000,
            SampleRate::Hz44100 => sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_44100,
            SampleRate::Hz32000 => sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_32000,
            SampleRate::Hz24000 => sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_24000,
            SampleRate::Hz22050 => sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_22050,
            SampleRate::Hz16000 => sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_16000,
        }
    }

    /// `None` for a raw value this crate doesn't recognize (see `Error::Other`'s own doc comment
    /// on why the C side of this API is treated as open).
    pub(crate) fn from_raw(raw: sys::iclforge_sample_rate_t) -> Option<Self> {
        #[allow(non_upper_case_globals)]
        Some(match raw {
            sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_48000 => SampleRate::Hz48000,
            sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_44100 => SampleRate::Hz44100,
            sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_32000 => SampleRate::Hz32000,
            sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_24000 => SampleRate::Hz24000,
            sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_22050 => SampleRate::Hz22050,
            sys::iclforge_sample_rate_ICLFORGE_SAMPLE_RATE_16000 => SampleRate::Hz16000,
            _ => return None,
        })
    }

    /// The rate in Hz, for callers who just want a number.
    pub fn hz(self) -> u32 {
        match self {
            SampleRate::Hz48000 => 48_000,
            SampleRate::Hz44100 => 44_100,
            SampleRate::Hz32000 => 32_000,
            SampleRate::Hz24000 => 24_000,
            SampleRate::Hz22050 => 22_050,
            SampleRate::Hz16000 => 16_000,
        }
    }
}

/// `iclforge_acmod_t` — A/52 Table 5.8. `DualMono` (1+1) is two independent programmes sharing
/// one syncframe, not a channel count.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Acmod {
    /// 1+1: Ch1, Ch2.
    DualMono,
    /// C.
    Mono,
    /// L, R.
    Stereo,
    /// L, C, R.
    Channels3_0,
    /// L, R, S.
    Channels2_1,
    /// L, C, R, S.
    Channels3_1,
    /// L, R, SL, SR.
    Channels2_2,
    /// L, C, R, SL, SR.
    Channels3_2,
}

impl Acmod {
    pub(crate) fn to_raw(self) -> sys::iclforge_acmod_t {
        match self {
            Acmod::DualMono => sys::iclforge_acmod_ICLFORGE_ACMOD_DUAL_MONO,
            Acmod::Mono => sys::iclforge_acmod_ICLFORGE_ACMOD_1_0,
            Acmod::Stereo => sys::iclforge_acmod_ICLFORGE_ACMOD_2_0,
            Acmod::Channels3_0 => sys::iclforge_acmod_ICLFORGE_ACMOD_3_0,
            Acmod::Channels2_1 => sys::iclforge_acmod_ICLFORGE_ACMOD_2_1,
            Acmod::Channels3_1 => sys::iclforge_acmod_ICLFORGE_ACMOD_3_1,
            Acmod::Channels2_2 => sys::iclforge_acmod_ICLFORGE_ACMOD_2_2,
            Acmod::Channels3_2 => sys::iclforge_acmod_ICLFORGE_ACMOD_3_2,
        }
    }

    pub(crate) fn from_raw(raw: sys::iclforge_acmod_t) -> Option<Self> {
        #[allow(non_upper_case_globals)]
        Some(match raw {
            sys::iclforge_acmod_ICLFORGE_ACMOD_DUAL_MONO => Acmod::DualMono,
            sys::iclforge_acmod_ICLFORGE_ACMOD_1_0 => Acmod::Mono,
            sys::iclforge_acmod_ICLFORGE_ACMOD_2_0 => Acmod::Stereo,
            sys::iclforge_acmod_ICLFORGE_ACMOD_3_0 => Acmod::Channels3_0,
            sys::iclforge_acmod_ICLFORGE_ACMOD_2_1 => Acmod::Channels2_1,
            sys::iclforge_acmod_ICLFORGE_ACMOD_3_1 => Acmod::Channels3_1,
            sys::iclforge_acmod_ICLFORGE_ACMOD_2_2 => Acmod::Channels2_2,
            sys::iclforge_acmod_ICLFORGE_ACMOD_3_2 => Acmod::Channels3_2,
            _ => return None,
        })
    }

    /// Full-bandwidth channel count this `acmod` names (excludes LFE — see
    /// `EncoderConfig::lfe`/`Eac3FrameConfig::lfe`). `DualMono` is 2 (Ch1, Ch2), matching
    /// `iclforge_encoder_channel_count()`'s own convention rather than "0, it's not really a
    /// channel count" — the encoder still wants two spans either way.
    pub fn full_bandwidth_channel_count(self) -> usize {
        match self {
            Acmod::DualMono => 2,
            Acmod::Mono => 1,
            Acmod::Stereo => 2,
            Acmod::Channels3_0 => 3,
            Acmod::Channels2_1 => 3,
            Acmod::Channels3_1 => 4,
            Acmod::Channels2_2 => 4,
            Acmod::Channels3_2 => 5,
        }
    }
}

/// `iclforge_drc_profile_t` — the five conventional Dolby DRC curves, the same presets `forge
/// --drc` accepts.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum DrcProfile {
    FilmStandard,
    FilmLight,
    MusicStandard,
    MusicLight,
    Speech,
}

impl DrcProfile {
    pub(crate) fn to_raw(self) -> sys::iclforge_drc_profile_t {
        match self {
            DrcProfile::FilmStandard => sys::iclforge_drc_profile_ICLFORGE_DRC_FILM_STANDARD,
            DrcProfile::FilmLight => sys::iclforge_drc_profile_ICLFORGE_DRC_FILM_LIGHT,
            DrcProfile::MusicStandard => sys::iclforge_drc_profile_ICLFORGE_DRC_MUSIC_STANDARD,
            DrcProfile::MusicLight => sys::iclforge_drc_profile_ICLFORGE_DRC_MUSIC_LIGHT,
            DrcProfile::Speech => sys::iclforge_drc_profile_ICLFORGE_DRC_SPEECH,
        }
    }

    pub(crate) fn from_raw(raw: sys::iclforge_drc_profile_t) -> Option<Self> {
        #[allow(non_upper_case_globals)]
        Some(match raw {
            sys::iclforge_drc_profile_ICLFORGE_DRC_FILM_STANDARD => DrcProfile::FilmStandard,
            sys::iclforge_drc_profile_ICLFORGE_DRC_FILM_LIGHT => DrcProfile::FilmLight,
            sys::iclforge_drc_profile_ICLFORGE_DRC_MUSIC_STANDARD => DrcProfile::MusicStandard,
            sys::iclforge_drc_profile_ICLFORGE_DRC_MUSIC_LIGHT => DrcProfile::MusicLight,
            sys::iclforge_drc_profile_ICLFORGE_DRC_SPEECH => DrcProfile::Speech,
            _ => return None,
        })
    }
}

/// `iclforge_centre_mix_level_t` — A/52 Table 5.9 (§5.4.2.4). The default is −4.5 dB, the
/// library's own (`iclforge_encoder_config_init()`, `iclforge::ac3::EncoderConfig::cmixlev`) - not the
/// first variant.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum CentreMixLevel {
    Minus3Db,
    #[default]
    Minus4_5Db,
    Minus6Db,
}

impl CentreMixLevel {
    pub(crate) fn to_raw(self) -> sys::iclforge_centre_mix_level_t {
        match self {
            CentreMixLevel::Minus3Db => sys::iclforge_centre_mix_level_ICLFORGE_CMIXLEV_MINUS_3DB,
            CentreMixLevel::Minus4_5Db => {
                sys::iclforge_centre_mix_level_ICLFORGE_CMIXLEV_MINUS_4_5DB
            }
            CentreMixLevel::Minus6Db => sys::iclforge_centre_mix_level_ICLFORGE_CMIXLEV_MINUS_6DB,
        }
    }

    pub(crate) fn from_raw(raw: sys::iclforge_centre_mix_level_t) -> Option<Self> {
        #[allow(non_upper_case_globals)]
        Some(match raw {
            sys::iclforge_centre_mix_level_ICLFORGE_CMIXLEV_MINUS_3DB => CentreMixLevel::Minus3Db,
            sys::iclforge_centre_mix_level_ICLFORGE_CMIXLEV_MINUS_4_5DB => {
                CentreMixLevel::Minus4_5Db
            }
            sys::iclforge_centre_mix_level_ICLFORGE_CMIXLEV_MINUS_6DB => CentreMixLevel::Minus6Db,
            _ => return None,
        })
    }
}

/// `iclforge_surround_mix_level_t` — A/52 Table 5.10 (§5.4.2.5). The default is −6 dB, the
/// library's own (`iclforge_encoder_config_init()`, `iclforge::ac3::EncoderConfig::surmixlev`) - not the
/// first variant.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum SurroundMixLevel {
    Minus3Db,
    #[default]
    Minus6Db,
    Silent,
}

impl SurroundMixLevel {
    pub(crate) fn to_raw(self) -> sys::iclforge_surround_mix_level_t {
        match self {
            SurroundMixLevel::Minus3Db => {
                sys::iclforge_surround_mix_level_ICLFORGE_SURMIXLEV_MINUS_3DB
            }
            SurroundMixLevel::Minus6Db => {
                sys::iclforge_surround_mix_level_ICLFORGE_SURMIXLEV_MINUS_6DB
            }
            SurroundMixLevel::Silent => sys::iclforge_surround_mix_level_ICLFORGE_SURMIXLEV_SILENT,
        }
    }

    pub(crate) fn from_raw(raw: sys::iclforge_surround_mix_level_t) -> Option<Self> {
        #[allow(non_upper_case_globals)]
        Some(match raw {
            sys::iclforge_surround_mix_level_ICLFORGE_SURMIXLEV_MINUS_3DB => {
                SurroundMixLevel::Minus3Db
            }
            sys::iclforge_surround_mix_level_ICLFORGE_SURMIXLEV_MINUS_6DB => {
                SurroundMixLevel::Minus6Db
            }
            sys::iclforge_surround_mix_level_ICLFORGE_SURMIXLEV_SILENT => SurroundMixLevel::Silent,
            _ => return None,
        })
    }
}

/// `iclforge_heavy_config_t` — `iclforge::ac3::meta::HeavyConfig` verbatim (§7.7.2).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HeavyConfig {
    pub dialogue_target_dbfs: f64,
    pub peak_ceiling_dbfs: f64,
    pub release_db_per_second: f64,
}

impl HeavyConfig {
    pub(crate) fn to_raw(self) -> sys::iclforge_heavy_config_t {
        sys::iclforge_heavy_config_t {
            dialogue_target_dbfs: self.dialogue_target_dbfs,
            peak_ceiling_dbfs: self.peak_ceiling_dbfs,
            release_db_per_second: self.release_db_per_second,
        }
    }

    pub(crate) fn from_raw(raw: sys::iclforge_heavy_config_t) -> Self {
        HeavyConfig {
            dialogue_target_dbfs: raw.dialogue_target_dbfs,
            peak_ceiling_dbfs: raw.peak_ceiling_dbfs,
            release_db_per_second: raw.release_db_per_second,
        }
    }
}

impl Default for HeavyConfig {
    /// Same defaults `iclforge_heavy_config_init()` fills — this is a plain value struct with no
    /// growth story of its own (unlike the `_config_init` structs), so mirroring its literal
    /// defaults here rather than calling the raw `_init()` function is safe and one call cheaper.
    fn default() -> Self {
        HeavyConfig {
            dialogue_target_dbfs: -20.0,
            peak_ceiling_dbfs: -0.5,
            release_db_per_second: 20.0,
        }
    }
}

/// `iclforge_decoder_config_t` — shared by both the AC-3 (`FrameDecoder`) and E-AC-3/Atmos
/// (`Eac3Decoder`) decoders.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DecoderConfig {
    /// 0.0..1.0, §7.7.1's "Partial Compression".
    pub drc_scale: f64,
    pub heavy_compression: bool,
}

impl DecoderConfig {
    pub(crate) fn to_raw(self) -> sys::iclforge_decoder_config_t {
        sys::iclforge_decoder_config_t {
            drc_scale: self.drc_scale,
            heavy_compression: self.heavy_compression as i32,
        }
    }
}

impl Default for DecoderConfig {
    /// Calls `iclforge_decoder_config_init()` rather than hand-mirroring its defaults — see
    /// `bindings/rust/README.md` on why every config type in this crate goes through its raw `_init()`
    /// first (the `_config_init` growth convention `iclforge.h` documents).
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: iclforge_decoder_config_init() unconditionally overwrites every field of
        // `raw` via a full struct assignment (see e.g. iclforge_atmos_config_init()'s
        // implementation in libs/capi/src/atmos.cpp) - `raw` is never read before being written.
        unsafe { sys::iclforge_decoder_config_init(&mut raw) };
        DecoderConfig {
            drc_scale: raw.drc_scale,
            heavy_compression: raw.heavy_compression != 0,
        }
    }
}

/// `iclforge_karaoke_reproduction_t` — how an AC-3 karaoke stream (ETSI TS 102 366 Annex C:
/// `bsmod` 7 with a layout above 1/0) is reproduced by [`crate::ac3::Decoder::with_karaoke`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum KaraokeReproduction {
    /// The channels as they are coded; the default.
    #[default]
    Off,
    /// The karaoke-aware decoder (C.2.3.1, Table C.2.2): L, C, R and then the LFE, with the
    /// melody at unity in the centre and the vocals at the stream's own surround level.
    Aware,
    /// The karaoke-capable decoder (C.2.3.2, Table C.2.3): as aware, with the listener's choice
    /// of [`KaraokeVocals`] and each part at its own level.
    Capable,
}

impl KaraokeReproduction {
    pub(crate) fn to_raw(self) -> sys::iclforge_karaoke_reproduction_t {
        match self {
            KaraokeReproduction::Off => sys::iclforge_karaoke_reproduction_ICLFORGE_KARAOKE_OFF,
            KaraokeReproduction::Aware => sys::iclforge_karaoke_reproduction_ICLFORGE_KARAOKE_AWARE,
            KaraokeReproduction::Capable => {
                sys::iclforge_karaoke_reproduction_ICLFORGE_KARAOKE_CAPABLE
            }
        }
    }

    pub(crate) fn from_raw(raw: sys::iclforge_karaoke_reproduction_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::iclforge_karaoke_reproduction_ICLFORGE_KARAOKE_AWARE => KaraokeReproduction::Aware,
            sys::iclforge_karaoke_reproduction_ICLFORGE_KARAOKE_CAPABLE => {
                KaraokeReproduction::Capable
            }
            _ => KaraokeReproduction::Off,
        }
    }
}

/// `iclforge_karaoke_vocals_t` — which of a stream's vocals a listener hears under
/// [`KaraokeReproduction::Capable`]. A vocal the stream does not code reproduces nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum KaraokeVocals {
    None,
    V1,
    V2,
    #[default]
    Both,
}

impl KaraokeVocals {
    pub(crate) fn to_raw(self) -> sys::iclforge_karaoke_vocals_t {
        match self {
            KaraokeVocals::None => sys::iclforge_karaoke_vocals_ICLFORGE_KARAOKE_VOCALS_NONE,
            KaraokeVocals::V1 => sys::iclforge_karaoke_vocals_ICLFORGE_KARAOKE_VOCALS_V1,
            KaraokeVocals::V2 => sys::iclforge_karaoke_vocals_ICLFORGE_KARAOKE_VOCALS_V2,
            KaraokeVocals::Both => sys::iclforge_karaoke_vocals_ICLFORGE_KARAOKE_VOCALS_BOTH,
        }
    }

    pub(crate) fn from_raw(raw: sys::iclforge_karaoke_vocals_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::iclforge_karaoke_vocals_ICLFORGE_KARAOKE_VOCALS_NONE => KaraokeVocals::None,
            sys::iclforge_karaoke_vocals_ICLFORGE_KARAOKE_VOCALS_V1 => KaraokeVocals::V1,
            sys::iclforge_karaoke_vocals_ICLFORGE_KARAOKE_VOCALS_V2 => KaraokeVocals::V2,
            _ => KaraokeVocals::Both,
        }
    }
}

/// The bottom of a karaoke level's span in dB against Table C.2.3, which is off
/// (`ICLFORGE_KARAOKE_MIN_LEVEL_DB`).
pub const KARAOKE_MIN_LEVEL_DB: f64 = -24.0;
/// The top of a karaoke level's span, 12 dB over the table (`ICLFORGE_KARAOKE_MAX_LEVEL_DB`).
pub const KARAOKE_MAX_LEVEL_DB: f64 = 12.0;

/// `iclforge_karaoke_config_t` — the reproduction a karaoke-aware or karaoke-capable decoder
/// gives a karaoke stream. Construct with [`KaraokeConfig::default`] (the raw `_init()`'s
/// defaults: no reproduction, both vocals, every level 0 dB).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct KaraokeConfig {
    pub reproduction: KaraokeReproduction,
    /// Read under [`KaraokeReproduction::Capable`] alone.
    pub vocals: KaraokeVocals,
    /// Annex C.2.3's "additional flexibility", the capable decoder's alone: the level of the guide
    /// melody and of each vocal against Table C.2.3, in dB. 0.0 is the table bit for bit; a value
    /// is held to [`KARAOKE_MIN_LEVEL_DB`]..=[`KARAOKE_MAX_LEVEL_DB`], the bottom of which is off,
    /// and one that is not a number is 0 dB. A part raised far enough lowers the others.
    pub melody_db: f64,
    pub vocal1_db: f64,
    pub vocal2_db: f64,
}

impl KaraokeConfig {
    pub(crate) fn to_raw(self) -> sys::iclforge_karaoke_config_t {
        sys::iclforge_karaoke_config_t {
            reproduction: self.reproduction.to_raw(),
            vocals: self.vocals.to_raw(),
            melody_db: self.melody_db,
            vocal1_db: self.vocal1_db,
            vocal2_db: self.vocal2_db,
        }
    }
}

impl Default for KaraokeConfig {
    /// Calls `iclforge_karaoke_config_init()` rather than hand-mirroring its defaults, as every
    /// config type in this crate does.
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: iclforge_karaoke_config_init() overwrites every field of `raw` with a full
        // struct assignment; `raw` is never read before being written.
        unsafe { sys::iclforge_karaoke_config_init(&mut raw) };
        KaraokeConfig {
            reproduction: KaraokeReproduction::from_raw(raw.reproduction),
            vocals: KaraokeVocals::from_raw(raw.vocals),
            melody_db: raw.melody_db,
            vocal1_db: raw.vocal1_db,
            vocal2_db: raw.vocal2_db,
        }
    }
}

/// `iclforge_latency_t` — the algorithmic delay of an encode → decode chain, in samples at the
/// coded sample rate. See `iclforge.h`'s own extensive comment on this struct for what each term
/// means; compute time is a separate question this type says nothing about.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Latency {
    pub frame_samples: i32,
    pub transform_samples: i32,
    pub lookahead_samples: i32,
    pub holdback_samples: i32,
}

impl Latency {
    pub(crate) fn from_raw(raw: sys::iclforge_latency_t) -> Self {
        Latency {
            frame_samples: raw.frame_samples,
            transform_samples: raw.transform_samples,
            lookahead_samples: raw.lookahead_samples,
            holdback_samples: raw.holdback_samples,
        }
    }

    /// The figure to budget with: the sum of the four terms.
    pub fn total_samples(&self) -> i32 {
        self.frame_samples + self.transform_samples + self.lookahead_samples + self.holdback_samples
    }
}
