//! TrueHD and MLP decode, probe and scan — `iclforge::truehd` via `iclforge_truehd_*`.
//!
//! A lossless decoder: the samples a decode returns are the samples the stream was made from, 24
//! bits wide and sign-extended into an `i32`. A stream is read in three ways, each a type here:
//!
//! * [`probe`] reads the first major sync and the restart headers after it;
//! * [`scan`] splits a bare `.thd` or `.mlp` file into access units, and [`ScannedStream`] says how
//!   long the whole stream is and where a decoder can join it (the restart interval is the seek
//!   granule: decode from the last restart at or before a sample);
//! * [`Decoder`] takes one whole access unit at a time and keeps the state the next one continues.
//!
//! The results of [`scan`] and [`ScannedStream::restart_for_sample`] are offsets into the CALLER's
//! stream buffer, which Rust expresses as borrows of it, as [`crate::stream`] does for the AC-3
//! family. Only decoding is wrapped. The encoder, the Atmos layer and the metadata edit of the C++
//! library are not (`bindings/rust/README.md`).
//!
//! Present only when the linked `iclforge_c` was built with `ICLFORGE_BUILD_TRUEHD` on (the
//! default); a library built without it answers every function with an unsupported status.

use iclforge_sys as sys;
use std::ffi::CStr;
use std::ptr;
use std::slice;

use crate::error::Error;

/// Mirrors `iclforge::truehd::ErrorKind`, one for one, in the order of `ICLFORGE_ERROR_TRUEHD_*`.
/// [`Error::Truehd`] carries one of these for every status [`probe`], [`scan`] and
/// [`Decoder::decode`] return.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ErrorKind {
    Truncated,
    BadSync,
    BadCheckNibble,
    BadLength,
    BadSignature,
    BadMajorSyncCrc,
    BadRestartSync,
    BadRestartCrc,
    BadParity,
    BadSubstreamCrc,
    BadParameter,
    BadBlockData,
    BadLosslessCheck,
    NeedsRestart,
    Unsupported,
}

impl ErrorKind {
    /// The kind a raw `iclforge_status_t` names, or `None` for a status that is not one of
    /// `ICLFORGE_ERROR_TRUEHD_TRUNCATED` to `_UNSUPPORTED` (the encoder's `_ENCODE_*` codes among
    /// them: nothing here wraps the encoder, so they stay [`Error::Other`]).
    pub(crate) fn from_status(status: sys::iclforge_status_t) -> Option<Self> {
        use sys::*;
        Some(match status {
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_TRUNCATED => ErrorKind::Truncated,
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_SYNC => ErrorKind::BadSync,
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_CHECK_NIBBLE => {
                ErrorKind::BadCheckNibble
            }
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_LENGTH => ErrorKind::BadLength,
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_SIGNATURE => {
                ErrorKind::BadSignature
            }
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_MAJOR_SYNC_CRC => {
                ErrorKind::BadMajorSyncCrc
            }
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_RESTART_SYNC => {
                ErrorKind::BadRestartSync
            }
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_RESTART_CRC => {
                ErrorKind::BadRestartCrc
            }
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_PARITY => ErrorKind::BadParity,
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_SUBSTREAM_CRC => {
                ErrorKind::BadSubstreamCrc
            }
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_PARAMETER => {
                ErrorKind::BadParameter
            }
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_BLOCK_DATA => {
                ErrorKind::BadBlockData
            }
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_LOSSLESS_CHECK => {
                ErrorKind::BadLosslessCheck
            }
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_NEEDS_RESTART => {
                ErrorKind::NeedsRestart
            }
            s if s == iclforge_status_ICLFORGE_ERROR_TRUEHD_UNSUPPORTED => ErrorKind::Unsupported,
            _ => return None,
        })
    }

