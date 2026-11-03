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

### Linux HTML/CEF shutdown and supervision

Use the normal AMCP `KILL` command, or send `SIGINT`/`SIGTERM` to the CasparCG
**server PID** after startup. Leave CEF renderer, GPU, network-service and zygote
processes running while CasparCG closes browsers and shuts CEF down. Supervisors
should wait for the server to exit before restarting it. Do not send an initial
shutdown signal to the entire process tree or cgroup: killing CEF children while
the browser process is still running can trigger Chromium's crash recovery.
Reserve forced process-tree termination for a graceful-shutdown timeout; it is
not expected to produce a clean CEF shutdown.

On Linux, a child-side fork hook places subprocesses in separate process groups
before exec, including while the dynamic loader is loading CEF. The subprocess
entry point also checks isolation before executing CEF. The hook is process-wide;
CasparCG's current subprocess forks are CEF's.
This keeps a terminal's foreground-group Ctrl+C from prematurely terminating
those children. This does not shield them from direct PID signals, cgroup/tree
termination or the brief interval between the kernel fork and its child hook.

The HTML module tracks pending browser creation as well as existing browsers.
Shutdown closes them, keeps the CEF loop alive until all close callbacks finish,
then calls `CefShutdown` on the same thread used for `CefInitialize`. A producer
closed before its asynchronous browser creation completes closes that browser as
soon as it is created. No shutdown timeout forcibly calls CEF teardown with live
browsers; a supervisor should enforce its own final termination deadline.

JavaScript/browser console messages are routed through CasparCG's HTML logging
callback. Native Chromium diagnostics are different: CEF/Chromium initializes
its own logging, and native errors/fatals can be written directly to inherited
stderr, including in subprocesses. CasparCG's `log-level` does not filter those
writes. A supervisor capturing the child's stderr will therefore see them.
This change does not redirect stderr, disable native logging or disable GPU
acceleration; runtime failures and fatal diagnostics remain visible.

Development
-----------

See [BUILDING](BUILDING.md) for instructions on how to build the CasparCG Server from source manually.

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
