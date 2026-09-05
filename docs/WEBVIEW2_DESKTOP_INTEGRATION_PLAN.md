# WebView2 desktop integration plan

Status: design guidance only. WebView2 is not part of the current SparkMoE build.

This document describes a future Windows-only desktop shell for the existing SparkMoE Web UI. The goal is one branded application window and one taskbar button without replacing the Svelte UI, the local manager, or the llama.cpp Router.

The current notification-area icon should remain the control surface for background operation and the fallback when WebView2 is unavailable.

## 1. Product decision

Use a direct Win32 WebView2 host inside `SparkMoE.exe`.

Do not migrate SparkMoE to Electron, CEF, Tauri, WinUI, or another application framework only to obtain a desktop window. A direct host reuses the current C++ launcher and adds the smallest new runtime surface.

Use the WebView2 Evergreen Runtime by default. Do not bundle a Fixed Version Runtime in the normal ZIP or installer. A Fixed Version package adds more than 250 MB and makes SparkMoE responsible for browser security updates.

Official references:

- Win32 setup: https://learn.microsoft.com/en-us/microsoft-edge/webview2/get-started/win32
- Runtime distribution: https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution
- Security guidance: https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security
- User data folders: https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/user-data-folder

## 2. Goals

- Start SparkMoE from one shortcut.
- Show one native window titled `SparkMoE` with the existing application icon.
- Show the window in the taskbar, Start menu, and Alt+Tab.
- Render the existing llama.cpp and SparkMoE Web UI without a frontend rewrite.
- Keep `SparkMoE.exe`, the manager API, the Router, model profiles, logs, and diagnostics as the authority.
- Preserve the current hidden launch of `llama-server.exe` and its model processes.
- Keep the notification-area icon for Open, Start, Stop, and Exit actions.
- Fall back to the default browser if WebView2 cannot initialize.
- Preserve the Linux behavior that opens the default browser.

## 3. Non-goals

- Do not embed Edge, Chrome, or another external browser window by HWND reparenting.
- Do not expose a general-purpose browser with an address bar.
- Do not allow arbitrary remote navigation inside the trusted application window.
- Do not replace the Svelte UI with native Win32 controls.
- Do not move inference or manager logic into JavaScript.
- Do not add a second model-management protocol.
- Do not bundle Chromium in the normal package.

## 4. Current architecture

```text
SparkMoE.exe
  - owns configuration, logs, and the manager API on 127.0.0.1:18080
  - starts llama-server.exe as a Router without a console window
  - opens http://127.0.0.1:<router-port>/ in the default browser

llama-server.exe
  - serves the embedded Svelte Web UI
  - exposes chat, model, Router, and SparkMoE runtime APIs
  - owns one child process per loaded model
```

Relevant files:

- `tools/sparkmoe/sparkmoe.cpp`: manager lifecycle, browser launch, single-instance mutex, notification-area icon, and manager API.
- `tools/sparkmoe/CMakeLists.txt`: Windows GUI target and native libraries.
- `tools/sparkmoe/sparkmoe.rc.in`: executable icon.
- `tools/ui/`: existing Svelte UI.
- `tools/server/`: embedded UI and Router HTTP server.
- `packaging/windows/SparkMoE.iss`: installer.
- `scripts/build-windows.ps1`: Windows build.
- `scripts/package-windows.ps1`: portable package and ZIP.

## 5. Target architecture

```text
Win32 UI thread
  - main SparkMoE window
  - WebView2 controller
  - notification-area icon
  - window activation and shutdown messages

Manager worker
  - loopback manager API
  - Router process lifecycle
  - configuration and logs

Router process
  - embedded Web UI
  - chat and model APIs
  - model child processes
```

The WebView2 control navigates only to the configured loopback Router origin. The target URL remains:

```text
http://127.0.0.1:<router-port>/
```

The manager redirect at port 18080 can remain for browser fallback, but the native host should navigate directly to the Router after readiness is confirmed.

## 6. Required refactor before WebView2

The current manager blocks in `httplib::Server::listen`. WebView2 requires a single-threaded COM apartment and a Windows message loop on its owning thread.

Refactor the launcher into a small runtime object before adding WebView2:

```text
manager_runtime
  start()
  stop_router()
  start_router()
  request_shutdown()
  router_port()
  router_running()
  router_ready()
  wait_until_ready()
```

Requirements:

- Keep `app_state` protected by its existing mutex.
- Run the manager HTTP listener on a worker thread.
- Keep all HWND, WebView2, and notification-area operations on the Win32 UI thread.
- Use one idempotent shutdown path for the Web UI, window close, notification-area Exit, Windows session end, and fatal initialization errors.
- Stop accepting manager requests before destroying shared state.
- Stop the Router job before `app_state` is destroyed.
- Join the log reader and manager threads before process exit.
- Do not detach threads.

Add this refactor separately and prove that browser mode behaves exactly as before. Do not combine an untested lifecycle rewrite with the first WebView2 window.

## 7. WebView2 SDK integration

Use the official `Microsoft.Web.WebView2` NuGet package and pin an explicit release version in the build configuration.

Recommended build policy:

1. Add a Windows-only CMake option such as `SPARKMOE_WEBVIEW2`.
2. Keep the option disabled on non-Windows platforms.
3. Restore the pinned SDK in `scripts/build-windows.ps1` or a dedicated helper.
4. Verify the downloaded NuGet package and retain its license notice.
5. Add the SDK include directory to the SparkMoE target.
6. Prefer the static WebView2 loader when its license and supported toolchain are verified. Otherwise package `WebView2Loader.dll` beside `SparkMoE.exe`.
7. Do not download dependencies implicitly during a normal CMake configure.
8. Record the SDK version in the release manifest.

Keep the WebView2 host in SparkMoE-owned files. Do not place it in upstream llama.cpp UI or server files.

Suggested files after design approval:

```text
tools/sparkmoe/windows-webview.h
tools/sparkmoe/windows-webview.cpp
```

Do not add these files until the lifecycle refactor and public behavior are approved.

## 8. Runtime detection and fallback

Before creating the environment, call the official runtime version API. Treat a missing or invalid version as WebView2 unavailable.

Fallback order:

1. Start the SparkMoE manager and Router normally.
2. Attempt WebView2 initialization.
3. If the Runtime is missing or initialization fails, write the HRESULT and stage to the SparkMoE log.
4. Show one concise native error message.
5. Open the existing UI in the default browser.
6. Keep the notification-area icon available.

Do not fail model serving only because the desktop shell failed.

Do not silently download or install the Runtime from `SparkMoE.exe`. Runtime installation belongs in the installer and requires a clear user-facing action.

## 9. Installer behavior

The normal installer should use the Evergreen Runtime.

During installation:

1. Detect the WebView2 Runtime using the official API or documented registry locations.
2. If present, continue without downloading anything.
3. If missing, explain that SparkMoE can still use the default browser.
4. Offer an explicit action to install the Microsoft Evergreen bootstrapper.
5. Run the bootstrapper per-user because the SparkMoE installer uses `PrivilegesRequired=lowest`.
6. If installation is declined or fails, finish installing SparkMoE and retain browser fallback.

Portable ZIP behavior:

- Do not include the full Runtime.
- Detect the Runtime at launch.
- Fall back to the default browser when unavailable.
- Include troubleshooting instructions and the official Microsoft download URL.

An optional offline package may include the Evergreen Standalone Installer, but it must have a distinct filename and documented size. Do not make the normal portable ZIP hundreds of megabytes larger.

## 10. Window behavior

Create one standard resizable top-level window.

Minimum behavior:

- Title: `SparkMoE`.
- Use `SparkMoE.ico` for the small and large window icons.
- Restore the last valid size and position.
- Validate restored bounds against current monitors.
- Use a sensible minimum size so the chat composer remains usable.
- Resize the WebView2 controller on `WM_SIZE`.
- Apply the current Windows theme to the title bar when supported.
- Preserve keyboard navigation and standard shortcuts.
- Set focus to WebView2 after the controller is ready.
- Show a native startup surface until the Router and WebView2 are ready.

Recommended close policy:

- Default: closing the main window exits SparkMoE and unloads models.
- Optional future setting: `Keep running in the notification area`.
- Never keep models loaded invisibly unless the user enabled that setting.
- Notification-area Exit always stops the Router, all model processes, WebView2, and the manager.

Do not use a fake invisible taskbar window. The taskbar button must belong to the real SparkMoE window.

## 11. Single-instance activation

Keep the existing named mutex.

When a second `SparkMoE.exe` starts:

1. Query the existing manager on port 18080.
2. Request native activation through a small internal callback or registered Windows message.
3. Restore the window if minimized.
4. bring it to the foreground without opening a second WebView2 profile.
5. Fall back to opening the default browser if native activation is unavailable.