    pub(crate) fn raw(self) -> sys::iclforge_status_t {
        use sys::*;
        match self {
            ErrorKind::Truncated => iclforge_status_ICLFORGE_ERROR_TRUEHD_TRUNCATED,
            ErrorKind::BadSync => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_SYNC,
            ErrorKind::BadCheckNibble => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_CHECK_NIBBLE,
            ErrorKind::BadLength => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_LENGTH,
            ErrorKind::BadSignature => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_SIGNATURE,
            ErrorKind::BadMajorSyncCrc => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_MAJOR_SYNC_CRC,
            ErrorKind::BadRestartSync => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_RESTART_SYNC,
            ErrorKind::BadRestartCrc => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_RESTART_CRC,
            ErrorKind::BadParity => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_PARITY,
            ErrorKind::BadSubstreamCrc => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_SUBSTREAM_CRC,
            ErrorKind::BadParameter => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_PARAMETER,
            ErrorKind::BadBlockData => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_BLOCK_DATA,
            ErrorKind::BadLosslessCheck => iclforge_status_ICLFORGE_ERROR_TRUEHD_BAD_LOSSLESS_CHECK,
            ErrorKind::NeedsRestart => iclforge_status_ICLFORGE_ERROR_TRUEHD_NEEDS_RESTART,
            ErrorKind::Unsupported => iclforge_status_ICLFORGE_ERROR_TRUEHD_UNSUPPORTED,
        }
    }
}

/// Mirrors `iclforge_truehd_presentation_t`: which substreams a decode reads.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum Presentation {
    /// The highest of substreams 0 to 2 the stream has.
    #[default]
    Best,
    /// Substream 0.
    TwoChannel,
    /// Substreams 0 and 1.
    SixChannel,
    /// Substreams 0 to 2.
    EightChannel,
    /// Every substream, the sixteen-channel one last.
    All,
}

impl Presentation {
    fn to_raw(self) -> sys::iclforge_truehd_presentation_t {
        match self {
            Presentation::Best => {
                sys::iclforge_truehd_presentation_ICLFORGE_TRUEHD_PRESENTATION_BEST
            }
            Presentation::TwoChannel => {
                sys::iclforge_truehd_presentation_ICLFORGE_TRUEHD_PRESENTATION_2CH
            }
            Presentation::SixChannel => {
                sys::iclforge_truehd_presentation_ICLFORGE_TRUEHD_PRESENTATION_6CH
            }
            Presentation::EightChannel => {
                sys::iclforge_truehd_presentation_ICLFORGE_TRUEHD_PRESENTATION_8CH
            }
            Presentation::All => sys::iclforge_truehd_presentation_ICLFORGE_TRUEHD_PRESENTATION_ALL,
        }
    }
}

/// Mirrors `iclforge_truehd_decoder_config_t`. [`DecoderConfig::default`] calls the raw
/// `iclforge_truehd_decoder_config_init()`, as the other configs of this crate do.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DecoderConfig {
    pub presentation: Presentation,
    /// `false` (the default): a check that fails (check nibble, CRC, parity, the lossless check of a
    /// restart interval) is an error and the unit is dropped. `true`: the failure is counted
    /// ([`Decoder::counters`]) and the unit is decoded as far as it reads.
    pub lenient: bool,
}

impl Default for DecoderConfig {
    fn default() -> Self {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: iclforge_truehd_decoder_config_init() overwrites every field of `raw`.
        unsafe { sys::iclforge_truehd_decoder_config_init(&mut raw) };
        let presentation = [
            Presentation::Best,
            Presentation::TwoChannel,
            Presentation::SixChannel,
            Presentation::EightChannel,
            Presentation::All,
        ]
        .into_iter()
        .find(|candidate| candidate.to_raw() == raw.presentation)
        .unwrap_or_default();
        DecoderConfig {
            presentation,
            lenient: raw.lenient != 0,
        }
    }
}

/// What the first major sync and the restart headers after it say of a stream
/// (`iclforge_truehd_stream_info_t`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StreamInfo {
    /// `false` for TrueHD, `true` for MLP (DVD-Audio).
    pub is_mlp: bool,
    pub sample_rate: u32,
    pub substreams: u32,
    /// The channels a decode of substream `i` delivers (the substream and the ones it builds on), 0
    /// for a substream the probe did not reach.
    pub channels: [u32; 4],
    pub samples_per_access_unit: u32,
    /// Counted in the data given.
    pub access_units: usize,
    pub variable_rate: bool,
    /// The 15-bit field of the major sync, as written.
    pub peak_data_rate_field: u32,
    /// A fourth substream, or the major sync's Atmos bit.
    pub has_object_substream: bool,
    /// Where the first access unit's header starts in the data given.
    pub first_unit_offset: usize,
}

