#pragma once

#include <QObject>
#include <QString>

#include "iclforge/sendspin/iclforge_player.hpp"
#include "network_view.hpp"

// What the Network page's "What the sink reports" panel says of how a sink's stream leaves it
// (planning/hearth-android.md, D6): the extension role's `output` state, which a sink that passes the
// coded bits on to a receiver (Hearth for Android) says "passthrough" and one that decodes says "pcm".
//
// Here, with no QObject of its own, as sink_lyrics_text.hpp is, so that a test reads what each state
// says.

namespace iclforge::hearth::ui {

// Empty while the sink has no stream open (the page shows its dash), "not reported" for a stream the
// sink did not say how it plays, which is every sink from before the key.
[[nodiscard]] inline QString sink_output_text(const iclforge::hearth::SinkFacts& facts) {
    if (!facts.iclforge_state) {
        return {};
    }
    if (facts.iclforge_state->output) {
        return *facts.iclforge_state->output == iclforge::sendspin::player::Output::kPassthrough
                   ? QObject::tr("passed through as coded")
                   : QObject::tr("decoded by the sink");
    }
    return facts.iclforge_state->decoder ? QObject::tr("not reported") : QString();
}

}  // namespace iclforge::hearth::ui
