# WebView2 desktop integration

Status: implemented in source for local validation. The complete release matrix below remains required. A successful build or syntax check does not establish native UX, clean-machine installation, or a signed release.

## Architecture

SparkMoE uses a direct Win32 WebView2 host in `tools/sparkmoe/desktop-window.cpp`. The Svelte chat and management interface remain shared with browser mode. No Electron, CEF, Tauri, or external browser window embedding is involved.

The stable application origin is `http://127.0.0.1:18080/`, or the manager port selected through `SPARKMOE_MANAGER_PORT`. The manager serves the embedded UI and forwards inference requests to its Router. Stopping the Router does not remove the application's UI or model-management APIs.

The manager listener retains its own lifecycle. The desktop window owns a separate, joinable STA thread, message loop, HWND, controller, and environment. The existing notification-area window retains its own thread. Native activation crosses these boundaries through posted messages; WebView2 objects never leave their owning thread.

`desktop_window::start(options)` waits up to 25 seconds for successful initial navigation. An initialization timer limits each WebView2 startup attempt to 20 seconds. The main window becomes visible after initial navigation succeeds. The notification-area controls and session log provide startup and failure information.

`show()` restores and activates the existing window. `stop()` posts an unconditional close and joins its thread. Shutdown callbacks signal the manager; they must not join the desktop thread. The root lifecycle joins the startup worker, closes the desktop, removes the notification-area icon, and stops the Router before destroying shared state.

## Build and dependency restore

The official Microsoft.Web.WebView2 SDK is pinned to `1.0.3650.58`. Its NuGet source URL and SHA256 are recorded in `packaging/windows/webview2-sdk.json`.

Run:

```powershell
./scripts/restore-webview2.ps1
./scripts/build-windows.ps1 -Variant avx2 -RunTests
```

The restore helper checks the archive hash before extracting required SDK headers, the x64 loader, and Microsoft notices. It produces an SDK file-hash manifest. Existing cached archives are checked again. Build dependencies remain under ignored `build-deps/webview2/`.

The normal Windows build script restores the SDK explicitly and enables `SPARKMOE_WEBVIEW2`. CMake never downloads a dependency during configure. A direct configure needs `SPARKMOE_WEBVIEW2_SDK_DIR` pointing to the restored SDK. The current package and loader path target Windows x64.

For a build without the desktop shell:

```powershell
./scripts/build-windows.ps1 -Variant avx2 -BrowserOnly
```

Non-Windows builds retain browser mode. The native source has a no-WebView implementation for those builds.

## Runtime and fallback

The application loads `WebView2Loader.dll` only from the executable directory, with system DLL lookup restricted to the system directory. It calls Microsoft's Runtime version API before creating an environment and selects the stable Evergreen channel. It does not download or install a Runtime.

If the loader, Runtime, environment, controller, or initial navigation is unavailable, the manager opens the same stable application origin in the default browser. Serving and model management do not depend on desktop initialization.

One controller/environment recreation is attempted after a WebView2 process failure. A further failure opens browser fallback without restarting loaded models. Logs contain the failing stage and HRESULT, without prompts, browser storage, or personal file paths.

Set `SPARKMOE_BROWSER_ONLY=1` for a recovery launch that bypasses WebView2 initialization. `SPARKMOE_NO_BROWSER=1` is the existing headless test switch and suppresses opening either surface.

## Window behavior and storage

The real top-level window provides the SparkMoE icon, normal taskbar presence, resize, minimize, restore, focus, and Windows title-bar theme. Its minimum size scales with DPI and fits the current monitor. Saved normal bounds and maximized state are restored within the available monitor work area, with size adjusted for changed DPI.

Closing exits SparkMoE by default. If background operation is enabled, close hides the window and leaves the notification-area controls. Closing with an active proxied generation requests confirmation. Notification-area Exit and manager shutdown use the unconditional close path.

The existing single-instance manager endpoint `/api/open` activates the native window when available, otherwise it opens browser fallback. No additional local port or broad native bridge is introduced.

The WebView profile is stored under `%LOCALAPPDATA%\\SparkMoE\\WebView2` by default. `SPARKMOE_DATA_DIR` can select a separate application-data root. Window placement is stored beside that profile. Installed and portable packages use the same explicit data policy; extraction does not silently place personal data beside the executable.

