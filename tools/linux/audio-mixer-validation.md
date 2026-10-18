# Audio Mixer Validation

Incomplete trailing interleaved channel samples are discarded and reported as
`audio-invalid-buffer`. If no complete current sample frame remains, valid
cadence-buffered audio is still consumed, with per-channel hold if it runs
short. Without complete current or buffered audio, that item contributes
silence. Invalid input does not reset the stream's cadence or volume history.
Complete but short input reports `audio-buffer-underflow` when the last-sample
hold is needed.

With `BUILD_TESTING` enabled, run the focused regression:

```sh
cmake --build build --target audio_mixer_test
ctest --test-dir build -R '^audio_mixer$' --output-on-failure
```

The production-backed test covers incomplete frames, cadence, tagged and
untagged streams, startup and carry-over, queued-audio preservation on invalid
input, recovery, output length, warning events, volume ramps, and full-scale
peak metering. Peak magnitude saturates negative full scale to `INT32_MAX` to
avoid signed absolute-value overflow without changing the audio samples.