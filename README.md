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

Channel audio matrix
--------------------

`MIXER <channel> AUDIO_MATRIX` configures a weighted sum for each output audio
channel. Each input is `source-channel:source-layer:audio-index:PRE|POST:gain`.
Channel and layer numbers use normal AMCP addressing; audio indices are
zero-based. Gains are finite values from -16 to 16. Inputs add together, and
one input can feed multiple outputs. Up to 64 inputs per output and 64 distinct
source buses per destination channel are supported.

`PRE` reads before the outer layer fader; `POST` reads after it. `*` selects
the whole source stage. Taps are pre-master and pre-matrix, so channel matrices
cannot feed back into one another. Only configured output channels are
overridden; `SILENCE` explicitly mutes an output and `DEFAULT` restores legacy
summing.

```text
MIXER 5 AUDIO_MATRIX 0 "5:10:0:POST:1,5:20:4:PRE:0.5"
MIXER 5 AUDIO_MATRIX 1 "1:10:4:PRE:1"
MIXER 5 AUDIO_MATRIX 2 "2:*:0:POST:1"
MIXER 5 AUDIO_MATRIX 3 SILENCE
MIXER 5 AUDIO_MATRIX
MIXER 5 AUDIO_MATRIX 3 DEFAULT
MIXER 5 AUDIO_MATRIX DEFAULT
```

Updates replace a destination bus atomically without recreating producers,
video, or consumers. Cross-channel taps transport PCM only through bounded
single-writer/single-reader FIFOs; they do not render source video for the tap.
Different video rates and field cadences are accommodated by sample-count
consumption. All source and destination channels must use the same sample rate;
rate mismatches are rejected, and independent-clock drift is not corrected.
Format changes invalidate the affected tap until its matrix configuration is
reissued. See [BUILDING](BUILDING.md) for the headless regression test commands.

Channel audio width
-------------------

Set `<audio-channels>` inside a `<channel>` in `casparcg.config` to configure
that channel's audio bus width (1–256); it defaults to 16. The core mixer,
routes, audio matrix, and FFmpeg/HTML producers preserve the configured width,
padding or truncating source PCM as needed.

DeckLink and Bluefish embedded-audio outputs remain limited to 16 channels per
output connector and reject larger configured widths when embedded audio is
enabled. This setting does not distribute audio across multilink connectors.

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
