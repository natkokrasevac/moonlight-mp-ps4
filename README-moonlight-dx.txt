MOONLIGHT MP FOR PLAYSTATION 4
==============================
Version 2.10

This is an unofficial Moonlight client for PlayStation 4 with multi-controller support (MP).

The most recent build can be found at 
https://fourletterworkshop.com/projects/moonlight_dx_for_playstation_4

Features
~~~~~~~~
- All common Moonlight client features, incl. mDNS discovery
- Multi-controller support (Player 1 + Player 2 up to 4 local controllers)
- PS4 Pro support with HEVC and true 4K video pipeline
- Fully GPU-accelerated (stream and UI) - console should stay cool(er)
- Automatic bitrate recommendation (Moonlight convention)
- PlayStation 4 inspired UI, choose between CrossBar and classic Grid presentation
- OrbisOS controller limitation adaptation for in-stream controls
- Keyboard and Mouse pass-through
- Mouse emulation via touchpad (in-stream toggle)

Installing
~~~~~~~~~~
Requires jailbroken PlayStation 4
Tested on PlayStation 4 and PlayStation 4 Pro - Firmware 11.5 & 12.02 - GoldHen 2.4

No kernel files, plugins, separate uploads or installations are necessary.  
The .pkg is self-contained.

via USB:  
- copy .pkg file onto USB stick (FAT or exFAT) and install via GoldHen package installer.
- make sure Package Source in GoldHen is set to `Usb` or `All`

via FTP:  
- make sure the FTP server is activated on the console
- use an FTP client to connect to [console ip]:[port - default is 2121]
- upload to /data/pkg/
- make sure Package Source in GoldHen is set to `Hdd` or `All`

Using
~~~~~
The client has built-in help functions and overlays, so it should be easy to use.

License
~~~~~~~
This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with this program. If not, see https://www.gnu.org/licenses/.
