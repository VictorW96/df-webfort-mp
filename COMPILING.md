## HOW TO COMPILE ##

Webfort compiles as an external plugin of DFHack, targeting
**Dwarf Fortress 53.x (Steam, v50+)**.

The source is hosted at:

	https://github.com/VictorW96/df-webfort-mp

To get a working copy:

	git clone --recursive https://github.com/VictorW96/df-webfort-mp

Then you can follow the usual
[DFHack compiling process](https://docs.dfhack.org/en/stable/docs/dev/compile/Compile.html#how-to-get-the-code).

Below are extra notes which you should probably read.

### Linux (Ubuntu 22.04+, 64-bit) ###

Install the standard DFHack build dependencies as documented upstream, then
build with CMake and Ninja:

	mkdir build && cd build
	cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
	ninja webfort && ninja install

Webfort bundles all of its own C++ dependencies (websocketpp, standalone asio,
lodepng), so no extra packages beyond the standard DFHack requirements are
needed.

### Windows ###

Follow the standard DFHack Windows build instructions using Visual Studio 2022
or a recent MSVC toolchain. The same bundled dependencies apply — no separate
Boost or Asio install is required.