/// Reads the first major sync in `data` (it may start anywhere before it) and the restart headers
/// after it. [`ErrorKind::BadSync`] when the data holds no major sync.
pub fn probe(data: &[u8]) -> Result<StreamInfo, Error> {
    let mut raw = unsafe { std::mem::zeroed() };
    // SAFETY: `data` is valid for `data.len()` bytes; `raw` is a valid out-parameter.
    let status = unsafe { sys::iclforge_truehd_probe(data.as_ptr(), data.len(), &mut raw) };
    Error::check(status)?;
    let raw: sys::iclforge_truehd_stream_info_t = raw;
    Ok(StreamInfo {
        is_mlp: raw.is_mlp != 0,
        sample_rate: raw.sample_rate,
        substreams: raw.substreams as u32,
        channels: raw.channels.map(|channels| channels as u32),
        samples_per_access_unit: raw.samples_per_access_unit as u32,
        access_units: raw.access_units,
        variable_rate: raw.variable_rate != 0,
        peak_data_rate_field: raw.peak_data_rate_field as u32,
        has_object_substream: raw.has_object_substream != 0,
        first_unit_offset: raw.first_unit_offset,
    })
}

/// The access units of a bare stream: slices of the caller's data, in order.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Units<'a> {
    /// Each unit's bytes, its 4-byte header first.
    pub units: Vec<&'a [u8]>,
    /// Bytes between units (and ahead of the first) that did not read as one.
    pub skipped_bytes: usize,
}

/// Splits a bare stream into its access units. The data may start anywhere before the first major
/// sync; bytes between units that do not read as one are skipped and counted. A stream with no unit
/// in it is [`ErrorKind::BadSync`].
pub fn scan(data: &[u8]) -> Result<Units<'_>, Error> {
    let mut raw: *mut sys::iclforge_truehd_units_t = ptr::null_mut();
    // SAFETY: `data` is valid for `data.len()` bytes; `raw` is a valid out-parameter.
    let status = unsafe { sys::iclforge_truehd_scan(data.as_ptr(), data.len(), &mut raw) };
    Error::check(status)?;
    let raw = ptr::NonNull::new(raw).expect("iclforge_truehd_scan returned OK with a null list");
    // SAFETY: `raw` is a list this function now owns; it is destroyed below, after its offsets are
    // copied out, and the units it names are slices of `data`, which outlives them.
    let units = unsafe {
        let count = sys::iclforge_truehd_units_count(raw.as_ptr());
        let mut units = Vec::with_capacity(count);
        for index in 0..count {
            let offset = sys::iclforge_truehd_units_offset(raw.as_ptr(), index);
            let size = sys::iclforge_truehd_units_size(raw.as_ptr(), index);
            units.push(&data[offset..offset + size]);
        }
        let skipped_bytes = sys::iclforge_truehd_units_skipped_bytes(raw.as_ptr());
        sys::iclforge_truehd_units_destroy(raw.as_ptr());
        Units {
            units,
            skipped_bytes,
        }
    };
    Ok(units)
}

/// Where to start decoding to reach a sample: the last restart at or before the unit that holds it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Restart {
    /// The unit's number, counting the units that read from the first one. The first sample a
    /// decoder gives from there is this times [`ScannedStream::samples_per_unit`].
    pub unit: u64,
    /// The unit's first byte (its 4-byte header) in the data the scan was made from.
    pub offset: usize,
}

/// A scan of a whole stream (`iclforge_truehd_scanned_stream_t`): how long it is, and where a
/// decoder can join it. A unit holds 40, 80 or 160 samples, so a unit's first sample is its number
/// times that length; a decoder can only start at a unit that carries a major sync, so those units
/// are listed ("restarts"). To reach a sample, decode from the last restart at or before its unit:
/// the restart interval is the seek granule.
///
/// Borrows the stream it was made from: a [`Restart`]'s offset is into it. Read by any number of
/// threads at once.
pub struct ScannedStream<'a> {
    raw: ptr::NonNull<sys::iclforge_truehd_scanned_stream_t>,
    data: &'a [u8],
}

// SAFETY: the C handle is read by any number of threads at once and changed by none
// (iclforge.h), and `data` is a shared borrow.
unsafe impl Send for ScannedStream<'_> {}
unsafe impl Sync for ScannedStream<'_> {}

impl<'a> ScannedStream<'a> {
    /// Scans the whole of `data`. `restart_spacing` thins the restarts to one every that many units
    /// at most (0 or 1 lists every one).
    pub fn new(data: &'a [u8], restart_spacing: u64) -> Result<Self, Error> {
        let mut raw: *mut sys::iclforge_truehd_scanned_stream_t = ptr::null_mut();
        // SAFETY: `data` is valid for `data.len()` bytes; `raw` is a valid out-parameter.
        let status = unsafe {
            sys::iclforge_truehd_scanned_stream_create(
                data.as_ptr(),
                data.len(),
                restart_spacing,
                &mut raw,
            )
        };
        Error::check(status)?;
        let raw = ptr::NonNull::new(raw)
            .expect("iclforge_truehd_scanned_stream_create returned OK with a null stream");
        Ok(ScannedStream { raw, data })
    }

