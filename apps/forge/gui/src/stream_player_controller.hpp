#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "iclforge/ac3/analysis/levels.hpp"
#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "background_jobs.hpp"
#include "cdg_frame_source.hpp"
#include "cdg_frames.hpp"
#include "truehd_pcm_source.hpp"

namespace iclforge::audio {
class MonitorSink;
}

// The decoded-in-memory result of one openFile() pass, kept OUTSIDE
// StreamPlayerController itself for the same reason objdec_detail::RawResult
// is (see object_decode_controller.hpp's own comment): the free decode
// helper in stream_player_controller.cpp can build one without member
// access, and it is reference-counted (shared_ptr, not a plain member) so a
// playback worker mid-flight on its own thread keeps its OWN handle on the
// result it started with even if openFile() replaces the controller's
// result_ with a new one for a second file - see play()'s own comment in
// stream_player_controller.cpp on why a raw reference into result_ would be
// a dangling-reference hazard here that objdec_detail::RawResult never had
// to consider (nothing there is read from a background thread after the
// decode itself finishes).
namespace splayer_detail {

struct RawResult {
    QString codec_label;              // "AC-3" or "E-AC-3"
    QString layout_label;             // iclforge::ac3::analysis::layout_name(acmod, lfe)
    std::uint32_t sample_rate_hz = 0;
    iclforge::ac3::Acmod acmod = iclforge::ac3::Acmod::k2_0;
    bool lfe = false;
    std::uint64_t frame_count = 0;    // samples per channel, whole file
    std::uint64_t unit_count = 0;     // syncframes/access units decoded
    // "programme 0 of 2 (0, 1)" for an E-AC-3 stream that carries more than one
    // independent substream - the programme that was decoded and what else was
    // there, as forge's own commands say it; empty for every other stream.
    QString programme_note;
    double duration_seconds = 0.0;
    bool has_objects = false;
    int object_count = 0;
    // Planar bed audio, one vector per channel, already reordered to
    // monitor/WAV playback order (iclforge::ac3::io::wav_channel_order /
    // iclforge::ac3::plan::monitor_order) - channels[i] is playback position i's
    // whole-file audio, ready to hand straight to write_wav_f32 for export.
    std::vector<std::vector<float>> channels;
    // Location per channel, parallel to `channels` above - drives
    // channelMeta()/the soundfield ring's geometry.
    std::vector<iclforge::ac3::eac3::chanmap::Location> locations;
    // One mono buffer per JOC-reconstructed object, present only for an
    // Atmos E-AC-3 stream - export only (exportObjects()), never shown as a
    // QVariantList property the way the bed's channels are.
    std::vector<std::vector<float>> object_audio;
    // AC-4: the table of contents' presentations by their labels
    // (forge_gui::ac4_presentation_label), and the one decoded.
    bool ac4 = false;
    QStringList presentations;
    std::size_t presentation = 0;
    // Karaoke (Annex C, E-AC-3 by analogy): the stream is bsmod 7 with a mode above 1/0,
    // whether the decode applied the karaoke reproduction (so `acmod` above
    // is the layout it came out in, not the coded one), and the layout as
    // coded for the page to say beside it.
    bool karaoke = false;
    bool karaoke_reproduced = false;
    QString coded_layout_label;
    // TrueHD or MLP: the audio is not held here. `channels` stays empty and the player reads it a
    // window at a time from this source (row U2 of planning/truehd-parity-forge.md); `locations`
    // and the counts above are filled as for any other stream.
    std::shared_ptr<forge_gui::TruehdPcmSource> truehd;
    [[nodiscard]] std::size_t channel_total() const noexcept {
        return truehd ? static_cast<std::size_t>(truehd->info().channels) : channels.size();
    }
};

}  // namespace splayer_detail

