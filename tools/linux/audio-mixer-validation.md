# Audio Mixer Validation

Incomplete trailing interleaved channel samples are discarded and reported as
`audio-invalid-buffer`. If no complete channel frame remains, that item's audio
is skipped for the tick. Complete but short input retains the existing
last-sample-per-channel hold and reports `audio-buffer-underflow` when that hold
is needed.

With `BUILD_TESTING` enabled, run the focused regression:

```sh
cmake --build build --target audio_mixer_test
ctest --test-dir build -R '^audio_mixer$' --output-on-failure
```

The production-backed test covers incomplete frames, cadence, tagged and
untagged streams, startup and carry-over, recovery after invalid input, output
length, warning events, volume ramps, and full-scale peak metering.