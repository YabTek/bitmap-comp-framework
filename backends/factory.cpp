#include "backends/factory.hpp"
#include "backends/raw_backend.hpp"
#include "backends/roaring_backend.hpp"
#include "backends/stream_backend.hpp"
#include "backends/wah_backend.hpp"

namespace bench {
const std::vector<std::string> &backend_names() {
    static const std::vector<std::string> names{"raw",   "wah", "ewah", "roaring", "concise",
                                                "plwah", "val", "sbh",  "bbc"};
    return names;
}
std::unique_ptr<BitmapBackend> make_backend(const std::string &name, CodecOptions options) {
    if (name == "raw")
        return std::make_unique<RawBackend>();
    if (name == "wah")
        return std::make_unique<WahBackend>();
    if (name == "roaring")
        return std::make_unique<RoaringBackend>();
    for (Codec c : {Codec::Ewah, Codec::Concise, Codec::Plwah, Codec::Val, Codec::Sbh, Codec::Bbc})
        if (name == codec_name(c))
            return std::make_unique<StreamBackend>(c, options);
    throw std::runtime_error("unknown backend: " + name);
}
} // namespace bench
