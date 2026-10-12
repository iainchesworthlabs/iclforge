//! Timed lyrics for a karaoke stream — `iclforge::lyrics` via `iclforge_lyrics_t`.
//!
//! A lyrics file (LRC in UTF-8 or UTF-16, a SubRip `.srt`, an Advanced SubStation Alpha `.ass`
//! with its karaoke tags, or a MIDI karaoke `.kar`) is read into lines of timed words, and a
//! stateless cursor says which line and word a playing position is in. The reader is the one
//! Hearth, Crucible and the sink firmware use. This crate takes the file's *bytes*; reading a file
//! is the caller's.
//!
//! ```no_run
//! use iclforge::lyrics::Lyrics;
//!
//! let bytes = std::fs::read("song.lrc").unwrap();
//! let lyrics = Lyrics::parse(&bytes, None).unwrap();
//! for line in lyrics.lines() {
//!     println!("{:>7} ms  {}", line.start_ms(), line.text());
//! }
//! let at = lyrics.position(12_700, 0);
//! assert!(at.line.is_some());
//! ```

use iclforge_sys as sys;
use std::ffi::CStr;
use std::fmt;
use std::ptr;

use crate::error::Error;

/// Why [`Lyrics::parse`] refused its bytes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LyricsError {
    /// The bytes are not lyrics: empty, in an encoding that is not read (Latin-1, Windows-1255 and
    /// UTF-32 are named and not guessed at), or with no lyrics in them. The sentence says which.
    Unreadable(String),
    /// Any other status the C library returned.
    Other(Error),
}

impl fmt::Display for LyricsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LyricsError::Unreadable(reason) => write!(f, "{reason}"),
            LyricsError::Other(error) => write!(f, "{error}"),
        }
    }
}

impl std::error::Error for LyricsError {}

/// `iclforge_lyrics_singer_t` — which singer of a duet sings a line. `Unmarked` is a line that
/// says nothing, which a solo's every line is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum Singer {
    #[default]
    Unmarked,
    /// `V1:`
    First,
    /// `V2:`
    Second,
    /// `V1+V2:`
    Both,
}

impl Singer {
    fn from_raw(raw: sys::iclforge_lyrics_singer_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::iclforge_lyrics_singer_ICLFORGE_LYRICS_SINGER_FIRST => Singer::First,
            sys::iclforge_lyrics_singer_ICLFORGE_LYRICS_SINGER_SECOND => Singer::Second,
            sys::iclforge_lyrics_singer_ICLFORGE_LYRICS_SINGER_BOTH => Singer::Both,
            _ => Singer::Unmarked,
        }
    }
}

/// `iclforge_lyrics_word_state_t` — a word against a position.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum WordState {
    /// From the word's end.
    Sung,
    /// From the word's start.
    BeingSung,
    NotYetSung,
}

impl WordState {
    fn from_raw(raw: sys::iclforge_lyrics_word_state_t) -> Self {
        #[allow(non_upper_case_globals)]
        match raw {
            sys::iclforge_lyrics_word_state_ICLFORGE_LYRICS_WORD_SUNG => WordState::Sung,
            sys::iclforge_lyrics_word_state_ICLFORGE_LYRICS_WORD_BEING_SUNG => WordState::BeingSung,
            _ => WordState::NotYetSung,
        }
    }
}

/// `iclforge_lyrics_position_t` — where in the lyrics a playing position is.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LyricPosition {
    /// The line most recently started; `None` before the first.
    pub line: Option<usize>,
    /// Whether the position is before that line's end: false in the stretch after the last line.
    pub in_line: bool,
    /// The word being sung, when that line is word-timed and one word spans the position.
    pub word: Option<usize>,
    /// How far through that word, from 0 up to but not including 1, by time.
    pub fraction: f64,
    /// The position the answer is for: the one asked, less the delay.
    pub position_ms: i64,
}

/// A lyrics file read into lines of timed words. Immutable, so it is `Send` and `Sync`.
pub struct Lyrics {
    raw: ptr::NonNull<sys::iclforge_lyrics_t>,
}

// SAFETY: the C handle is never written after iclforge_lyrics_parse() returns, and every
// accessor only reads it (iclforge.h: "Reading it is safe from several threads at once").
unsafe impl Send for Lyrics {}
unsafe impl Sync for Lyrics {}

