<script lang="ts">
	import { onMount } from 'svelte';
	import { Button } from '$lib/components/ui/button';
	import { AlertCircle, CheckCircle2, ExternalLink, LoaderCircle, RefreshCw } from '@lucide/svelte';

	const MANAGER_URL = 'http://127.0.0.1:18080';

	type ServerConfig = {
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
		model: '',
		mode: 'auto',
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
		throw new Error('The model did not become ready within three minutes. Check the server log.');
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
		<div class="flex flex-wrap items-center justify-between gap-3 rounded-xl border border-border bg-muted/30 p-4">
			<div class="flex items-center gap-3">
				{#if status?.server_ready}
					<CheckCircle2 class="h-5 w-5 text-emerald-500" />
				{:else if status?.running}
					<LoaderCircle class="h-5 w-5 animate-spin text-amber-500" />
				{:else}
					<AlertCircle class="h-5 w-5 text-muted-foreground" />
				{/if}
				<div>
					<p class="font-medium">
						{status?.server_ready ? 'Server ready' : status?.running ? 'Model is loading' : 'Server stopped'}
					</p>
					<p class="text-xs text-muted-foreground">
						PID {status?.pid || '-'} | port {status?.port || config.port}
						{#if effectiveContext} | {effectiveContext.toLocaleString()} tokens per chat{/if}
					</p>
				</div>
			</div>
			<Button variant="outline" size="sm" onclick={refresh} disabled={busy}>
				<RefreshCw class="h-4 w-4" /> Refresh
			</Button>
		</div>
	{:else}
		<div class="rounded-xl border border-amber-500/40 bg-amber-500/10 p-4 text-sm">
			<p class="font-medium">QwenStream Control Center is not available</p>
			<p class="mt-1 text-muted-foreground">
				Start the server through QwenStream.exe. The manager listens locally on port 18080 and allows
				this page to apply startup-only settings safely.
			</p>
			<Button class="mt-3" variant="outline" onclick={refresh}>Try again</Button>
		</div>
	{/if}

	<div class="space-y-3">
		<h4 class="font-medium">Model and expert paging</h4>
		<p class="text-sm text-muted-foreground">
			Explicit mode keeps routed expert weights on the GGUF file and reads selected experts into a
			bounded RAM cache. Auto mode uses it when a cache or slot limit is supplied.
		</p>
		<label class="grid gap-2 text-sm">
			<span>Local GGUF model path</span>
			<input class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.model} />
		</label>
		<div class="grid gap-4 sm:grid-cols-2">
			<label class="grid gap-2 text-sm"><span>Paging mode</span><select class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.mode}><option value="auto">Auto</option><option value="explicit">Explicit SSD cache</option><option value="os">Operating system / mmap</option><option value="off">Resident / off</option></select></label>
			<label class="grid gap-2 text-sm"><span>Server port</span><input type="number" min="1024" max="65535" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.port} /></label>
			<label class="grid gap-2 text-sm"><span>Expert cache (MiB)</span><input type="number" min="0" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.cache} /></label>
			<label class="grid gap-2 text-sm"><span>Expert slots per layer (0 = use budget)</span><input type="number" min="0" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.slots} /></label>
			<label class="grid gap-2 text-sm"><span>SSD read workers</span><input type="number" min="1" max="128" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.threads} /></label>
		</div>
	</div>

	<div class="space-y-3 border-t border-border/40 pt-5">
		<h4 class="font-medium">Context and memory</h4>
		<p class="text-sm text-muted-foreground">
			These values allocate memory when the model starts. Context size 0 uses the model default. The
			physical microbatch cannot exceed the logical batch size.
		</p>
		<div class="grid gap-4 sm:grid-cols-2">
			<label class="grid gap-2 text-sm"><span>Context tokens</span><input type="number" min="0" max="1048576" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.ctx_size} /></label>
			<label class="grid gap-2 text-sm"><span>Parallel chat slots (explicit mode requires 1)</span><input type="number" min="1" max="32" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.parallel} /></label>
			<label class="grid gap-2 text-sm"><span>Logical batch size</span><input type="number" min="1" max="16384" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.batch_size} /></label>
			<label class="grid gap-2 text-sm"><span>Physical microbatch</span><input type="number" min="1" max="16384" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.ubatch_size} /></label>
			<label class="grid gap-2 text-sm"><span>Prompt cache RAM (MiB, 0 = off)</span><input type="number" min="-1" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.cache_ram_mib} /></label>
			<label class="grid gap-2 text-sm"><span>Context checkpoints</span><input type="number" min="0" max="128" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.ctx_checkpoints} /></label>
			<label class="grid gap-2 text-sm"><span>K cache precision</span><select class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.cache_type_k}>{#each ['f16','bf16','q8_0','q5_1','q5_0','q4_1','q4_0','iq4_nl'] as type}<option value={type}>{type}</option>{/each}</select></label>
			<label class="grid gap-2 text-sm"><span>V cache precision</span><select class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.cache_type_v}>{#each ['f16','bf16','q8_0','q5_1','q5_0','q4_1','q4_0','iq4_nl'] as type}<option value={type}>{type}</option>{/each}</select></label>
		</div>
		<label class="flex items-start gap-3 rounded-lg border border-border/60 p-3 text-sm">
			<input class="mt-1" type="checkbox" bind:checked={config.context_shift} />
			<span><b>Keep recent messages when context fills</b><span class="mt-1 block text-xs text-muted-foreground">Context shift drops the oldest reusable tokens instead of ending generation.</span></span>
		</label>
	</div>

	<div class="space-y-3 border-t border-border/40 pt-5">
		<h4 class="font-medium">Multi-Token Prediction (MTP)</h4>
		<p class="text-sm text-muted-foreground">
			MTP uses the model's built-in prediction head through llama.cpp draft-mtp. Enabling it on a GGUF
			without a compatible head will produce a clear startup error. More draft tokens are not always faster.
		</p>
		<label class="flex items-start gap-3 rounded-lg border border-border/60 p-3 text-sm"><input class="mt-1" type="checkbox" bind:checked={config.mtp_enabled} /><span><b>Enable built-in MTP</b><span class="mt-1 block text-xs text-muted-foreground">Compatible with QwenStream expert paging; the draft context receives its own bounded expert cache.</span></span></label>
		<div class="grid gap-4 sm:grid-cols-2">
			<label class="grid gap-2 text-sm"><span>Maximum draft tokens</span><input type="number" min="1" max="64" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.mtp_draft_tokens} /></label>
			<label class="grid gap-2 text-sm"><span>Minimum draft tokens</span><input type="number" min="0" max="64" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.mtp_min_tokens} /></label>
			<label class="grid gap-2 text-sm"><span>Minimum draft probability</span><input type="number" min="0" max="1" step="0.01" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.mtp_p_min} /></label>
			<label class="grid gap-2 text-sm"><span>MTP CPU threads (0 = auto)</span><input type="number" min="0" max="128" class="rounded-md border border-input bg-background px-3 py-2" bind:value={config.mtp_threads} /></label>
		</div>
		<label class="flex items-center gap-3 text-sm"><input type="checkbox" bind:checked={config.mtp_backend_sampling} /> Use backend sampling when supported</label>
	</div>

	{#if error}
		<div class="rounded-lg border border-destructive/40 bg-destructive/10 p-3 text-sm text-destructive">{error}</div>
	{/if}

	<div class="flex flex-wrap gap-3 border-t border-border/40 pt-5">
		<Button onclick={applyAndRestart} disabled={!connected || busy || !config.model}>
			{#if busy}<LoaderCircle class="h-4 w-4 animate-spin" />{/if}
			{status?.running ? 'Apply and restart server' : 'Start server'}
		</Button>
		<Button variant="destructive" onclick={stopServer} disabled={!connected || busy || !status?.running}>Stop server</Button>
		<Button variant="outline" onclick={() => window.open(MANAGER_URL, '_blank')}>
			<ExternalLink class="h-4 w-4" /> Open full control center
		</Button>
	</div>
	<p class="text-xs text-muted-foreground">
		Applying server-wide settings reloads the model. Conversations remain stored in this browser.
	</p>
</div>
