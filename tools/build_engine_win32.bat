@echo off
rem Builds the 32-bit engine DLL. Needs Rust (rustup) and the Visual Studio C++ build tools.
rem Run from a clone of https://github.com/YoucefNabil/delirium-physics (branch skyrim).
rustup target add i686-pc-windows-msvc
cargo build -p delirium-sdk --release --target i686-pc-windows-msvc
echo.
echo Copy target\i686-pc-windows-msvc\release\delirium_physics.dll into  Data\NVSE\Plugins\DeliriumFNV\
