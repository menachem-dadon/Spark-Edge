# GiantLLM branding assets

Replace these two files while keeping their exact names:

- `giantllm.svg`: dashboard logo and browser favicon. Use a square SVG with a `viewBox` of `0 0 512 512`.
- `giantllm.ico`: Windows executable, installer, uninstall entry, Start menu, and desktop shortcut icon.

The ICO should contain square images at 16, 24, 32, 48, 64, 128, and 256 pixels. Keep important artwork away from the outer 8 percent so Windows does not crop it at small sizes.

After replacing either file, rebuild and package:

```powershell
.\scripts\build-windows.ps1 -Variant avx2 -RunTests
.\scripts\package-windows.ps1 -BuildDir .\build-windows-avx2 -Variant avx2
.\scripts\build-installer.ps1
```

Do not rename the files. The build, dashboard, portable package, and installer all use these paths.
