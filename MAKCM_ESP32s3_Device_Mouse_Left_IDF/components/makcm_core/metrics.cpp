#include "metrics.h"

namespace makcm::native {

void Metrics::reset() {
    for (auto &value : values_) value.store(0, std::memory_order_relaxed);
}

void Metrics::add(MetricId id, uint32_t amount) {
    const size_t index = static_cast<size_t>(id);
    if (index < static_cast<size_t>(MetricId::Count))
        values_[index].fetch_add(amount, std::memory_order_relaxed);
}

uint32_t Metrics::get(MetricId id) const {
    const size_t index = static_cast<size_t>(id);
    return index < static_cast<size_t>(MetricId::Count)
               ? values_[index].load(std::memory_order_relaxed)
               : 0;
}

MetricsSnapshot Metrics::snapshot() const {
    MetricsSnapshot result;
    result.bridge_rx_frame = get(MetricId::BridgeRxFrame);
    result.bridge_rx_error = get(MetricId::BridgeRxError);
    result.bridge_tx_error = get(MetricId::BridgeTxError);
    result.profile_reject = get(MetricId::ProfileReject);
    result.endpoint_ready = get(MetricId::EndpointReady);
    result.endpoint_submit = get(MetricId::EndpointSubmit);
    result.endpoint_complete = get(MetricId::EndpointComplete);
    result.endpoint_stale = get(MetricId::EndpointStale);
    result.endpoint_retry = get(MetricId::EndpointRetry);
    result.endpoint_drop = get(MetricId::EndpointDrop);
    result.control_deferred = get(MetricId::ControlDeferred);
    result.control_timeout = get(MetricId::ControlTimeout);
    result.out_rearm = get(MetricId::OutRearm);
    result.remote_wake = get(MetricId::RemoteWake);
    return result;
}

} // namespace makcm::native