impl Lyrics {
    /// Reads the bytes of a lyrics file. `duration_ms` is the item's length, which the last
    /// line's end is taken from when the file does not say.
    pub fn parse(bytes: &[u8], duration_ms: Option<i64>) -> Result<Lyrics, LyricsError> {
        let mut out: *mut sys::iclforge_lyrics_t = ptr::null_mut();
        // SAFETY: `bytes` is a valid slice for the duration of this call; `out` is a valid
        // out-parameter, left untouched on failure.
        let status = unsafe {
            sys::iclforge_lyrics_parse(
                bytes.as_ptr(),
                bytes.len(),
                duration_ms.unwrap_or(-1),
                &mut out,
            )
        };
        if status == sys::iclforge_status_ICLFORGE_ERROR_LYRICS_UNREADABLE {
            // SAFETY: the message is library-owned storage for this thread, good until its next
            // parse, and is copied before anything else is called.
            let reason = unsafe { CStr::from_ptr(sys::iclforge_lyrics_error_message()) };
            return Err(LyricsError::Unreadable(
                reason.to_string_lossy().into_owned(),
            ));
        }
        Error::check(status).map_err(LyricsError::Other)?;
        let raw = ptr::NonNull::new(out)
            .expect("iclforge_lyrics_parse returned OK with a null lyrics handle");
        Ok(Lyrics { raw })
    }

    /// A string accessor's text. The reader hands back UTF-8 whatever the file's encoding was.
    fn text(&self, raw: *const std::os::raw::c_char) -> &str {
        if raw.is_null() {
            return "";
        }
        // SAFETY: library-owned, NUL-terminated, valid until the lyrics are destroyed - which
        // `&self` outlives; tied to that borrow here.
        unsafe { CStr::from_ptr(raw) }.to_str().unwrap_or("")
    }

    pub fn title(&self) -> &str {
        self.text(unsafe { sys::iclforge_lyrics_title(self.raw.as_ptr()) })
    }

    pub fn artist(&self) -> &str {
        self.text(unsafe { sys::iclforge_lyrics_artist(self.raw.as_ptr()) })
    }

    pub fn album(&self) -> &str {
        self.text(unsafe { sys::iclforge_lyrics_album(self.raw.as_ptr()) })
    }

    pub fn author(&self) -> &str {
        self.text(unsafe { sys::iclforge_lyrics_author(self.raw.as_ptr()) })
    }

    /// What the file calls the duet's first singer (`[v1:name]`); empty when it does not.
    pub fn first_singer_name(&self) -> &str {
        self.text(unsafe { sys::iclforge_lyrics_first_singer_name(self.raw.as_ptr()) })
    }

    /// What the file calls the duet's second singer (`[v2:name]`); empty when it does not.
    pub fn second_singer_name(&self) -> &str {
        self.text(unsafe { sys::iclforge_lyrics_second_singer_name(self.raw.as_ptr()) })
    }

    /// The file's `[length:]`, in milliseconds.
    pub fn length_ms(&self) -> Option<i64> {
        let mut length = 0i64;
        // SAFETY: `length` is a valid out-parameter, written only when the result is 1.
        let has = unsafe { sys::iclforge_lyrics_length_ms(self.raw.as_ptr(), &mut length) };
        (has != 0).then_some(length)
    }

    /// The file's `[offset:]`, which is already applied to every time.
    pub fn offset_ms(&self) -> i64 {
        unsafe { sys::iclforge_lyrics_offset_ms(self.raw.as_ptr()) }
    }

    /// False for plain text with no times at all: its lines are the words, in order, and are not
    /// followed.
    pub fn is_timed(&self) -> bool {
        unsafe { sys::iclforge_lyrics_timed(self.raw.as_ptr()) != 0 }
    }

    /// Whether any line has a singer: the file is a duet.
    pub fn has_singers(&self) -> bool {
        unsafe { sys::iclforge_lyrics_has_singers(self.raw.as_ptr()) != 0 }
    }

    pub fn line_count(&self) -> usize {
        unsafe { sys::iclforge_lyrics_line_count(self.raw.as_ptr()) }
    }