    pub fn sample_rate(&self) -> u32 {
        unsafe { sys::iclforge_truehd_scanned_stream_sample_rate(self.raw.as_ptr()) }
    }

    /// 40, 80 or 160 by the sample rate.
    pub fn samples_per_unit(&self) -> u32 {
        unsafe { sys::iclforge_truehd_scanned_stream_samples_per_unit(self.raw.as_ptr()) as u32 }
    }

    /// The units that read.
    pub fn units(&self) -> u64 {
        unsafe { sys::iclforge_truehd_scanned_stream_units(self.raw.as_ptr()) }
    }

    /// Every sample the stream holds per channel: whole units less the padding the last one counts.
    pub fn samples(&self) -> u64 {
        unsafe { sys::iclforge_truehd_scanned_stream_samples(self.raw.as_ptr()) }
    }

    /// The samples at the end of the last unit that are padding.
    pub fn padding_samples(&self) -> u32 {
        unsafe { sys::iclforge_truehd_scanned_stream_padding_samples(self.raw.as_ptr()) as u32 }
    }

    pub fn duration_seconds(&self) -> f64 {
        unsafe { sys::iclforge_truehd_scanned_stream_duration_seconds(self.raw.as_ptr()) }
    }

    /// Bytes that were not part of a unit that read, those ahead of the first unit included.
    pub fn skipped_bytes(&self) -> u64 {
        unsafe { sys::iclforge_truehd_scanned_stream_skipped_bytes(self.raw.as_ptr()) }
    }

    /// The part of [`skipped_bytes`](Self::skipped_bytes) that came before the first unit (not a
    /// loss of the stream).
    pub fn leading_bytes(&self) -> u64 {
        unsafe { sys::iclforge_truehd_scanned_stream_leading_bytes(self.raw.as_ptr()) }
    }

    /// The restarts listed, in order.
    pub fn restarts(&self) -> Vec<Restart> {
        let count = unsafe { sys::iclforge_truehd_scanned_stream_restart_count(self.raw.as_ptr()) };
        (0..count)
            .map(|index| unsafe {
                Restart {
                    unit: sys::iclforge_truehd_scanned_stream_restart_unit(
                        self.raw.as_ptr(),
                        index,
                    ),
                    offset: sys::iclforge_truehd_scanned_stream_restart_offset(
                        self.raw.as_ptr(),
                        index,
                    ) as usize,
                }
            })
            .collect()
    }

    /// The sample at `seconds`, rounded down; a negative time is sample 0.
    pub fn sample_at_seconds(&self, seconds: f64) -> u64 {
        unsafe { sys::iclforge_truehd_scanned_stream_sample_at_seconds(self.raw.as_ptr(), seconds) }
    }

    /// Where to start decoding to reach `sample`, or `None` for a sample past the end. Decode on
    /// from [`Restart::offset`]; [`stream_from`](Self::stream_from) is the bytes.
    pub fn restart_for_sample(&self, sample: u64) -> Option<Restart> {
        let mut unit = 0;
        let mut offset = 0;
        // SAFETY: both out-parameters are valid.
        let status = unsafe {
            sys::iclforge_truehd_scanned_stream_restart_for_sample(
                self.raw.as_ptr(),
                sample,
                &mut unit,
                &mut offset,
            )
        };
        if Error::check(status).is_err() {
            return None;
        }
        Some(Restart {
            unit,
            offset: offset as usize,
        })
    }

    /// The stream from `restart` to its end: what to hand to a [`Decoder`] to reach the restart's
    /// samples.
    pub fn stream_from(&self, restart: Restart) -> &'a [u8] {
        &self.data[restart.offset..]
    }
}

impl Drop for ScannedStream<'_> {
    fn drop(&mut self) {
        // SAFETY: `self.raw` is owned exclusively by this handle.
        unsafe { sys::iclforge_truehd_scanned_stream_destroy(self.raw.as_ptr()) };
    }
}

impl std::fmt::Debug for ScannedStream<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ScannedStream")
            .field("units", &self.units())
            .field("samples", &self.samples())
            .finish()
    }
}

