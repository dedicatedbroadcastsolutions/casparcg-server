CasparCG Server
===============

Thank you for your interest in CasparCG Server, a professional software used to
play out and record professional graphics, audio and video to multiple outputs.
CasparCG Server has been in 24/7 broadcast production since 2006.

The CasparCG Server works on Windows and Linux.

System Requirements
-------------------

- A graphics card (GPU) capable of OpenGL 4.5 is required.
- An Nvidia GPU is recommended, but other GPU's will likely work fine.
- Intel and AMD CPU's have been tested and are known to work
- PCIE bandwidth is important between your GPU and CPU, as well as Decklink and CPU. Avoid chipset lanes when possible.

### Windows

 - Windows 11 is recommended
 - Windows 10 is supported as best effort

### Linux

 - Ubuntu 24.04 is recommended
 - Other distributions and releases will work but you will need to compile it yourself and have not been tested

Getting Started
---------------

1. Download a release from (http://casparcg.com/downloads).
   Alternatively, newer testing versions can be downloaded from (http://builds.casparcg.com) or [built from source](BUILDING.md)

2. Install any optional non-GPL modules
    - Flash template support (Windows only):

    1. Uninstall any previous version of the Adobe Flash Player using this file:
        (http://download.macromedia.com/get/flashplayer/current/support/uninstall_flash_player.exe)

    2. Download and unpack
        (http://download.macromedia.com/pub/flashplayer/installers/archive/fp_11.8.800.94_archive.zip)

    3. Install Adobe Flash Player 11.8.800.94 from the unpacked archive:
        fp_11.8.800.94_archive\11_8_r800_94\flashplayer11_8r800_94_winax.exe

3. Configure the server by editing the self-documented "casparcg.config" file in
   a text editor.

4.
   1. Windows: start `casparcg_auto_restart.bat`, or `casparcg.exe` and `scanner.exe` separately.
   1. Linux: start the `run.sh` program or use tools/linux/start_docker.sh to run within docker (documentation is at the top of the file).

5. Connect to the Server from a client software, such as the "CasparCG Client"
   which is available as a separate download.

Documentation
-------------

The most up-to-date documentation is always available at https://casparcg.com/docs/wiki

Ask questions in the forum: https://casparcgforum.org/

Development
-----------

See [BUILDING](BUILDING.md) for instructions on how to build the CasparCG Server from source manually.

Native route audio mapping
--------------------------

Native `route://` producers always have an audio patchbay, initially an identity
map: each destination receives the same-numbered source audio channel, matching
master's normal passthrough. Entries use `destination:source` with zero-based
audio channel indices. Each command patches only the destinations listed;
unlisted destinations retain their existing mappings and are never implicitly
muted. Source channel buffers are not modified. The same creation option works
with `LOAD` and `LOADBG`; it is native route processing, not the FFmpeg producer's
`AF` option.

Normal route commands require no changes. The optional mapping uses colon-separated
indices, similar in style to FFmpeg selectors, but its order is destination first:
`2:4` sends source audio channel 4 to destination audio channel 2.

```text
PLAY 5-10 route://1
PLAY 5-20 route://2 AUDIO_MAP "2:0,3:1"
```

Change a playing route without replacing its producer using `CALL`. The update
takes effect on the next source frame pair, after previously queued frames drain.
`DEFAULT` resets all destinations to identity. Calling `AUDIO_MAP` without a
value returns the complete accepted map, including explicit silences.
The effective mapping is reported as `route/audio-map` in producer state, and
the source bus width as `route/source-audio-channels`. Mapping uses the engine's
existing 16-channel PCM buses: source and destination indices are 0 through 15.
This feature does not change channel bus widths or add a configurable audio
channel count.

```text
CALL 5-20 AUDIO_MAP "2:4,3:5"
CALL 5-20 AUDIO_MAP "4:SILENCE"
CALL 5-20 AUDIO_MAP "4:4"
CALL 5-20 AUDIO_MAP
CALL 5-20 AUDIO_MAP DEFAULT
```

`destination:SILENCE` is the only way to explicitly mute a destination. Routing
it to a source again restores audio. Invalid patches are rejected atomically
without changing any destination.

Live changes ramp only the affected destinations down over 5 ms and back up
over 5 ms, using the source sample rate. Other destinations remain sample-exact,
even while a different destination is fading. This also applies to explicit
silence and `DEFAULT`; initial creation maps apply immediately. Identity routes
share the original PCM and preserve master passthrough behavior, including
late-frame repeats. Remapped or fading frames mute repeated late audio rather
than replaying stale audio, while preserving their video.

License
---------

CasparCG Server is distributed under the GNU General Public License GPLv3 or
higher, see [LICENSE](LICENSE) for details.

CasparCG Server uses the following third party libraries:
- FFmpeg (http://ffmpeg.org/) under the GPLv2 Licence.
  FFmpeg is a trademark of Fabrice Bellard, originator of the FFmpeg project.
- Threading Building Blocks (http://www.threadingbuildingblocks.org/) library under the GPLv2 Licence.
- SFML (http://www.sfml-dev.org/) under the zlib/libpng License.
- GLEW (http://glew.sourceforge.net) under the modified BSD License.
- boost (http://www.boost.org/) under the Boost Software License, version 1.0.
