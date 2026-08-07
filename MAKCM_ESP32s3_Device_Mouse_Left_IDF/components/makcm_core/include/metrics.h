#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace makcm::native {

enum class MetricId : uint8_t {
    BridgeRxFrame,
    BridgeRxError,
    BridgeTxError,
    ProfileReject,
    EndpointReady,
    EndpointSubmit,
    EndpointComplete,
    EndpointStale,
    EndpointRetry,
    EndpointDrop,
    ControlDeferred,
    ControlTimeout,
    OutRearm,
    RemoteWake,
    Count,
};

struct MetricsSnapshot {
    uint32_t bridge_rx_frame = 0;
    uint32_t bridge_rx_error = 0;
    uint32_t bridge_tx_error = 0;
    uint32_t profile_reject = 0;
    uint32_t endpoint_ready = 0;
    uint32_t endpoint_submit = 0;
    uint32_t endpoint_complete = 0;
    uint32_t endpoint_stale = 0;
    uint32_t endpoint_retry = 0;
    uint32_t endpoint_drop = 0;
    uint32_t control_deferred = 0;
    uint32_t control_timeout = 0;
    uint32_t out_rearm = 0;
    uint32_t remote_wake = 0;
};

class Metrics final {
public:
    void reset();
    void add(MetricId id, uint32_t amount = 1);
    uint32_t get(MetricId id) const;
    MetricsSnapshot snapshot() const;

private:
    std::atomic<uint32_t> values_[static_cast<size_t>(MetricId::Count)]{};
};

} // namespace makcm::native
