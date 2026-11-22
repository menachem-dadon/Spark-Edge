# Spark Edge on Windows

Spark Edge for Windows is the desktop edition. Spark Edge for Android is the mobile edition of the same product.

## Preparing an application release

Integrate the current official llama.cpp master before building a release. Record its full commit SHA in UPSTREAM_BASE.txt and review the changes in the Spark Edge integration points. The build records this SHA. Application builds and independently distributed engine builds use the same modified engine source. Check that all target engines use the same SHA for one release batch.

scripts/build-windows.ps1 accepts -Backend cpu, cuda, vulkan, hip, or sycl. Install the matching compiler and SDK for the chosen backend. The CPU installer includes the local engine for offline use.

The installer keeps its existing Inno Setup AppId, the default install path (%LOCALAPPDATA%\Programs\SparkMoE), and the launcher name (SparkMoE.exe) for upgrades and existing shortcuts. The user data root (%LOCALAPPDATA%\SparkMoE) contains settings, the WebView2 profile, installed engines and logs. Keeping it in place preserves these files and chat history across upgrades.

## Publishing an engine separately

After building and validating the target backend, run scripts/package-engine-windows.ps1 with the build directory and backend. It emits one JSON manifest and the matching binaries. The package name contains the backend and the upstream commit. Pass required redistributable DLLs with -RuntimeLibraries. Keep project and third-party notices with the release.

Publish those files together as assets of a release in [menachem-dadon/Spark-Edge](https://github.com/menachem-dadon/Spark-Edge/releases). An engine release does not require a new Spark Edge installer. The manager reads GitHub's metadata over HTTPS, accepts only matching assets from that repository and verifies their SHA-256 digests against the release and package manifest before placing a copy in the installed engines directory. The local catalog does not depend on an existing running engine.

In Spark Edge, open Engines, check for updates and choose Install and activate. The manager verifies and installs the package, then switches the default engine. If a generation request is active, the package stays installed and you can activate it when the request finishes. The desktop window stays open during activation. The manager checks the new router and tries to restore the most recently loaded model. On failure it tries previously working engines and the bundled engine.

Each model profile can use an installed engine independently. With a router that supports per-model fallback, a failed worker launch tries a locally installed package for the same backend and then the bundled worker. Other loaded workers keep their own processes during that model's retry.

Use `-EngineOnly` to compile a runtime for a given backend without rebuilding the desktop UI. Real GPU coverage requires a matching adapter and driver. Run `scripts/test-sparkmoe-server.py --bin-dir <bin> --log-dir <logs> --gpu-device <device>` with a GPU build to exercise the expert pager. The test creates its own small GGUF fixture and checks that expert data was allocated and uploaded to the device.

## Expert cache settings

The Spark Edge RAM budget is --moe-paging-cache-mib. The upstream --moe-cache-mib flag sets the device cache budget. In explicit Spark Edge paging mode, eligible GPU layers can mirror their active CPU slots in a bounded device cache. A resident GPU copy is reused when the corresponding host slot has not changed generation. The host cache reads missing experts from storage. Paths unsupported by the GPU or beyond the budget use the host pool.

## Release qualification

Use the Windows regression suite for preset loading, GGUF splits, exact routing, inference and the manager. Verify the engine switch with an invalid candidate and with a loaded model, and verify the installer upgrade with a legacy data profile. For every advertised GPU backend, confirm device execution during explicit expert paging on appropriate physical hardware. Compare model correctness and latency between CPU and GPU and measure storage reads, RAM and VRAM.
