#!/usr/bin/env python3
"""Static guard for the compose-only direct-stick integration boundary."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ROUTER = (
    ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
    "makcm_core" / "endpoint_router.cpp"
)
RUNTIME = (
    ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "components" /
    "makcm_core" / "stick_runtime.cpp"
)


def ordered(text: str, fragments: list[str]) -> None:
    cursor = 0
    for fragment in fragments:
        position = text.find(fragment, cursor)
        if position < 0:
            raise AssertionError(f"missing/out-of-order boundary: {fragment}")
        cursor = position + len(fragment)


def main() -> int:
    router = ROUTER.read_text(encoding="utf-8")
    runtime = RUNTIME.read_text(encoding="utf-8")

    # Generic non-mouse input remains an independent per-endpoint ordered FIFO.
    ordered(router, [
        "if (endpoint->fifo_count >= kQueueDepth)",
        "metrics_->add(MetricId::EndpointDrop)",
        "const uint8_t slot = endpoint->fifo_tail",
        "std::memcpy(endpoint->fifo[slot], data, length)",
        "endpoint->fifo_tail = static_cast<uint8_t>((slot + 1) % kQueueDepth)",
        "++endpoint->fifo_count",
    ])

    # Overlay is applied only after copying the current FIFO head to compose;
    # the submitted buffer is then copied from compose, never from a mutation
    # of the FIFO slot itself.
    ordered(router, [
        "const uint8_t slot = endpoint.fifo_head",
        "std::memcpy(endpoint.compose, endpoint.fifo[slot], length)",
        "stick_runtime_->compose(",
        "endpoint.address, endpoint.fifo[slot], endpoint.compose, length",
        "std::memcpy(endpoint.inflight, endpoint.compose, endpoint.inflight_length)",
    ])

    # Retirement semantics remain completion-owned and success-only.
    ordered(router, [
        "case Source::Fifo:",
        "if (success && endpoint.fifo_count != 0)",
        "endpoint.fifo_head = static_cast<uint8_t>(",
        "--endpoint.fifo_count",
    ])
    assert router.count("stick_runtime_->compose(") == 1
    assert "fifo_head" not in runtime and "fifo_tail" not in runtime
    assert "latest-wins" not in router.lower()
    # Router state remains serialized in task context, but report composition
    # and TinyUSB submission must never execute with interrupts disabled.
    assert "xSemaphoreCreateRecursiveMutexStatic" in router
    assert "xSemaphoreTakeRecursive" in router
    assert "portENTER_CRITICAL(&mux_)" not in router
    constructor = router.index("EndpointRouter::EndpointRouter()")
    initializer = router.index("initializeMutex();", constructor)
    bind_entry = router.index("void EndpointRouter::bindStickPhysical")
    assert constructor < initializer < bind_entry
    print("PASS: compose-only overlay preserves ordered per-endpoint FIFO boundary")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
