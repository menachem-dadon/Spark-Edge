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

- `GET /qwenstream/v1/status`
- `GET /qwenstream/v1/paging`
- `POST /qwenstream/v1/paging/reset-stats`
- `GET /qwenstream/v1/config`

These routes use the server's normal API-key middleware. Health and model-list routes retain upstream public-route behavior. Paging values come from the active llama context; when explicit paging is off, `enabled` is false and counters are zero.

The normal OpenAI-compatible `/v1/chat/completions` and `/v1/responses` endpoints are unchanged.
