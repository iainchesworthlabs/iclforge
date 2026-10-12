#pragma once

#include <QObject>
#include <QString>

#include <cstdint>

#include "network_view.hpp"

// What the Network page's "What the sink reports" panel says of a sink's lyrics (planning/karaoke-
// phase2.md, W7d): whether the sink takes them, whether it holds the item playing's, and where in
// them its own clock has the sound. The sink reports the item and the line's index; the text and
// the count are this computer's reading of the document it sent (SinkFacts::lyrics_*).
//
// Here, with no QObject of its own, so that a test reads what each state says.

namespace iclforge::hearth::ui {

[[nodiscard]] inline QString sink_lyrics_text(const iclforge::hearth::SinkFacts& facts) {
    if (!facts.iclforge_support || facts.iclforge_support->lyrics_max_bytes == 0) {
        return QObject::tr("not offered");
    }
    if (!facts.iclforge_state || !facts.iclforge_state->lyrics) {
        return QObject::tr("none held");
    }
    const std::int32_t line = facts.iclforge_state->lyrics->line;
    if (line < 0) {
        return QObject::tr("held · before the first line");
    }
    QString text = facts.lyrics_lines ? QObject::tr("line %1 of %2").arg(line + 1).arg(*facts.lyrics_lines)
                                      : QObject::tr("line %1").arg(line + 1);
    if (!facts.lyrics_line_text.empty()) {
        text += QStringLiteral(" · ") + QString::fromStdString(facts.lyrics_line_text);
    }
    return text;
}

}  // namespace iclforge::hearth::ui