The existing `/api/open` endpoint can be extended to call a registered native activation callback. Do not add another local network port.

## 12. User data and conversation continuity

Use one explicit writable user data folder:

```text
%LOCALAPPDATA%\SparkMoE\WebView2
```

Do not use the directory beside `SparkMoE.exe`. The portable package may be extracted under a read-only or protected location.

The Web UI stores conversation and UI state in browser storage. Moving from the user's default browser to WebView2 creates a new storage profile, so existing browser conversations do not automatically appear in the desktop shell.

Before release, choose and document one policy:

- Accept a new desktop-only conversation store, or
- Add an explicit export/import flow using the existing Web UI data model.

Do not read another browser's profile database directly. Do not copy passwords, cookies, or unrelated browser state.

Persist the WebView2 user data folder across normal upgrades. Remove it on uninstall only when the user explicitly selects deletion of local application data.

## 13. Navigation and security policy

Treat all WebView2 content as untrusted until its origin is validated.

Required rules:

- Allow top-level navigation only to `http://127.0.0.1:<current-router-port>` and the equivalent current loopback origin chosen by SparkMoE.
- Reject unexpected frames and top-level redirects.
- Open approved external `http` and `https` links in the user's default browser.
- Reject `file:`, `javascript:`, `data:`, custom schemes, UNC paths, and non-loopback hosts inside WebView2 unless a later design explicitly requires them.
- Do not add native host objects to JavaScript for the first implementation.
- Do not use `ExecuteScript` for lifecycle or model control when existing HTTP APIs already provide the operation.
- Disable DevTools in release builds unless a documented diagnostics setting enables them.
- Disable default script dialogs unless the UI has a verified need for them.
- Keep the WebView2 host at normal user integrity. Never run it elevated.
- Preserve the manager API origin checks.

If a native bridge is added later, validate the current document origin for every message and use a narrow JSON schema.

## 14. Browser capability inventory

Verify these existing UI behaviors inside WebView2 before replacing browser mode:

- Chat streaming and reconnect.
- IndexedDB conversation persistence.
- Service worker update behavior.
- File upload and drag-and-drop.
- Clipboard read and write.
- Audio recording and microphone permission.
- Image, audio, video, PDF, SVG, and Mermaid rendering.
- File downloads for diagnostics and logs.
- External documentation links.
- Model directory picker.
- Browser history, refresh, and deep routes such as `/#/sparkmoe`.
- Dark and light theme updates.

For permissions, default to deny and add explicit handling only for capabilities SparkMoE uses. Permission prompts must name the requested capability and remember a choice only when appropriate.

## 15. Native folder picker ownership

The manager currently selects `GetForegroundWindow()` as the owner of `IFileOpenDialog`. That is acceptable for external browser mode but is not a stable contract for WebView2.

After the desktop window exists:

- Store or register its HWND through a narrow Windows-only interface.
- Use that exact HWND as the folder dialog owner.
- Clear the reference before destroying the window.
- Fall back to no owner only when browser mode is active.

Validate focus restoration after both selection and cancellation.

## 16. Startup sequence

Recommended order:

1. Acquire the single-instance mutex.
2. Initialize COM as `COINIT_APARTMENTTHREADED` on the UI thread.
3. Load configuration and start the manager listener.
4. Create the main Win32 window and notification-area icon.
5. Start the Router.
6. Show native startup status while polling bounded Router readiness.
7. Detect the WebView2 Runtime.
8. Create the WebView2 environment with the explicit user data folder.
9. Create the controller as a child of the main window.
10. Register navigation, permission, download, process-failure, and new-window handlers.
11. Navigate to the loopback Router URL.
12. Replace the startup surface after navigation completes.

Never block the UI thread while waiting for Router health or WebView2 callbacks.

## 17. Shutdown sequence

Use one idempotent shutdown coordinator:

1. Mark shutdown requested.
2. Reject new start or restart actions.
3. Close WebView2 and release controller and environment references on the UI thread.
4. Stop the manager listener.
5. Stop the Router job and all model processes.
6. Join the log reader and manager worker threads.
7. Remove the notification-area icon.
8. Destroy the native window.
9. Call `CoUninitialize` on the UI thread.
10. Release the single-instance mutex.

Handle repeated requests from the Web UI, the window close button, the notification-area icon, and Windows session shutdown without double-closing handles.

## 18. Failure handling

Write structured stage information to the normal SparkMoE session log for:

