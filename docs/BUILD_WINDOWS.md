# Build on Windows

The supported release toolchain is Visual Studio 2022 x64 plus CMake/Ninja. Developers may use MinGW, but public Windows packages should use MSVC.

```powershell
pwsh scripts/build-windows.ps1 -Variant avx2
pwsh scripts/build-windows.ps1 -Variant generic
```

The AVX2 build disables `-march=native` and enables AVX2. The generic build disables AVX, AVX2, FMA, F16C, and BMI2 for wider compatibility, but current upstream still requires SSE4.2 for this x86 CPU backend variant. Both are Release builds.

Unicode source and model paths are supported by the paging reader and launcher. Some third-party UI build tools can still mishandle a Unicode build directory; build through a short ASCII drive alias or use the verified upstream UI archive fallback.

OpenSSL is optional. Without it, the local HTTP server works but built-in HTTPS is disabled; use a trusted reverse proxy for remote TLS.
