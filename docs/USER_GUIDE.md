# User guide

Use the launcher for the simplest local workflow. It listens only on `127.0.0.1:18080`, stores form preferences in browser local storage, validates that the model is a readable `.gguf`, and starts the sibling `llama-server` without a command shell.

The hardware recommendation reserves substantial RAM headroom and forces one parallel inference slot. It is a conservative starting point, not proof that a particular model fits.

For a split model, select the first conventional shard; upstream discovers the remaining shards. Keep all shards together and do not rename only one file.

Use `os` mode first if explicit mode reports an unsupported architecture. Use `explicit` only for Qwen3.5/Qwen3.6 MoE and start with the `16gb-safe.json` preset values. Reduce context/ubatch size before reducing the expert cache below the model's required active-expert minimum.

The launcher does not download models. Upstream CLI/server Hugging Face options remain available, but large downloads should be deliberate and model licenses must be reviewed first.
