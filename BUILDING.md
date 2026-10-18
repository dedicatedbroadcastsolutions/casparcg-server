# Building the CasparCG Server

The CasparCG Server source code uses the CMake build system in order to easily
generate build systems for multiple platforms. CMake is basically a build
system for generating build systems.

On Windows we can use CMake to generate a .sln file and .vcproj files. On
Linux CMake can generate make files or ninja files. Qt Creator has support for
loading CMakeLists.txt files directly.

# Dependency caching

CMake will automatically download some dependencies as part of the build process.
These are taken from https://github.com/CasparCG/dependencies/releases (make sure to expand the 'Assets' group under each release to see the files), most of which are direct copies of distributions from upstream.

During the build, you can specify the CMake option `CASPARCG_DOWNLOAD_MIRROR` to download from an alternate HTTP server (such as an internally hosted mirror), or `CASPARCG_DOWNLOAD_CACHE` to use a specific path on disk for the local cache of these files, by default a folder called `external` will be created inside the build directory to cache these files.

If you want to be able to build CasparCG offline, you may need to manually seed this cache. You can do so by placing the correct tar.gz or zip into a folder and using `CASPARCG_DOWNLOAD_CACHE` to tell CMake where to find it.
You can figure out which files you need by looking at each of the `ExternalProject_Add` function calls inside of [Bootstrap_Linux.cmake](./src/CMakeModules/Bootstrap_Linux.cmake) or [Bootstrap_Windows.cmake](./src/CMakeModules/Bootstrap_Windows.cmake). Some of the ones listed are optional, depending on other CMake flags.

# Windows

## Building distributable

1. Install Visual Studio 2022.

