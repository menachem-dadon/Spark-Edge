# Third-party notices

QwenStream is based on [llama.cpp](https://github.com/ggml-org/llama.cpp), which is MIT licensed. Its license and bundled third-party license files are preserved in this repository and release packages.

The expert streaming design was informed by publicly available work in [Colibri](https://github.com/JustVugg/colibri) and the `moe-expert-residency` proof-of-concept branch. QwenStream's CPU implementation was written independently against current upstream interfaces; no Colibri or proof-of-concept source file was copied into the paging subsystem.

The local dashboard uses the `cpp-httplib` and `nlohmann/json` copies already distributed by upstream llama.cpp under their respective licenses.

Qwen, Gemma, and other model weights are not bundled. Users must obtain weights separately and comply with the exact model repository's license and use terms.