/// What a [`Decoder`] has counted (`iclforge_truehd_counters_t`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Counters {
    /// Given to [`Decoder::decode`].
    pub units: u64,
    /// ... that gave samples.
    pub units_decoded: u64,
    /// ... that gave none because no restart header had come yet.
    pub units_waiting: u64,
    pub restarts: u64,
    pub lossless_checks_ok: u64,
    pub lossless_check_failures: u64,
    /// Nibble, major sync CRC, restart CRC, parity and segment CRC together.
    pub check_failures: u64,
}

/// A TrueHD or MLP decoder (`iclforge_truehd_decoder_t`): one whole access unit at a time, header
/// included, keeping the state (filter histories, the restart headers, the lossless check) the next
/// one continues. Used by one thread at a time.
pub struct Decoder {
    raw: ptr::NonNull<sys::iclforge_truehd_decoder_t>,
}

// SAFETY: one thread at a time (the C handle has no thread affinity), and `&mut self` for every
// call that changes it.
unsafe impl Send for Decoder {}

impl Decoder {
    pub fn new(config: &DecoderConfig) -> Result<Self, Error> {
        let mut raw_config = unsafe { std::mem::zeroed() };
        // SAFETY: _config_init() fills every field, which are then set from `config`.
        unsafe { sys::iclforge_truehd_decoder_config_init(&mut raw_config) };
        let mut raw_config: sys::iclforge_truehd_decoder_config_t = raw_config;
        raw_config.presentation = config.presentation.to_raw();
        raw_config.lenient = config.lenient as _;
        let mut out: *mut sys::iclforge_truehd_decoder_t = ptr::null_mut();
        // SAFETY: `raw_config` is fully initialised; `out` is a valid out-parameter.
        let status = unsafe { sys::iclforge_truehd_decoder_create(&raw_config, &mut out) };
        Error::check(status)?;
        let raw = ptr::NonNull::new(out)
            .expect("iclforge_truehd_decoder_create returned OK with a null decoder");
        Ok(Decoder { raw })
    }

    /// Forgets the stream: the next unit has to carry a major sync.
    pub fn reset(&mut self) {
        unsafe { sys::iclforge_truehd_decoder_reset(self.raw.as_ptr()) };
    }

    /// For a caller that dropped input between the unit it decoded last and the next one (a packet
    /// lost, bytes [`scan`] skipped): the decoder cannot see a missing unit, so this makes every
    /// substream wait for its next restart header, as after an error.
    pub fn lost_data(&mut self) {
        unsafe { sys::iclforge_truehd_decoder_lost_data(self.raw.as_ptr()) };
    }

    /// Decodes one whole access unit. `Ok(None)` when the unit gave no samples yet (it came before a
    /// major sync, or before its substreams' restart headers: a stream joined part way). On an error
    /// the decoder stays usable, and the substream that failed waits for its next restart header.
    pub fn decode(&mut self, access_unit: &[u8]) -> Result<Option<Decoded>, Error> {
        let mut out: *mut sys::iclforge_truehd_decoded_t = ptr::null_mut();
        // SAFETY: `access_unit` is valid for its length; `out` is a valid out-parameter.
        let status = unsafe {
            sys::iclforge_truehd_decoder_decode(
                self.raw.as_ptr(),
                access_unit.as_ptr(),
                access_unit.len(),
                &mut out,
            )
        };
        Error::check(status)?;
        Ok(ptr::NonNull::new(out).map(|raw| Decoded { raw }))
    }

    /// The sentence that goes with the status of the most recent decode: empty when that decode
    /// succeeded or none has been made.
    pub fn last_error_detail(&self) -> String {
        // SAFETY: a static NUL-terminated string.
        unsafe {
            CStr::from_ptr(sys::iclforge_truehd_decoder_last_error_detail(
                self.raw.as_ptr(),
            ))
            .to_string_lossy()
            .into_owned()
        }
    }

    pub fn counters(&self) -> Counters {
        let mut raw = unsafe { std::mem::zeroed() };
        // SAFETY: `raw` is a valid out-parameter.
        unsafe { sys::iclforge_truehd_decoder_counters(self.raw.as_ptr(), &mut raw) };
        let raw: sys::iclforge_truehd_counters_t = raw;
        Counters {
            units: raw.units,
            units_decoded: raw.units_decoded,
            units_waiting: raw.units_waiting,
            restarts: raw.restarts,
            lossless_checks_ok: raw.lossless_checks_ok,
            lossless_check_failures: raw.lossless_check_failures,
            check_failures: raw.check_failures,
        }
    }
}

