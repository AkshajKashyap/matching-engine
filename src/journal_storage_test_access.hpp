#pragma once

#include <cstddef>

namespace matching::testing {

// Source-tree-only deterministic append-path I/O controls. They deliberately
// exclude journal creation, recovery, and directory durability barriers.
class JournalStorageTestAccess {
public:
    static void reset_append_io();
    static void fail_append_write_after(std::size_t successful_bytes);
    static void fail_next_append_sync();
    [[nodiscard]] static std::size_t successful_append_sync_count();
};

} // namespace matching::testing
