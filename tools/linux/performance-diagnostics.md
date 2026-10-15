# Performance diagnostics on Linux

The text-log diagnostics sink reports each graph's numeric minimum, average,
maximum, and sample count every five seconds at debug level. WARNING and INFO
events are counted over the actual reported interval; SILENT events are excluded.
Warnings are delayed until the next summary, not written from the playout callback.
Reporting continues without new events, so the last burst is not lost. Retired
graphs are flushed on the next reporter pass (approximately one second).

Use debug level to retain summaries without enabling trace. Each line uses
the graph's existing component label. These are interval aggregates, not an
exact per-frame trace or percentile distribution.

Each active graph emits at most one combined event-and-measurement line per
five-second interval (12 lines per minute); graph retirement or shutdown can
emit one final partial-interval line. Idle graphs emit nothing. Repeated events
increment counts rather than generating extra lines. Total volume scales with
the number of active graphs, not the number of frames or events. Event severity
is included in the summary text; the summary itself is always debug level.
Existing logging outside this sink is unchanged.

## Units and interpretation

- Channel `produce-ms`, `mix-ms`, `consume-ms`, and `frame-ms` are wall-clock
  milliseconds. Consumption includes output pacing/waiting; it is not CPU time.
- DeckLink `callback-gap-ms` is elapsed time between completion callbacks.
- DeckLink `buffered-video-frames` is the hardware's queued video frame count.
- DeckLink `buffered-audio-sample-frames` counts sample frames, not individual
  interleaved channel samples. `buffered-audio-ms` converts using the output
  sample rate. These are sampled before scheduling replacement output.
- `audio-buffer-empty` counts zero-buffer observations, which indicate risk but
  do not by themselves prove an audible underrun. Buffer query failures have
  separate warning counts and are not recorded as zero samples.
- Existing graph values retain their original units. In particular, normalized
  `*-time` values are not milliseconds and normalized queue values are not raw
  counts. Do not compare them directly with the new explicitly named metrics.

Compare producer underflows and buffer minima with channel timing maxima,
FFmpeg consumer input queue levels/drops, and DeckLink audio minima and late
frames. Five-second summaries narrow the cause but cannot establish ordering
between events within the same interval.

No encoder thread counts, scheduling policy, or affinity defaults are changed
by these measurements.

## External capture

Run on the actual playout server, using its CasparCG PID in place of `PID`.
Collect timestamps alongside CasparCG's log. The following tools may require
the distribution's `sysstat` and `numactl` packages:

```sh
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
numactl --hardware
lspci -Dnn
pidstat -H -t -u -w -d -p PID 1
mpstat -P ALL 1
iostat -xz -t 1
numastat -p PID
nvidia-smi dmon -d 1 -o DT
```

Check PCIe locality using `/sys/bus/pci/devices/PCI_ADDRESS/numa_node`
(`-1` means unknown), and effective per-thread affinity using
`/proc/PID/task/TID/status` (`Cpus_allowed_list`). CPU affinity does not bind
memory or establish IRQ locality. Keep the exact server build, channel format,
media codec, consumer arguments, NVIDIA driver, and Desktop Video version with
the capture. Inspect `journalctl -k` over the incident window for driver faults.
Stop monitoring commands with Ctrl-C after reproducing the issue.

## Focused sink regression test

With the normal Boost development dependencies installed, run from the repository
root (the executable is outside the worktree):

```sh
g++ -std=c++17 -DBOOST_LOG_DYN_LINK -pthread -Isrc \
    tools/linux/tests/performance_diagnostics_test.cpp \
    src/common/diagnostics/graph.cpp src/common/diagnostics/graph_log_sink.cpp \
    src/common/utf.cpp -lboost_log -lboost_thread -lboost_filesystem \
    -lboost_system -o /tmp/caspar-performance-diagnostics-test
/tmp/caspar-performance-diagnostics-test
```

The test checks idle-period flushing, numeric aggregates, silent-event exclusion,
concurrent updates, and final reporting after a graph is destroyed. It does not
exercise DeckLink hardware.
