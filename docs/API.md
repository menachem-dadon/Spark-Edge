# API

## C API

`llama_model_params` and `llama_context_params` contain `llama_moe_paging_params`.

```c
bool llama_model_moe_paging_capabilities(const llama_model *, llama_moe_paging_capabilities *);
bool llama_context_moe_paging_get_stats(const llama_context *, llama_moe_paging_stats *);
void llama_context_moe_paging_reset_stats(llama_context *);
bool llama_context_moe_paging_set_expert_pinned(llama_context *, int32_t layer, int32_t expert, bool pinned);
```

Stats are monotonic until reset and include cache hits/misses, bytes and count of reads, cumulative read latency, evictions, mutex waits, and duplicate IDs within a routing request.

Pin requests can be registered before an expert is resident. When that expert is loaded, its slot is excluded from LRU victim selection until unpinned. The function returns false for unsupported layers, invalid IDs, or contexts without explicit paging.

## Server management endpoints

- `GET /sparkmoe/v1/status`
- `GET /sparkmoe/v1/paging`
- `POST /sparkmoe/v1/paging/reset-stats`
- `GET /sparkmoe/v1/config`

These routes use the server's normal API-key middleware. In Router mode add `?model=<id>` to GET requests and a `model` field to POST bodies. The Router forwards the request to that model process. Health and model-list routes retain upstream public-route behavior. Paging values come from the selected llama context; when explicit paging is off, `enabled` is false and counters are zero.

The normal OpenAI-compatible `/v1/chat/completions` and `/v1/responses` endpoints are unchanged.

`/sparkmoe/v1/config` also reports the effective total and per-slot context size, training context, logical and physical batch sizes, KV cache types, parallel slots, context-shift state, and active MTP draft parameters.

## Local manager API

The hidden manager exposes a loopback-only API on `127.0.0.1:18080`:

- `GET /api/status`, `/api/config`, `/api/recommendation`, `/api/diagnostics`
- `POST /api/config` to persist Router port, models directory, automatic loading, and `models-max`, then restart the Router
- `GET`, `POST`, and `DELETE /api/model-profile?model=<id>` for persistent per-model process profiles
- `POST /api/browse-model-directory`, `/api/start`, `/api/stop`, `/api/restart`
- `GET /api/log-sessions`, `/api/logs`, and `/api/logs/download` for complete persistent session logs

The directory browser uses the native Windows folder picker. Requests with a browser `Origin` header are accepted only from the manager or the current/previous configured loopback Web UI port.

## OpenAI-compatible thinking control

The Router requires a `model` field and forwards generation fields unchanged. The saved profile controls process allocation; it does not lock request behavior. For supported thinking templates, a caller can disable thinking for one request with `reasoning_effort: "none"` and `chat_template_kwargs: {"enable_thinking": false}`. This does not reload the model.