2. Install 7-zip (https://www.7-zip.org/).

3. `git clone --single-branch --branch master https://github.com/CasparCG/server casparcg-server-master`

4. `cd casparcg-server-master`

5. `.\tools\windows\build.bat`

6. Copy the `dist\casparcg_server.zip` file for distribution

## Development using Visual Studio

1. Install Visual Studio 2022.

2. `git clone --single-branch --branch master https://github.com/CasparCG/server casparcg-server-master`

3. Open the cloned folder in Visual Studio.

4. Build All and ensure it builds successfully

# Linux

## Building on your system

We only officially support Ubuntu LTS releases, other distros may work but often run into build issues. We are happy to accept PRs to resolve these issues, but are unlikely to write fixes ourselves.

We currently document two approaches to building CasparCG. The recommended way is to use the `deb` packaging we have in the repository for Ubuntu and Debian.
For native builds on other distributions, follow the Development steps below.

We also provide a script to produce a build in docker, but this is not recommended unless absolutely necessary. The resulting builds are often rather brittle depending on where they are used.

To perform a custom build, follow the Development steps below, and you may need to do some extra packaging steps, or install steps on the target systems.

### Building inside Docker

1. `git clone --single-branch --branch master https://github.com/CasparCG/server casparcg-server-master`
2. `cd casparcg-server-master`
3. `./tools/linux/build-in-docker`

If all goes to plan, a docker image `casparcg/server` has been created containing CasparCG Server.

### Extracting CasparCG Server from Docker

1. `./tools/linux/extract-from-docker`

You will then find a folder called `casparcg_server` which should contain everything you need to run CasparCG Server.

_Note: if you ran docker with sudo, CasparCG server will not be able to run without sudo out of the box. For security reasons we do not recommend to run CasparCG with sudo. Instead you can use chown to change the ownership of the CasparCG Server folder._

## Development

Before beginning, check the build options section below, to decide if you want to use any to simplify or customise your build.

1. `git clone --single-branch --branch master https://github.com/CasparCG/server casparcg-server-master`
2. `cd casparcg-server-master`
3. Install dependencies, this can be done with `sudo ./tools/linux/install-dependencies`
4. `mkdir build && cd build`
5. `cmake ../src` You can add any of the build options from below to this command
6. `cmake --build . --parallel`
7. `cmake --install . --prefix staging`

If all goes to plan, a folder called 'staging' has been created with everything you need to run CasparCG server.

The dependency installer handles Ubuntu 24.04/26.04, Debian Trixie/Sid, Arch Linux, Fedora Rawhide, and openSUSE Leap 16.0/Tumbleweed, including CEF setup. It adds the CasparCG PPA on Ubuntu and performs a full system upgrade on Arch. Build as your normal user after installing dependencies.
HTML is enabled by default. Compatible packaged CEF is used on Ubuntu, Arch, and Fedora; Debian and openSUSE default to prebuilt CEF binaries with only the wrapper compiled locally. The downloaded archive is x86-64 only; Ubuntu ARM64 requires its matching PPA package.
These builds require CMake 3.28 or newer and Boost 1.83 or newer. Non-Ubuntu targets are experimental.

Fedora CI uses Rawhide because Fedora 44/45 currently package CEF 146, which is
older than the required API 15000. Like Arch, the Fedora job builds with system
CEF and checks the installed binary's RPATH/RUNPATH, library resolution, and
relocations without setting `LD_LIBRARY_PATH`.

Debian CI checks compilation only: it downloads prebuilt CEF, compiles its wrapper,
and builds CasparCG with HTML enabled. It does not install or package the bundled
CEF runtime, so these jobs do not produce `.deb` artifacts. Native development
installs still include the bundled runtime when `USE_SYSTEM_CEF=OFF`.

### Regression tests

From the repository root, enable tests in your existing build, then build and
run the OSC loopback test:

```
cmake -S src -B build -DBUILD_TESTING=ON
cmake --build build --target osc_client_test
ctest --test-dir build -R '^osc_client$' --output-on-failure
```

The test uses local UDP sockets and does not require a GPU or broadcast hardware.

## Build options

-DENABLE_HTML=OFF - useful if you lack CEF, and would like to build without that module.

-DUSE_STATIC_BOOST=ON - (Linux only, default OFF) statically link against Boost.

-DUSE_SYSTEM_CEF=OFF - (Linux only, default OFF on Debian/openSUSE and ON elsewhere) use pinned prebuilt CEF binaries instead of a compatible system CEF package.

-DENABLE_VULKAN=ON - (Linux only, default OFF) enable Vulkan support; requires a compatible Vulkan SDK.

-DENABLE_AVX2=ON (Linux only, default OFF) Enable the AVX and AVX2 instruction sets (requires a CPU that supports it)

-DDIAG_FONT_PATH - Specify an alternate path/font to use for the DIAG window. On linux, this will often want to be set to an absolute path of a font

-DCASPARCG_BINARY_NAME=casparcg-server - (Linux only) generate the executable with the specified name. This also reconfigures the install target to be a bit more friendly with system package managers.

## Linux crash diagnostics

For crash investigation, configure with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` and
retain the exact executable, shared libraries and debug symbols from the failing
build. This adds debug information; the Linux build still applies its explicit
optimization flags. `-rdynamic` improves in-process symbol lookup for externally
visible executable symbols, not static/local functions or next-process decoding.

The SIGSEGV, SIGABRT and SIGILL handlers attempt to write `./backtrace.dump` in
the process's working directory, then abort using the default SIGABRT disposition.
The directory must be writable. Capture is best effort: a severe crash may also
prevent stack unwinding or writing the dump. CEF subprocesses that exit through
command-line interception do not install these handlers.

After logging is initialized on the next successful startup, CasparCG reports
the saved **raw addresses** to the configured logging sinks and archives the
binary dump as `backtrace-<unique-id>.dump` in the working directory. It does not
delete the evidence or resolve old addresses against the new process's mappings.
Archival creates a hard link without replacing an existing destination, retrying
name collisions up to eight times, then removes the original name only after
the archive exists. The working filesystem must support hard links; failures
retain the original dump and produce an error diagnostic. Failure to remove the
original after archiving retains both names and is also reported.
Unreadable or empty dumps remain in place with an error diagnostic. Retain
archives as needed and remove old ones according to your operational retention
policy. If `backtrace.dump` remains after reporting, the next crash uses a
separate `backtrace-capture-<unique-id>.dump` path announced in the startup log.
Capture creates its file exclusively and never truncates an existing dump or
hard-linked archive. Alternate capture files are retained for manual inspection;
they are not automatically reported on startup. Failure to create a capture file
leaves existing evidence untouched, but no new Boost dump is written.

ASLR changes executable and library addresses between processes. Neither
`-rdynamic` nor debug information makes an address-only dump reliably
symbolizable after restart. Do not disable ASLR as a workaround.

For a reliable symbolized postmortem trace, enable OS core dumps before starting
the server, subject to your deployment's disk-space and retention policies:
Replace `casparcg` in the commands below with the executable name configured by
`CASPARCG_BINARY_NAME` if it differs.

```sh
ulimit -c unlimited
./casparcg casparcg.config
```

For systemd services, configure `LimitCORE=infinity` in the service rather than
relying on an interactive shell limit. Where systemd-coredump is installed:

```sh
coredumpctl list casparcg
coredumpctl debug CRASH_PID
```

Otherwise, locate the core according to `/proc/sys/kernel/core_pattern`, then:

```sh
gdb /path/to/exact/casparcg /path/to/core
```

In GDB, use `thread apply all bt full`. Install or supply matching debug symbols
for shared libraries as well as CasparCG. A core retains the crashed process's
mapping information; the raw Boost dump does not. Core dumps can contain media,
credentials and other process memory: restrict access and review them before
sharing.