// The GUI twin of `forge monitor` plus `forge decode [objects_dir]`: opens
// an already-encoded .ac3/.ec3 file, plays its decoded bed through an
// ordinary shared-mode output with real transport (play/pause/seek), and can
// export the decode to a WAV (and, for an Atmos stream, one WAV per
// JOC-reconstructed object) without ever going through the CLI. Reuses the
// MonitorSink plumbing ObjectDecodeController's audition_sink_ and
// EncoderController's motion-preview/live-monitor sinks already established
// (open a shared-mode sink, submit interleaved chunks from a worker thread
// with a 4ms retry while the queue is full) rather than inventing a second
// way to reach an audio device.
//
// Deliberately its own controller, not folded into ObjectDecodeController or
// EncoderController, for the reason both of their own header comments
// already give for staying separate from each other: this opens an
// ALREADY-ENCODED file with no plan, no source and no encoder involved, and
// it is neither an object-metadata report (ObjectDecodeController) nor a
// loudness measurement (QcController) - just playback and a decode-to-file
// export, the GUI's third "distinct surface, reachable from the header"
// dialog alongside QC and Inspect objects.
//
// Whole-file decode to memory, like ObjectDecodeController::inspectFile -
// not the CLI's incremental object-WAV-per-access-unit streaming write -
// because a real seek needs the samples already resident, and every other
// interactive GUI decode dialog in this window already accepts the same
// trade for the same reason. A feature-length programme is a lot of RAM this
// way; nothing here pretends otherwise.
//
// A `name.cdg` beside the stream (a karaoke disc's picture, MP3+G's other
// half) is held beside the decoded bed and given to the dialog's CdgPicture
// through the CdgFrameSource this is (apps/shared/cdg-picture), as Hearth's
// KaraokeController gives it to the lyric view: the picture is a function of
// the position of what is HEARD, which is pictureSeconds(), the position of
// what has been submitted less what the sink has not yet played.
class StreamPlayerController : public iclforge::apps::CdgFrameSource {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // The file last handed to openFile(), even while busy decoding it or
    // after a failed attempt - so the dialog can always say what it is
    // playing.
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
    Q_PROPERTY(QString error READ error NOTIFY resultChanged)
    // "E-AC-3 · 3/2 + LFE · 48000 Hz · 62 frame(s) · 1.98 s"
    Q_PROPERTY(QString summaryLine READ summaryLine NOTIFY resultChanged)
    Q_PROPERTY(bool hasObjects READ hasObjects NOTIFY resultChanged)
    Q_PROPERTY(int objectCount READ objectCount NOTIFY resultChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY resultChanged)
    // channelMeta's directionality/ceiling split - see SoundfieldView.qml's
    // own atmosEnabled read, reused verbatim ("objects panned in" vs a bare
    // per-speaker fed caption).
    Q_PROPERTY(bool atmosEnabled READ atmosEnabled NOTIFY resultChanged)

    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY positionChanged)

    // The picture of a CD+G disc beside the stream: whether the file beside it
    // is one (a `name.cdg` of the stream's name, in any case) and could be
    // read, its file name, why it could not be, and the position the picture
    // is drawn at. The picture itself is what pictureFrame() answers.
    Q_PROPERTY(bool hasPicture READ hasPicture NOTIFY pictureStateChanged)
    Q_PROPERTY(QString pictureFileName READ pictureFileName NOTIFY pictureStateChanged)
    Q_PROPERTY(QString pictureError READ pictureError NOTIFY pictureStateChanged)
    Q_PROPERTY(double pictureSeconds READ pictureSeconds NOTIFY positionChanged)

    // Same shapes as EncoderController's own channelLevels/channelMeta/
    // soundfield/meterFloorDb - see publishLevels()'s own comment - so
    // ChannelMeter.qml/SoundfieldView.qml read either controller identically
    // once given a `controller` property to address it by.
    Q_PROPERTY(QVariantList channelLevels READ channelLevels NOTIFY levelsChanged)
    Q_PROPERTY(QVariantList channelMeta READ channelMeta NOTIFY resultChanged)
    Q_PROPERTY(QVariantMap soundfield READ soundfield NOTIFY levelsChanged)
    Q_PROPERTY(double meterFloorDb READ meterFloorDb CONSTANT)

    Q_PROPERTY(bool exporting READ exporting NOTIFY exportingChanged)
    Q_PROPERTY(QString exportError READ exportError NOTIFY exportFinished)

    // AC-4, decoded through iclforge::ac4::Decoder as `forge play` and Hearth's engine
    // decode it: whether the open file is AC-4, its presentations, and which
    // one plays - `presentation=<n>`'s n, or -1 for the decoder's own choice.
    // Setting it decodes the file again.
    Q_PROPERTY(bool isAc4 READ isAc4 NOTIFY resultChanged)
    Q_PROPERTY(QStringList presentationNames READ presentationNames NOTIFY resultChanged)
    Q_PROPERTY(int presentationIndex READ presentationIndex WRITE setPresentationIndex NOTIFY
                   presentationChanged)
    Q_PROPERTY(int decodedPresentation READ decodedPresentation NOTIFY resultChanged)

    // Annex C's karaoke (ETSI TS 102 366): how an AC-3 karaoke stream is
    // reproduced - "asCoded" (the channels as the stream codes them, the
    // default and what this player has always done), "aware" (L, C and R with
    // the vocals at the stream's own level, Table C.2.2) or "capable" (the
    // listener's choice of vocals, Table C.2.3) - and which vocals "capable"
    // plays: "none", "v1", "v2" or "both". Setting either decodes the open
    // file again, when it is a karaoke stream: nothing else is changed by
    // them. The Karaoke flow's Hear step is what sets them.
    Q_PROPERTY(QString karaokeReproduction READ karaokeReproduction WRITE setKaraokeReproduction NOTIFY
                   karaokeChanged)
    Q_PROPERTY(QString karaokeVocals READ karaokeVocals WRITE setKaraokeVocals NOTIFY karaokeChanged)
    // Annex C.2.3's "additional flexibility", the capable decoder's alone: the level of the guide
    // melody and of each vocal against Table C.2.3, in dB - 0 is the table bit for bit, the span
    // is iclforge::ac3::kKaraokeMinLevelDb (-24, which is off) to kKaraokeMaxLevelDb (12), the one
    // Hearth's sliders, `forge decode karaoke-melody=` and a sink's decoder use. A value outside it
    // is held to it. Setting one decodes the open file again, when it is a karaoke stream and the
    // reproduction is "capable": under the others the library ignores them.
    Q_PROPERTY(double karaokeMelodyDb READ karaokeMelodyDb WRITE setKaraokeMelodyDb NOTIFY
                   karaokeChanged)
    Q_PROPERTY(double karaokeVocal1Db READ karaokeVocal1Db WRITE setKaraokeVocal1Db NOTIFY
                   karaokeChanged)
    Q_PROPERTY(double karaokeVocal2Db READ karaokeVocal2Db WRITE setKaraokeVocal2Db NOTIFY
                   karaokeChanged)
    // The open file is an AC-3 karaoke stream; the decode applied a
    // reproduction (so its channels are L, C, R and not the coded ones); and
    // the layout the stream codes.
    Q_PROPERTY(bool isKaraoke READ isKaraoke NOTIFY resultChanged)
    Q_PROPERTY(bool karaokeReproduced READ karaokeReproduced NOTIFY resultChanged)
    Q_PROPERTY(QString codedLayoutLabel READ codedLayoutLabel NOTIFY resultChanged)

   public:
    explicit StreamPlayerController(QObject* parent = nullptr);
    ~StreamPlayerController() override;

    [[nodiscard]] QString filePath() const { return file_path_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool hasResult() const { return result_ != nullptr; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QString summaryLine() const;
    [[nodiscard]] bool hasObjects() const { return result_ && result_->has_objects; }
    [[nodiscard]] int objectCount() const { return result_ ? result_->object_count : 0; }
    [[nodiscard]] double durationSeconds() const { return result_ ? result_->duration_seconds : 0.0; }
    [[nodiscard]] bool atmosEnabled() const { return result_ && result_->has_objects; }

    [[nodiscard]] bool playing() const { return playing_; }
    [[nodiscard]] double positionSeconds() const;

    [[nodiscard]] bool hasPicture() const { return picture_.loaded(); }
    [[nodiscard]] QString pictureFileName() const { return picture_file_name_; }
    [[nodiscard]] QString pictureError() const { return picture_error_; }
    // Where the picture is drawn: positionSeconds() less what the output has
    // been handed and not yet played while playing (the device's own buffer
    // and this sink's queue), so that it follows what is heard. A pause and a
    // seek are exact: nothing is in flight.
    [[nodiscard]] double pictureSeconds() const;

    // CdgFrameSource: the disc beside the open stream at `positionMs`. There
    // is one disc here, so `key` is not read; a null image where there is none.
    [[nodiscard]] QImage pictureFrame(int key, qlonglong positionMs) override;

    [[nodiscard]] QVariantList channelLevels() const { return channel_levels_; }
    [[nodiscard]] QVariantList channelMeta() const;
    [[nodiscard]] QVariantMap soundfield() const { return soundfield_; }
    [[nodiscard]] double meterFloorDb() const;

    [[nodiscard]] bool exporting() const { return exporting_; }
    [[nodiscard]] QString exportError() const { return export_error_; }

    [[nodiscard]] bool isAc4() const { return result_ && result_->ac4; }
    [[nodiscard]] QStringList presentationNames() const {
        return result_ ? result_->presentations : QStringList{};
    }
    [[nodiscard]] int presentationIndex() const { return presentation_index_; }
    void setPresentationIndex(int index);
    [[nodiscard]] int decodedPresentation() const {
        return result_ && result_->ac4 ? static_cast<int>(result_->presentation) : -1;
    }

    [[nodiscard]] QString karaokeReproduction() const { return karaoke_reproduction_; }
    void setKaraokeReproduction(const QString& name);
    [[nodiscard]] QString karaokeVocals() const { return karaoke_vocals_; }
    void setKaraokeVocals(const QString& name);
    [[nodiscard]] double karaokeMelodyDb() const { return karaoke_melody_db_; }
    void setKaraokeMelodyDb(double db);
    [[nodiscard]] double karaokeVocal1Db() const { return karaoke_vocal1_db_; }
    void setKaraokeVocal1Db(double db);
    [[nodiscard]] double karaokeVocal2Db() const { return karaoke_vocal2_db_; }
    void setKaraokeVocal2Db(double db);
    // All three back to 0 dB, which decodes the open file once and not three times.
    Q_INVOKABLE void resetKaraokeLevels();
    [[nodiscard]] bool isKaraoke() const { return result_ && result_->karaoke; }
    [[nodiscard]] bool karaokeReproduced() const { return result_ && result_->karaoke_reproduced; }
    [[nodiscard]] QString codedLayoutLabel() const {
        return result_ ? result_->coded_layout_label : QString();
    }

    // Reads and decodes `url` off the GUI thread (QtConcurrent, mirroring
    // ObjectDecodeController::inspectFile/QcController::measureFile) into
    // the whole-file planar buffers above. Stops any playback of a
    // previously-open file first. Refused while already busy.
    Q_INVOKABLE void openFile(const QUrl& url);

    // Opens the monitor sink if needed and resumes from positionSeconds().
    // A no-op while a previous play()/pause() cycle's worker has not yet
    // finished tearing itself down - see the .cpp's own comment on
    // worker_active_.
    Q_INVOKABLE void play();
    // Stops submitting audio, leaving positionSeconds() where it was so a
    // later play() resumes from the same point.
    Q_INVOKABLE void pause();
    // Moves the playback position. Safe whether or not playback is active.
    Q_INVOKABLE void seek(double seconds);

    Q_INVOKABLE void clearClipLatch(int channel);

    // Writes the whole decoded bed to `url` as a float32 WAV
    // (iclforge::ac3::io::write_wav_f32) - the GUI twin of `forge decode`'s primary
    // output, from data already resident rather than a second decode pass.
    Q_INVOKABLE void exportDecodedWav(const QUrl& url);
    // Writes one object_NN.wav per JOC-reconstructed object into the
    // directory `url` names, the same naming `forge decode`'s own
    // objects_dir writes - a no-op (with exportError set) when hasObjects()
    // is false.
    Q_INVOKABLE void exportObjects(const QUrl& url);

   signals:
    void filePathChanged();
    void busyChanged();
    void resultChanged();
    void playingChanged();
    void positionChanged();
    void levelsChanged();
    void exportingChanged();
    void exportFinished();
    void presentationChanged();
    void karaokeChanged();
    void pictureStateChanged();

   private:
    void decodePath(const QString& path);
    // Reads the `.cdg` beside `stream_path`, or forgets the last one: the
    // picture of an item is the file beside it, found when it is opened.
    void loadPicture(const QString& stream_path);
    // Decodes the open file again when the karaoke choice changed what it
    // decodes to: only a karaoke AC-3 stream is.
    void redecodeForKaraoke();
    int presentation_index_ = -1;
    QString karaoke_reproduction_ = QStringLiteral("asCoded");
    QString karaoke_vocals_ = QStringLiteral("both");
    double karaoke_melody_db_ = 0.0;
    double karaoke_vocal1_db_ = 0.0;
    double karaoke_vocal2_db_ = 0.0;
    // A choice made while a decode was running: decoded again, with the latest
    // choice, when that one lands.
    bool karaoke_redecode_pending_ = false;

    // `source` is always the worker's own captured shared_ptr, never read
    // from result_ directly - see stream_player_controller.cpp's own
    // comment on why a superseded worker (openFile() loaded a second file
    // while this one was still mid-flight) must never let its OWN geometry
    // leak into whatever result_ points to by the time this runs.
    void publishLevels(const splayer_detail::RawResult& source,
                       std::span<const iclforge::ac3::analysis::ChannelLevel> levels);

    QString file_path_;
    bool busy_ = false;
    QString error_;
    std::shared_ptr<const splayer_detail::RawResult> result_;

    bool playing_ = false;
    std::unique_ptr<iclforge::audio::MonitorSink> sink_;
    bool worker_active_ = false;
    std::atomic<bool> should_play_{false};
    std::atomic<std::uint64_t> read_frame_{0};
    std::atomic<bool> seek_pending_{false};
    std::atomic<std::uint64_t> seek_target_{0};
    // Frames handed to the sink and not yet heard (its queue and the device's
    // buffer), as of the last publish while playing; 0 otherwise.
    std::atomic<std::uint64_t> lead_frames_{0};

    iclforge::apps::CdgFrames picture_;
    QString picture_file_name_;
    QString picture_error_;

    QVariantList channel_levels_;
    QVariantMap soundfield_;
    std::vector<bool> clip_latched_;

    bool exporting_ = false;
    QString export_error_;

    // Where the decode, the playback loop and the exports run.
    // ~StreamPlayerController() stops the loop and waits for all of them before
    // any member above is destroyed; declared last, so ~BackgroundJobs() would
    // wait ahead of them too.
    forge_gui::BackgroundJobs jobs_;
};
