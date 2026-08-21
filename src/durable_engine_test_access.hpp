#pragma once

#include <cstddef>
#include <functional>

namespace matching::testing {

// Source-tree-only hook for testing the boundary after WAL sync and before an
// individual in-memory application. The hook is never configured in production.
class DurableEngineTestAccess {
public:
    static void set_before_batch_apply_hook(std::function<void(std::size_t)> hook);
    static void clear_before_batch_apply_hook();
};

} // namespace matching::testing
