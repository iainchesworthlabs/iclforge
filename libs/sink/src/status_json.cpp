#include "iclforge/sink/status_json.hpp"

#include <string_view>

#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sink/lyric_frame.hpp"

namespace iclforge::sink {

namespace {

[[nodiscard]] std::string_view activity_name(Sink::Status::Activity activity) {
    switch (activity) {
        case Sink::Status::Activity::kPlayback:
            return "playback";
        case Sink::Status::Activity::kPairing:
            return "pairing";
        case Sink::Status::Activity::kNothing:
            break;
    }
    return "nothing";
}

[[nodiscard]] std::string decoder_json(const std::optional<sendspin::player::DecoderReport>& decoder) {
    if (!decoder) {
        return "null";
    }
    std::string out = "{\"data_type\":";
    out += json_quote(sendspin::player::data_type_name(decoder->data_type));
    out += ",\"acmod\":" + std::to_string(decoder->acmod);
    out += std::string(",\"lfe\":") + (decoder->lfe ? "true" : "false");
    out += ",\"objects\":" + std::to_string(decoder->objects);
    out += std::string(",\"objects_placed\":") + (decoder->objects_placed ? "true" : "false");
    out += "}";
    return out;
}

}  // namespace

std::string status_json(const Sink::Status& status) {
    std::string out = "{\"connections\":[";
    for (std::size_t i = 0; i < status.connections.size(); ++i) {
        const Sink::Status::Peer& peer = status.connections[i];
        if (i != 0) {
            out.push_back(',');
        }
        // "passthrough" or "pcm" while a stream the output took is open, null otherwise.
        const std::string output =
            peer.output ? json_quote(sendspin::player::output_name(*peer.output)) : std::string("null");
        out += "{\"id\":" + std::to_string(peer.id) + ",\"address\":" + json_quote(peer.address) +
               ",\"server_name\":" + json_quote(peer.server_name) + ",\"activity\":\"" +
               std::string(activity_name(peer.activity)) + "\",\"streaming\":" + (peer.streaming ? "true" : "false") +
               ",\"output\":" + output + ",\"decoder\":" + decoder_json(peer.decoder) + "}";
    }
    out += "],\"pairing_code\":";
    out += status.pairing_code ? json_quote(*status.pairing_code) : "null";
    out += ",\"paired_servers\":" + std::to_string(status.paired_servers) + "}";
    return out;
}

}  // namespace iclforge::sink