    /// The line at `index`, in time order; `None` past the last.
    pub fn line(&self, index: usize) -> Option<Line<'_>> {
        (index < self.line_count()).then_some(Line {
            lyrics: self,
            index,
        })
    }

    /// Every line, in time order.
    pub fn lines(&self) -> impl Iterator<Item = Line<'_>> {
        (0..self.line_count()).map(move |index| Line {
            lyrics: self,
            index,
        })
    }

    /// Where `position_ms` is in the lyrics. `delay_ms` is the singer's nudge: positive shows the
    /// words later, so the answer is for an earlier position.
    pub fn position(&self, position_ms: i64, delay_ms: i64) -> LyricPosition {
        let mut raw = sys::iclforge_lyrics_position_t::default();
        // SAFETY: `raw` is a valid out-parameter, written in full for a non-NULL lyrics.
        unsafe {
            sys::iclforge_lyrics_position(self.raw.as_ptr(), position_ms, delay_ms, &mut raw)
        };
        LyricPosition {
            line: (raw.has_line != 0).then_some(raw.line),
            in_line: raw.in_line != 0,
            word: (raw.has_word != 0).then_some(raw.word),
            fraction: raw.fraction,
            position_ms: raw.position_ms,
        }
    }

    /// A word against a position. A word that is not there is not yet sung.
    pub fn word_state(&self, line: usize, word: usize, position_ms: i64) -> WordState {
        WordState::from_raw(unsafe {
            sys::iclforge_lyrics_word_state(self.raw.as_ptr(), line, word, position_ms)
        })
    }
}

impl Drop for Lyrics {
    fn drop(&mut self) {
        // SAFETY: `self.raw` is owned exclusively by this `Lyrics`.
        unsafe { sys::iclforge_lyrics_destroy(self.raw.as_ptr()) };
    }
}

impl fmt::Debug for Lyrics {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Lyrics")
            .field("title", &self.title())
            .field("lines", &self.line_count())
            .field("timed", &self.is_timed())
            .finish()
    }
}

/// One line of a [`Lyrics`]. Borrows it.
#[derive(Clone, Copy)]
pub struct Line<'a> {
    lyrics: &'a Lyrics,
    index: usize,
}

impl<'a> Line<'a> {
    /// This line's position among the lyrics' lines.
    pub fn index(&self) -> usize {
        self.index
    }

    pub fn start_ms(&self) -> i64 {
        unsafe { sys::iclforge_lyrics_line_start_ms(self.lyrics.raw.as_ptr(), self.index) }
    }

    /// The next line's start; for the last line the item's length, the file's `[length:]` or five
    /// seconds after it starts.
    pub fn end_ms(&self) -> i64 {
        unsafe { sys::iclforge_lyrics_line_end_ms(self.lyrics.raw.as_ptr(), self.index) }
    }

    /// The line without its tags; empty for a line that only marks a time.
    pub fn text(&self) -> &'a str {
        self.lyrics
            .text(unsafe { sys::iclforge_lyrics_line_text(self.lyrics.raw.as_ptr(), self.index) })
    }

    pub fn singer(&self) -> Singer {
        Singer::from_raw(unsafe {
            sys::iclforge_lyrics_line_singer(self.lyrics.raw.as_ptr(), self.index)
        })
    }

    /// Zero for a line with times and no word tags: no word times are invented for it.
    pub fn word_count(&self) -> usize {
        unsafe { sys::iclforge_lyrics_line_word_count(self.lyrics.raw.as_ptr(), self.index) }
    }

    pub fn word(&self, word: usize) -> Option<Word<'a>> {
        (word < self.word_count()).then_some(Word {
            lyrics: self.lyrics,
            line: self.index,
            index: word,
        })
    }

    pub fn words(&self) -> impl Iterator<Item = Word<'a>> + 'a {
        let lyrics = self.lyrics;
        let line = self.index;
        (0..self.word_count()).map(move |index| Word {
            lyrics,
            line,
            index,
        })
    }
}

/// One word of a [`Line`]. Its text keeps the spacing written after it, so a line's words joined
/// are its text.
#[derive(Clone, Copy)]
pub struct Word<'a> {
    lyrics: &'a Lyrics,
    line: usize,
    index: usize,
}

impl<'a> Word<'a> {
    pub fn start_ms(&self) -> i64 {
        unsafe {
            sys::iclforge_lyrics_word_start_ms(self.lyrics.raw.as_ptr(), self.line, self.index)
        }
    }

    pub fn end_ms(&self) -> i64 {
        unsafe { sys::iclforge_lyrics_word_end_ms(self.lyrics.raw.as_ptr(), self.line, self.index) }
    }

    pub fn text(&self) -> &'a str {
        self.lyrics.text(unsafe {
            sys::iclforge_lyrics_word_text(self.lyrics.raw.as_ptr(), self.line, self.index)
        })
    }

    /// This word against `position_ms`.
    pub fn state(&self, position_ms: i64) -> WordState {
        self.lyrics.word_state(self.line, self.index, position_ms)
    }
}
