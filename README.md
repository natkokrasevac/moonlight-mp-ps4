# Moonlight MP client for PlayStation 4

This is an unofficial Moonlight client for PlayStation 4 with multi-controller / multiplayer support (Moonlight MP).

Features:
- Multi-controller support: Connect multiple DualShock 4 controllers (Player 1, Player 2, etc.) to play multiplayer games on host PC.
- DualShock 4 force feedback (dual-motor rumble) with independent per-controller vibration.
- Dynamic in-game RGB light bar color control.
- PS4 Pro support with HEVC and true 4K video pipeline.
- Hardware-accelerated decoding and rendering.
- Mouse emulation on Player 1 touchpad and keyboard/mouse pass-through.

## Building from Source

This code is setup for VScode. It includes a build task in `.vscode/`

The current toolchain and script are designed for macOS and Linux.  
Windows will be added when time allows.

### Toolchain

- OpenOrbis SDK  
(tested with 0.5.4 non-docker)

- opengnm  
(tested with commit 4b295ca)

- OPTIONAL: opengnm-psbc shader compiler  
only needed for custom shaders. compiled shaders are included with source code.

- LLVM / LLD / Clang  
(tested with clang 22.1.8; `cmake/toolchain-ps4-linux.cmake` references clang-18)

- CMake >= 3.18  
(tested with 4.2.2)

macOS:

- install Xcode command-line tools

	`xcode-select --install`

- install LLVM and LD (CMake is part LLVM install)

	`brew install llvm lld`

- set toolchain environment variable in ~/.zshrc (macOS > 10.15 for zsh, otherwise set in bash_profile)

	`export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain`  
	if using the OpenOrbis Mac installer PKG, the path will be set automatically. 
	
	`export PATH=$PATH:$OO_PS4_TOOLCHAIN/bin/macos`

	then reload your shell config:

    `source ~/.zshrc`

- compile opengnm with OpenOrbis Toolchain

	inside the `opengnm` directory:  
	`cp config.orbis.mak config.mak`  
	`make`  
	`make install DESTDIR=$OO_PS4_TOOLCHAIN`

	this will install opengnm libraries in $OO_PS4_TOOLCHAIN/include/opengnm
	you need to manually copy the libopengnm.a file to $OO_PS4_TOOLCHAIN/lib

Linux (Debian shown, adapt for other distros):

- install LLVM 18, LLD 18, and CMake

    `sudo apt install clang-18 lld-18 llvm-18 cmake`  
	If clang/llv/lld > 18 is used, update `cmake/toolchain-ps4-linux.cmake` accordingly

- set the toolchain environment variable in ~/.bashrc (or ~/.profile)

    `export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain`

    `export PATH=$PATH:$OO_PS4_TOOLCHAIN/bin/linux`

    then reload your shell config:

    `source ~/.bashrc`

- compile opengnm with OpenOrbis Toolchain

	inside the `opengnm` directory:  
	`cp config.orbis.mak config.mak`  
	`make`  
	`make install DESTDIR=$OO_PS4_TOOLCHAIN`

	this will install opengnm libraries in $OO_PS4_TOOLCHAIN/include/opengnm
	you need to manually copy the libopengnm.a file to $OO_PS4_TOOLCHAIN/lib

Windows (via Docker):

- Run the included PowerShell build script:
	`.\build-docker.ps1`
	This builds the Docker builder container (with OpenOrbis v0.5.4 toolchain and OpenGNM) and generates the `.pkg` package directly.

### Obtaining the code

`git clone https://git.fourletterworkshop.com/moonlight-dx-ps4.git`

`cd moonlight-dx-ps4`

Update the submodules  
`git submodule update --init --recursive`

The submodules remain untouched when building.  
The compiler will slipstream in files during compile, so the project *should* survive updates to these libraries.

### Configuring

`pkg_template/` comes with all assets and necessary files for the package; they can be changed if desired.  
Do not change `pkg_template/sce_module/` or `pkg_template/sce_sys/` unless you know what you are doing.
The Makefile script will compile the code, create the eboot.bin, calculate the param.sfo (if needed), include all assets, and package them into the fpkg.

This project uses:

- TITLE_ID    := MLDX00001
- CONTENT_ID  := IV0000-MLDX00001_00-MOONLIGHTDX00000

If you want to make your own fork, please change the Title ID and Content ID in `config/config_pkg`. This will recalculate `param.sfo`

### Building

`Makefile` is a wrapper around `CMake` and packaging scripts. It should work without modification if environment variable is set. If compiler paths are not correct, edit `cmake/toolchain-ps4-[macos/linux].cmake`

to build:

`make`
This will compile the .elf, create the eboot.bin, pkg.gp4, and the .pkg file.

`make ftp`
This will build the pkg as in `make` and upload it to the console via ftp.  
The console IP address and Port [default is 2121] can be configured in `config/config_ftp`

`make clean`  
for cleaning all build files

### Installing

No kernel files, separate uploads or installations are necessary.  
The .pkg is self-contained.

via USB:  
- copy .pkg file onto USB stick (FAT or exFAT) and install via GoldHen package installer.
- make sure Package Source in GoldHen is set to `Usb` or `All`

via FTP (manually):  
- make sure the FTP server is activated on the console
- use an FTP client to connect to [console ip]:[port - default is 2121]
- upload to /data/pkg/
- make sure Package Source in GoldHen is set to `Hdd` or `All`

### Using

The client has built-in help functions and overlays, so it should be easy to use.