Browser and desktop storage are separate. Use the chat UI's export/import controls to transfer conversations; SparkMoE does not read another browser's profile database. Upgrades, fallback, and uninstall preserve local application data.

## Navigation, permissions, and downloads

- Top-level and frame navigation require the exact configured manager origin.
- User-initiated external HTTP/HTTPS links open in the default browser.
- New-window requests are handled explicitly; same-origin links remain in the shell.
- File, JavaScript, data, custom, and UNC navigation are rejected.
- Native host objects, web messages, and release DevTools are disabled.
- User-initiated microphone and clipboard-read requests from the manager origin use Runtime permission prompts. Other permission requests are denied.
- Downloads require a manager URL or a blob belonging to that origin and retain the standard Runtime download controls.
- Uploads, clipboard writing, keyboard interaction, and chat dialogs use ordinary web capabilities. There is no general JavaScript-to-native command interface.

## Installer and portable packaging

`scripts/package-windows.ps1` checks the configured SDK manifest and compares the built loader hash before staging. It includes Microsoft SDK notices under `licenses/WebView2`, `PORTABLE.txt`, the SDK version/archive hash, and binary signatures in the release manifest. Browser-only packages exclude a stale WebView2 loader from a previous build.

`scripts/build-installer.ps1` verifies stage hashes before compiling. For desktop packages it downloads or reuses Microsoft's small Evergreen bootstrapper and requires a valid Microsoft Corporation Authenticode signature. The installer artifact manifest records that bootstrapper's version and hash. The full Runtime is not bundled.

Inno Setup checks documented per-user and per-machine WebView2 registry keys. A missing Runtime exposes an unchecked, explicit optional installation task. The bootstrapper runs as the original user. Decline or installation failure preserves browser fallback.

The installer uses per-user installation. Its optional PATH update compares complete normalized segments and records whether it added the segment. Uninstall removes only the segment that this installer added, preserves similar directory names, and retains application data. Windows receives the environment-change notification.

To sign the outer installer with an existing CurrentUser code-signing certificate:

```powershell
./scripts/build-installer.ps1 -StageDir <stage> -SigningCertificateThumbprint <thumbprint>
```

Signing requires an available private key and a trusted timestamp; the script rejects failed signature or timestamp verification. This option does not sign all application binaries or the embedded uninstaller. Without a supplied certificate the artifact is explicitly recorded as unsigned. Do not present an unsigned local installer as a trusted production release.

## Validation matrix

Record evidence separately for each category:

| Category | Required checks |
| --- | --- |
| Source/build | AVX2 and generic Release, browser-only build, UI check/build, server regression, upstream integration, diff whitespace |
| Native Windows | One first-launch window; second launch activation; focus, keyboard, taskbar, minimize/restore; light/dark and 100/125/150/200 percent DPI; changed monitors |
| Lifecycle | Idle close; active load/generation close; background setting; notification-area Open/Start/Stop/Exit; manager shutdown; Windows logoff; Explorer restart |
| Failure | Missing loader/Runtime; startup timeout; render/browser process failure; browser fallback; Router stopped/crashed without losing management UI |
| UI capabilities | Conversation persistence/export/import; streaming/cancel/reconnect; model settings; folder picker ownership; upload/drag-drop; downloads; microphone/clipboard prompts; external links and rejected schemes |
| Package | Installed and extracted ZIP; Windows 10/11; standard user; Unicode paths; no bundled user data; required files and all manifest hashes |
| Installer | Install/upgrade/uninstall; Runtime present/missing; optional task declined/failed/succeeded; exact PATH addition/removal; signature status |

PowerShell parser and Inno Setup syntax-fixture checks establish syntax only. They do not install SparkMoE, run the Runtime bootstrapper, or validate the real packaged application. Record live native checks and clean-machine installer checks before release.

## Official references

- [Win32 setup](https://learn.microsoft.com/en-us/microsoft-edge/webview2/get-started/win32)
- [Runtime distribution and detection](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution)
- [Security guidance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security)
- [User data folders](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/user-data-folder)
- [Pinned NuGet SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.3650.58)