- Runtime detection.
- Environment creation.
- Controller creation.
- Navigation start and completion.
- WebView process failure.
- Router readiness timeout.
- Fallback browser launch.

Do not log prompts, generated text, API keys, browser storage, or full private paths.

On WebView2 process failure:

- Attempt one bounded controller recreation when the manager and Router remain healthy.
- If recreation fails, open the default browser and keep the notification-area controls.
- Do not restart loaded models solely because the rendering process failed.

## 19. Implementation stages

### Stage A: lifecycle preparation

- Extract the manager listener into a joinable worker.
- Centralize idempotent shutdown.
- Retain default-browser behavior.
- Add focused lifecycle tests using the existing test infrastructure.

Acceptance gate: browser mode, manager API, Start, Stop, Exit, second launch, logs, and notification-area behavior remain unchanged.

### Stage B: opt-in WebView2 prototype

- Add the pinned SDK and CMake option.
- Create the window and controller.
- Navigate to the local UI.
- Keep browser fallback.
- Hide the feature behind an explicit development flag.

Acceptance gate: chat, SparkMoE management, downloads, uploads, persistence, and shutdown work in a local Release build.

### Stage C: security and UX completion

- Enforce navigation and permission policy.
- Add startup and failure surfaces.
- Complete single-instance activation.
- Make the folder picker owned by the native window.
- Complete keyboard, DPI, theme, and accessibility behavior.

Acceptance gate: no remote page can access a native bridge or remain inside the trusted application window.

### Stage D: packaging

- Add Runtime detection to Inno Setup.
- Package the loader and notices.
- Add portable fallback instructions.
- Record versions and hashes in the release manifest.
- Preserve the browser-only recovery route.

Acceptance gate: clean Windows 10, Windows 11, installed, portable, Runtime-present, and Runtime-missing scenarios pass.

### Stage E: default enablement

- Enable WebView2 by default only after the complete validation matrix passes.
- Retain a documented browser fallback switch for recovery.
- Keep Linux behavior unchanged.

## 20. Validation matrix

Run at least the following tests before release:

### Build and static checks

- MSVC AVX2 Release build.
- MSVC generic Release build.
- Existing SparkMoE server regression.
- Relevant UI checks and production build.
- Upstream integration inventory check.
- `git diff --check`.

### Windows runtime

- Windows 11 with Evergreen Runtime.
- Windows 10 with Evergreen Runtime.
- Runtime missing: browser fallback and clear guidance.
- Installed and portable layouts.
- Unicode Windows user and model paths.
- Standard user without elevation.
- Explorer restart while SparkMoE is running.
- Windows logoff and shutdown.

### Window and lifecycle

- First launch opens one window.
- Second launch activates the same window.
- Minimize, restore, Alt+Tab, taskbar pin, and multi-monitor restore.
- Close with no model loaded.
- Close while a model is loading.
- Close while generation is active.
- Notification-area Open, Start, Stop, and Exit.
- Web UI Exit and manager API shutdown.
- Router crash and WebView render-process crash.

### UI capabilities

- New and existing conversations.
- Streaming cancellation and reconnect.
- Model load and unload.
- Settings and profile persistence.
- Folder picker focus and cancellation.
- Diagnostics download.
- File upload and drag-and-drop.
- Clipboard and microphone permission.
- External link handoff.
- Dark and light themes at 100, 125, 150, and 200 percent DPI.

### Packaging

- ZIP required-file inventory.
- Installer install, upgrade, repair, and uninstall.
- Release manifest hashes.
- PE GUI subsystem verification.
- Signature verification when release signing exists.
- No model, token, log, WebView profile, or personal path in the package.

## 21. Rollback

Keep external browser launch as a complete fallback until at least one released version has proven the WebView2 path.

The rollback switch should bypass WebView2 before COM or Runtime initialization and use the current `ShellExecuteW` path. A WebView2 failure must never require uninstalling SparkMoE to reach the UI.

Do not delete the user's WebView2 data folder during rollback. Preserve it for a later fixed release unless the user explicitly requests removal.

## 22. Release evidence boundary

A successful compile does not prove WebView2 behavior. A production claim requires a packaged Release build and live validation of the native window, taskbar identity, Runtime detection, navigation restrictions, UI capabilities, lifecycle, and fallback.

Report separately:

- Source and static checks.
- Build results.
- Automated runtime tests.
- Live Windows UI checks.
- ZIP and installer checks.
- Signature status.

Do not describe an unsigned local installer as a trusted production release.
