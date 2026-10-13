#include "sink.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "burst_output.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "wav_output.hpp"

namespace iclforge::hearth::testsink {

// Each connection's outputs, files in the output directory named by the connection's number.
class Sink::FileOutputs final : public sink::Outputs {
   public:
    FileOutputs(std::filesystem::path directory, const render::OutputLayout& layout)
        : directory_(std::move(directory)), layout_(layout) {}

    [[nodiscard]] std::unique_ptr<sink::StreamOutput> stream(std::uint64_t connection) override {
        return std::make_unique<WavOutput>(directory_, "stream-" + std::to_string(connection));
    }
    [[nodiscard]] std::unique_ptr<sink::BurstStreamOutput> bursts(std::uint64_t connection) override {
        return std::make_unique<BurstOutput>(directory_, "bursts-" + std::to_string(connection), layout_);
    }

   private:
    std::filesystem::path directory_;
    render::OutputLayout layout_;
};

std::expected<std::unique_ptr<Sink>, std::string> Sink::start(SinkOptions options, SinkLog& log) {
    const std::optional<render::OutputLayout> layout = render::OutputLayout::parse(options.layout);
    if (!layout) {
        return std::unexpected("not a speaker layout: " + options.layout);
    }
    if (!options.output_directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(options.output_directory, error);
        if (error) {
            return std::unexpected("cannot create " + options.output_directory.string());
        }
    }
    // What the support and state objects say the streams are rendered to.
    options.output_count = static_cast<std::int32_t>(layout->slots());
    options.crossover_hz = {render::LayoutRenderer::kMinCrossoverHz, render::LayoutRenderer::kMaxCrossoverHz};

    std::unique_ptr<Sink> sink(new Sink(std::make_unique<FileOutputs>(options.output_directory, *layout)));
    // The core takes the base: the output directory and the layout are this program's own, and are
    // not sliced off by a parameter of the base's type but named as the base being passed.
    std::expected<std::unique_ptr<iclforge::sink::Sink>, std::string> core =
        iclforge::sink::Sink::start(std::move(static_cast<iclforge::sink::Options&>(options)),
                                    *sink->outputs_, log);
    if (!core) {
        return std::unexpected(std::move(core.error()));
    }
    sink->core_ = std::move(*core);
    return sink;
}

Sink::Sink(std::unique_ptr<FileOutputs> outputs) : outputs_(std::move(outputs)) {}

Sink::~Sink() = default;

}  // namespace iclforge::hearth::testsink