impl Drop for Decoder {
    fn drop(&mut self) {
        // SAFETY: `self.raw` is owned exclusively by this decoder.
        unsafe { sys::iclforge_truehd_decoder_destroy(self.raw.as_ptr()) };
    }
}

impl std::fmt::Debug for Decoder {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Decoder")
            .field("counters", &self.counters())
            .finish()
    }
}

/// One decoded access unit (`iclforge_truehd_decoded_t`). Owns its samples, so it stays valid after
/// the next decode.
pub struct Decoded {
    raw: ptr::NonNull<sys::iclforge_truehd_decoded_t>,
}

// SAFETY: an owned, immutable buffer with no thread affinity.
unsafe impl Send for Decoded {}
unsafe impl Sync for Decoded {}

impl Decoded {
    pub fn sample_rate(&self) -> u32 {
        unsafe { sys::iclforge_truehd_decoded_sample_rate(self.raw.as_ptr()) }
    }

    pub fn channels(&self) -> usize {
        unsafe { sys::iclforge_truehd_decoded_channels(self.raw.as_ptr()) as usize }
    }

    /// Samples a channel.
    pub fn samples(&self) -> usize {
        unsafe { sys::iclforge_truehd_decoded_samples(self.raw.as_ptr()) as usize }
    }

    /// The substream the PCM is from.
    pub fn substream(&self) -> usize {
        unsafe { sys::iclforge_truehd_decoded_substream(self.raw.as_ptr()) as usize }
    }

    /// Whether a restart header was read in the substream decoded.
    pub fn restarted(&self) -> bool {
        unsafe { sys::iclforge_truehd_decoded_restarted(self.raw.as_ptr()) != 0 }
    }

    /// The last unit of a stream that does not end on a unit boundary carries a marker counting the
    /// samples at its end that are padding. They are not in [`samples`](Self::samples) or
    /// [`pcm`](Self::pcm).
    pub fn padding_samples(&self) -> usize {
        unsafe { sys::iclforge_truehd_decoded_padding_samples(self.raw.as_ptr()) as usize }
    }

    /// Interleaved, `samples() * channels()` values, each 24 bits wide and sign-extended into the
    /// `i32` (so a 16-bit stream's values are multiples of 256).
    pub fn pcm(&self) -> &[i32] {
        let count = self.samples() * self.channels();
        // SAFETY: the pointer is valid for `count` values until the handle is destroyed.
        unsafe {
            let data = sys::iclforge_truehd_decoded_pcm(self.raw.as_ptr());
            if data.is_null() || count == 0 {
                &[]
            } else {
                slice::from_raw_parts(data, count)
            }
        }
    }

    /// The unit's bytes after its last substream segment, uninterpreted (the object audio metadata
    /// is in them). Empty when the unit has none.
    pub fn extra_data(&self) -> &[u8] {
        let mut size = 0;
        // SAFETY: valid until the handle is destroyed; the size is written through `size`.
        unsafe {
            let data = sys::iclforge_truehd_decoded_extra_data(self.raw.as_ptr(), &mut size);
            if data.is_null() || size == 0 {
                &[]
            } else {
                slice::from_raw_parts(data, size)
            }
        }
    }

    /// The speaker of channel `index` by the major sync's channel assignment (`"L"`, `"R"`, `"C"`,
    /// `"LFE"`, `"Ls"`, `"Rs"`, ...), or `None` when the major sync does not name the channels for
    /// this channel count or the index is out of range.
    pub fn channel_name(&self, index: usize) -> Option<&str> {
        // SAFETY: a static NUL-terminated string.
        let name = unsafe {
            CStr::from_ptr(sys::iclforge_truehd_decoded_channel_name(
                self.raw.as_ptr(),
                index,
            ))
        };
        match name.to_str() {
            Ok("") | Err(_) => None,
            Ok(name) => Some(name),
        }
    }
}

impl Drop for Decoded {
    fn drop(&mut self) {
        // SAFETY: `self.raw` is owned exclusively by this handle.
        unsafe { sys::iclforge_truehd_decoded_destroy(self.raw.as_ptr()) };
    }
}

impl std::fmt::Debug for Decoded {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Decoded")
            .field("channels", &self.channels())
            .field("samples", &self.samples())
            .finish()
    }
}
