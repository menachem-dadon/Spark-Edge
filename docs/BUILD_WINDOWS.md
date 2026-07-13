# Build on Windows

Install Git, CMake, Ninja, Node.js 22 with npm, and either Visual Studio 2022 x64 or a current MinGW-w64 toolchain. Public Windows packages should use the documented CI toolchain. Node.js is required because the QwenStream Server settings panel is built into the chat UI.

```powershell
pwsh scripts/build-windows.ps1 -Variant avx2
pwsh scripts/build-windows.ps1 -Variant generic
```

The AVX2 build disables `-march=native` and enables AVX2. The generic build disables AVX, AVX2, FMA, F16C, and BMI2 for wider compatibility, but current upstream still requires SSE4.2 for this x86 CPU backend variant. Both are Release builds.

Unicode source and model paths are supported by the paging reader and launcher. Some third-party UI build tools can still mishandle a Unicode build directory; build through a short ASCII drive alias or use the verified upstream UI archive fallback.

The build places `dashboard.html` next to `QwenStream.exe`. Keep that file beside the executable to use the redesigned control center; the executable retains a minimal embedded fallback page for recovery.

OpenSSL is optional. Without it, the local HTTP server works but built-in HTTPS is disabled; use a trusted reverse proxy for remote TLS.
