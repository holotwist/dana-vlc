# dana-vlc

VLC plugin for Dana audio.

## Requirements

- C11 compiler (`gcc` or `clang`)
- CMake >= 3.16
- VLC >= 3.0 development headers (`libvlccore-dev`, `libvlc-dev`, or `vlc-devel`)
- `pkg-config`
- Dana source tree (clone it in src folder)

### Distro Packages

- **Arch Linux**: `vlc vlc-plugins-base`
- **Debian / Ubuntu**: `libvlccore-dev libvlc-dev`
- **Fedora**: `vlc-devel`

## Build and Installation

The build script automatically searches for Dana tree in ../dana, and installs the plugin in your system.

### Build and install for the current user:

```bash
./build.sh --dana-path /path/to/dana --user --install
```

Installed to: `~/.local/lib/vlc/plugins/codec/libdana_plugin.so`

### System-wide install:

```bash
sudo ./build.sh --dana-path /path/to/dana --install
```

Installed to: `/usr/lib/vlc/plugins/codec/libdana_plugin.so` (or distro plugin path).

### Uninstall:

```bash
./build.sh --user --uninstall
# or system-wide:
sudo ./build.sh --uninstall
```

## Manual Build (CMake)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DDANA_SOURCE_DIR=/path/to/dana
cmake --build .
```

Copy `libdana_plugin.so` into your VLC codec plugin directory and refresh the plugin cache:

```bash
vlc-cache-gen <path_to_vlc_plugins_dir>
```

## License

Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
```