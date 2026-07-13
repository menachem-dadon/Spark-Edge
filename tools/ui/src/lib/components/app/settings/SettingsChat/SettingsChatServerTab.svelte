<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
<!-- Required Notice: Copyright (c) 2026 SparkMoE contributors. -->
<script lang="ts">
	import { onMount } from 'svelte';
	import { Button } from '$lib/components/ui/button';
	import {
		AlertCircle,
		CheckCircle2,
		ExternalLink,
		Gauge,
		LoaderCircle,
		RefreshCw,
		Sparkles
	} from '@lucide/svelte';

	const MANAGER_URL = 'http://127.0.0.1:18080';
	const CONTEXT_PRESETS = [4096, 8192, 16384, 32768, 65536];

	type ServerConfig = {
		model_directory: string;
		model: string;
		mode: string;
		cache: number;
		slots: number;
		threads: number;
		port: number;
		ctx_size: number;
		batch_size: number;
		ubatch_size: number;
		parallel: number;
		ctx_checkpoints: number;
		cache_ram_mib: number;
		cache_type_k: string;
		cache_type_v: string;
		context_shift: boolean;
		mtp_enabled: boolean;
		mtp_draft_tokens: number;
		mtp_min_tokens: number;
		mtp_p_min: number;
		mtp_threads: number;
		mtp_backend_sampling: boolean;
	};

	type ManagerStatus = {
		running: boolean;
		server_ready: boolean;
		phase: string;
		pid: number;
		port: number;
		last_error?: string;
	};

	const defaults: ServerConfig = {
		model_directory: '',
		model: '',
		mode: 'os',
		cache: 2048,
		slots: 0,
		threads: 4,
		port: 8080,
		ctx_size: 0,
		batch_size: 2048,
		ubatch_size: 512,
		parallel: 1,
		ctx_checkpoints: 4,
		cache_ram_mib: 0,
		cache_type_k: 'f16',
		cache_type_v: 'f16',
		context_shift: true,
		mtp_enabled: false,
		mtp_draft_tokens: 3,
		mtp_min_tokens: 0,
		mtp_p_min: 0,
		mtp_threads: 0,
		mtp_backend_sampling: true
	};

	let config = $state<ServerConfig>({ ...defaults });
	let status = $state<ManagerStatus | null>(null);
	let connected = $state(false);
	let busy = $state(false);
	let error = $state('');
	let effectiveContext = $state<number | null>(null);

	function modelName(path: string) {
		return path.split(/[\\/]/).pop() || 'No model selected';
	}

	async function managerFetch<T>(path: string, body?: unknown): Promise<T> {
		const response = await fetch(`${MANAGER_URL}${path}`, {
			method: body === undefined ? 'GET' : 'POST',
			headers: body === undefined ? undefined : { 'content-type': 'application/json' },
			body: body === undefined ? undefined : JSON.stringify(body)
		});
		const payload = (await response.json()) as T & { error?: string };
		if (!response.ok) throw new Error(payload.error || `Manager returned HTTP ${response.status}`);
		return payload;
	}

	async function refresh() {
		try {
			const [nextConfig, nextStatus] = await Promise.all([
				managerFetch<ServerConfig>('/api/config'),
				managerFetch<ManagerStatus>('/api/status')
			]);
			config = { ...defaults, ...nextConfig };
			status = nextStatus;
			connected = true;
			error = '';
			if (nextStatus.server_ready) {
				const active = await managerFetch<{ context_size_per_slot?: number; context_size?: number }>(
					'/api/server-config'
				);
				effectiveContext = active.context_size_per_slot ?? active.context_size ?? null;
			} else {
				effectiveContext = null;
			}
		} catch (cause) {
			connected = false;
			error = cause instanceof Error ? cause.message : String(cause);
		}
	}

	async function waitUntilReady(port: number) {
		for (let attempt = 0; attempt < 180; attempt += 1) {
			const next = await managerFetch<ManagerStatus>('/api/status');
			status = next;
			if (next.server_ready) {
				if (window.location.port !== String(port)) {
					window.location.href = `http://127.0.0.1:${port}/#/settings/server`;
				} else {
					window.location.reload();
				}
				return;
			}
			if (!next.running && next.phase === 'exited') {
				throw new Error(next.last_error || 'The server exited while loading the model.');
			}
			await new Promise((resolve) => setTimeout(resolve, 1000));
		}
		throw new Error('The model did not become ready within three minutes. Check the dashboard log.');
	}

	async function applyAndRestart() {
		busy = true;
		error = '';
		try {
			await managerFetch(status?.running ? '/api/restart' : '/api/start', config);
			await waitUntilReady(config.port);
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
			await refresh();
		} finally {
			busy = false;
		}
	}

	async function stopServer() {
		busy = true;
		error = '';
		try {
			await managerFetch('/api/stop', {});
			await refresh();
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
		} finally {
			busy = false;
		}
	}

	onMount(() => {
		void refresh();
	});
