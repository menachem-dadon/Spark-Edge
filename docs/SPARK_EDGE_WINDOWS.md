# Spark Edge on Windows

Spark Edge for Windows is the desktop edition. Spark Edge for Android is the mobile edition of the same product.

## Preparing an application release

Integrate the current official llama.cpp master before building a release. Record its full commit SHA in UPSTREAM_BASE.txt and review the changes in the Spark Edge integration points. The build records this SHA. Application builds and independently distributed engine builds use the same modified engine source. Check that all target engines use the same SHA for one release batch.

scripts/build-windows.ps1 accepts -Backend cpu, cuda, vulkan, hip, or sycl. Install the matching compiler and SDK for the chosen backend. The CPU installer includes the local engine for offline use.

The installer keeps the existing Windows installation ID, data directory and WebView2 profile so an upgrade preserves users' models, settings and chats.

## Publishing an engine separately

After building and validating the target backend, run scripts/package-engine-windows.ps1 with the build directory and backend. It emits one JSON manifest and the matching binaries. The package name contains the backend and the upstream commit. Pass required redistributable DLLs with -RuntimeLibraries. Keep project and third-party notices with the release.

Publish those files together as assets of a release in menachem-dadon/SparkMoE. An engine release does not require a new Spark Edge installer. The manager reads GitHub's metadata over HTTPS, accepts only matching assets from that repository and verifies their SHA-256 digests against the release and package manifest before placing a copy in the installed engines directory. The local catalog does not depend on an existing running engine.

In Spark Edge, open Engines to check for downloads, install a package or switch the default engine. Each model profile can use an installed engine independently. A switch waits for active requests, restarts the engine process and checks health before saving the configuration. The application and its data stay open.

## Expert cache settings

The Spark Edge RAM budget is --moe-paging-cache-mib. The upstream --moe-cache-mib flag sets the device cache budget. In explicit Spark Edge paging mode, eligible GPU layers can mirror their active CPU slots in a bounded device cache. A resident GPU copy is reused when the corresponding host slot has not changed generation. The host cache reads missing experts from storage. Paths unsupported by the GPU or beyond the budget use the host pool.

## Release qualification

Use the Windows regression suite for preset loading, GGUF splits, exact routing, inference and the manager. Verify the engine switch with an invalid candidate and with a loaded model, and verify the installer upgrade with a legacy data profile. For every advertised GPU backend, confirm device execution during explicit expert paging on appropriate physical hardware. Compare model correctness and latency between CPU and GPU and measure storage reads, RAM and VRAM.
