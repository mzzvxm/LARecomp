#pragma once

namespace mcla::gpu {

// Probe the driver only when a process exit can leave the device for OS cleanup.
template <typename ProbeDeviceLoss>
constexpr bool ShouldSkipDeviceTeardown(bool enabled, bool fast_exit, bool diagnostics,
                                        ProbeDeviceLoss probe_device_loss) {
  return enabled && fast_exit && !diagnostics && !probe_device_loss();
}

}  // namespace mcla::gpu