</script>

<div class="space-y-6">
	{#if connected}
		<div class="rounded-2xl border border-border bg-gradient-to-br from-muted/50 to-muted/20 p-4 shadow-sm">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div class="flex min-w-0 items-center gap-3">
					{#if status?.server_ready}
						<CheckCircle2 class="h-5 w-5 shrink-0 text-emerald-500" />
					{:else if status?.running}
						<LoaderCircle class="h-5 w-5 shrink-0 animate-spin text-amber-500" />
					{:else}
						<AlertCircle class="h-5 w-5 shrink-0 text-muted-foreground" />
					{/if}
					<div class="min-w-0">
						<p class="font-medium">
							{status?.server_ready ? 'Model ready' : status?.running ? 'Model is loading' : 'Server stopped'}
						</p>
						<p class="truncate text-xs text-muted-foreground" title={config.model}>
							{modelName(config.model)} | port {status?.port || config.port}
							{#if effectiveContext} | {effectiveContext.toLocaleString()} context tokens{/if}
						</p>
					</div>
				</div>
				<Button variant="outline" size="sm" onclick={refresh} disabled={busy}>
					<RefreshCw class="h-4 w-4" /> Refresh
				</Button>
			</div>
		</div>
	{:else}
		<div class="rounded-xl border border-amber-500/40 bg-amber-500/10 p-4 text-sm">
			<p class="font-medium">SparkMoE Control Center is not available</p>
			<p class="mt-1 text-muted-foreground">
				Start the model through SparkMoE.exe, then return to this page.
			</p>
			<Button class="mt-3" variant="outline" onclick={refresh}>Try again</Button>
		</div>
	{/if}

	<section class="rounded-2xl border border-border/70 bg-card p-5 shadow-sm">
		<div class="mb-5 flex items-start gap-3">
			<div class="rounded-xl bg-sky-500/10 p-2 text-sky-500"><Gauge class="h-5 w-5" /></div>
			<div>
				<h4 class="font-semibold">Context length</h4>
				<p class="mt-1 text-sm text-muted-foreground">
					Enter the exact number of tokens available to this chat. A larger value uses more RAM.
				</p>
			</div>
		</div>

		<div class="grid gap-4 sm:grid-cols-2">
			<label class="grid gap-2 text-sm">
				<span class="font-medium">Context tokens</span>
				<input
					type="number"
					min="0"
					max="1048576"
					step="1024"
					class="rounded-lg border border-input bg-background px-3 py-2.5 text-base"
					bind:value={config.ctx_size}
				/>
				<span class="text-xs text-muted-foreground">Use 0 only when you want the model default.</span>
			</label>
			<label class="grid gap-2 text-sm">
				<span class="font-medium">When the context is full</span>
				<select
					class="rounded-lg border border-input bg-background px-3 py-2.5"
					value={config.context_shift ? 'shift' : 'stop'}
					onchange={(event) => (config.context_shift = event.currentTarget.value === 'shift')}
				>
					<option value="shift">Forget the oldest messages and continue</option>
					<option value="stop">Stop when the limit is reached</option>
				</select>
				<span class="text-xs text-muted-foreground">This controls conversation history, not output length.</span>
			</label>
		</div>

		<div class="mt-4 flex flex-wrap gap-2">
			{#each CONTEXT_PRESETS as tokens}
				<Button
					variant={config.ctx_size === tokens ? 'default' : 'outline'}
					size="sm"
					onclick={() => (config.ctx_size = tokens)}
				>
					{tokens >= 1024 ? `${tokens / 1024}K` : tokens}
				</Button>
			{/each}
		</div>
	</section>

	<section class="rounded-2xl border border-border/70 bg-card p-5 shadow-sm">
		<div class="mb-5 flex items-start gap-3">
			<div class="rounded-xl bg-violet-500/10 p-2 text-violet-500"><Sparkles class="h-5 w-5" /></div>
			<div>
				<h4 class="font-semibold">Multi-Token Prediction (MTP)</h4>
				<p class="mt-1 text-sm text-muted-foreground">
					MTP can accelerate generation when the selected GGUF includes a compatible prediction head.
				</p>
			</div>
		</div>

		<label class="flex items-start justify-between gap-4 rounded-xl border border-border/60 bg-muted/20 p-4 text-sm">
			<span>
				<b>Enable MTP for this model</b>
				<span class="mt-1 block text-xs text-muted-foreground">
					If startup reports that no MTP head exists, turn this off. MTP is optional and may not be faster on every CPU.
				</span>
			</span>
			<input class="mt-1 h-4 w-4 shrink-0" type="checkbox" bind:checked={config.mtp_enabled} />
		</label>

		{#if config.mtp_enabled}
			<label class="mt-4 grid max-w-sm gap-2 text-sm">
				<span class="font-medium">Draft tokens per step</span>
				<select class="rounded-lg border border-input bg-background px-3 py-2.5" bind:value={config.mtp_draft_tokens}>
					{#each [2, 3, 4, 5, 6, 8] as tokens}
						<option value={tokens}>{tokens}{tokens === 3 ? ' - recommended starting point' : ''}</option>
					{/each}
				</select>
			</label>
		{/if}
	</section>

	<details class="rounded-xl border border-border/60 bg-muted/10 p-4">
		<summary class="cursor-pointer text-sm font-medium">Advanced performance settings</summary>
		<p class="mt-3 text-xs text-muted-foreground">
			The defaults below are appropriate for most users. Change them only when benchmarking a known model.
		</p>
		<div class="mt-4 grid gap-4 sm:grid-cols-2">
			<label class="grid gap-2 text-sm"><span>Parallel chat slots</span><input type="number" min="1" max="32" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.parallel} /></label>
			<label class="grid gap-2 text-sm"><span>Logical batch size</span><input type="number" min="1" max="16384" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.batch_size} /></label>
			<label class="grid gap-2 text-sm"><span>Physical microbatch</span><input type="number" min="1" max="16384" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.ubatch_size} /></label>
			<label class="grid gap-2 text-sm"><span>Prompt cache RAM (MiB)</span><input type="number" min="-1" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.cache_ram_mib} /></label>
			<label class="grid gap-2 text-sm"><span>Context checkpoints</span><input type="number" min="0" max="128" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.ctx_checkpoints} /></label>
			<label class="grid gap-2 text-sm"><span>K cache precision</span><select class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.cache_type_k}>{#each ['f16','bf16','q8_0','q5_1','q5_0','q4_1','q4_0','iq4_nl'] as type}<option value={type}>{type}</option>{/each}</select></label>
			<label class="grid gap-2 text-sm"><span>V cache precision</span><select class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.cache_type_v}>{#each ['f16','bf16','q8_0','q5_1','q5_0','q4_1','q4_0','iq4_nl'] as type}<option value={type}>{type}</option>{/each}</select></label>
			{#if config.mtp_enabled}
				<label class="grid gap-2 text-sm"><span>Minimum draft tokens</span><input type="number" min="0" max="64" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.mtp_min_tokens} /></label>
				<label class="grid gap-2 text-sm"><span>Minimum draft probability</span><input type="number" min="0" max="1" step="0.01" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.mtp_p_min} /></label>
				<label class="grid gap-2 text-sm"><span>MTP CPU threads (0 = automatic)</span><input type="number" min="0" max="128" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.mtp_threads} /></label>
			{/if}
		</div>
		{#if config.mtp_enabled}
			<label class="mt-4 flex items-center gap-3 text-sm"><input type="checkbox" bind:checked={config.mtp_backend_sampling} /> Use backend sampling when supported</label>
		{/if}
	</details>

	<div class="rounded-xl border border-sky-500/30 bg-sky-500/10 p-3 text-sm text-muted-foreground">
		Context and MTP are startup settings. Applying changes reloads the model; conversations stay in this browser.
	</div>

	{#if error}
		<div class="rounded-lg border border-destructive/40 bg-destructive/10 p-3 text-sm text-destructive">{error}</div>
	{/if}

	<div class="flex flex-wrap gap-3 border-t border-border/40 pt-5">
		<Button onclick={applyAndRestart} disabled={!connected || busy || !config.model}>
			{#if busy}<LoaderCircle class="h-4 w-4 animate-spin" />{/if}
			{status?.running ? 'Apply and restart model' : 'Start model'}
		</Button>
		<Button variant="destructive" onclick={stopServer} disabled={!connected || busy || !status?.running}>Stop model</Button>
		<Button variant="outline" onclick={() => window.open(MANAGER_URL, '_blank')}>
			<ExternalLink class="h-4 w-4" /> Open dashboard
		</Button>
	</div>
</div>
