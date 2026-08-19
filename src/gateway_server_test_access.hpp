#pragma once

#include <functional>

namespace matching {

namespace testing {

class GatewayServerTestAccess {
public:
    // Invoked on the gateway thread immediately before draining a non-empty
    // engine response queue. Source-tree-only deterministic test control.
    static void set_before_response_drain_hook(std::function<void()> hook);
    static void clear_before_response_drain_hook();
};

} // namespace testing
} // namespace matching
