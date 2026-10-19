#pragma once
#include <cstdint>
namespace bench {
// Counts of stored units, not original bits or groups. Zero means absent or
// not applicable. See docs/measurements.md for the per-format interpretation.
struct RepresentationStats {
    uint64_t literal_units = 0, fill_units = 0, exception_units = 0, control_units = 0;
    uint64_t array_containers = 0, bitmap_containers = 0, run_containers = 0;
    RepresentationStats &operator+=(const RepresentationStats &b) {
        literal_units += b.literal_units;
        fill_units += b.fill_units;
        exception_units += b.exception_units;
        control_units += b.control_units;
        array_containers += b.array_containers;
        bitmap_containers += b.bitmap_containers;
        run_containers += b.run_containers;
        return *this;
    }
};
} // namespace bench
