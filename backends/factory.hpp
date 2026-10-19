#pragma once
#include "backends/bitmap_backend.hpp"
#include "backends/codecs.hpp"
#include <string>

namespace bench {
const std::vector<std::string> &backend_names();
std::unique_ptr<BitmapBackend> make_backend(const std::string &name, CodecOptions options = {});
} // namespace bench
