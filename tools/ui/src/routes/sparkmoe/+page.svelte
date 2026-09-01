<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
<!-- Required Notice: Copyright (c) 2026 SparkMoE contributors. -->
<script lang="ts">
	import {
		Activity,
		AlertCircle,
		BrainCircuit,
		ChartNoAxesCombined,
		CheckCircle2,
		CircleStop,
		Database,
		Download,
		FolderOpen,
		LoaderCircle,
		Play,
		RefreshCw,
		RotateCcw,
		Server,
		Sparkles,
		TerminalSquare
	} from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Card from '$lib/components/ui/card';
	import { ServerModelStatus } from '$lib/enums';
	import {
		type SparkMoeLogSession,
		type SparkMoeManagerStatus,
		type SparkMoeModelProfile,
		type SparkMoeRouterConfig,
		SparkMoeService
	} from '$lib/services/sparkmoe.service';
	import { modelsStore } from '$lib/stores';
	import { onMount } from 'svelte';

	type SparkMoeTab = 'overview' | 'models' | 'engine' | 'context' | 'mtp' | 'metrics' | 'logs';

	const CONTEXT_PRESETS = [4096, 8192, 16384, 32768, 65536];
	const CACHE_TYPES = ['f16', 'bf16', 'q8_0', 'q5_1', 'q5_0', 'q4_1', 'q4_0', 'iq4_nl'];
	const disableThinkingKwargs = '{"enable_thinking": false}';

	const defaultRouterConfig: SparkMoeRouterConfig = {
		model_directory: '',
		models_autoload: true,
		models_max: 1,
		port: 8080
	};

	const defaultProfile: SparkMoeModelProfile = {
		batch_size: 1024,
		cache: 2048,
		cache_policy: 'lru',
		cache_ram_mib: 0,
		cache_type_k: 'f16',
		cache_type_v: 'f16',
		context_shift: true,
		cpu_repack: 'auto',
		ctx_checkpoints: 4,
		ctx_size: 8192,
		dense_residency: 'auto',
		inference_threads: 0,
		io_mode: 'buffered',
		io_threads: 4,
		mode: 'auto',
		mtp_backend_sampling: true,
		mtp_cache_mib: 512,
		mtp_draft_tokens: 3,
		mtp_enabled: false,
		mtp_min_tokens: 0,
		mtp_model: '',
		mtp_p_min: 0,
		mtp_threads: 0,
		overlap: 'off',
		paged_layers: 0,
		parallel: 1,
		ple_cache_mib: 64,
		prefetch: 'off',
		reasoning: 'auto',
		slots: 0,
		ubatch_size: 128
	};

	let activeTab = $state<SparkMoeTab>('overview');
	let managerStatus = $state<SparkMoeManagerStatus | null>(null);
	let routerConfig = $state<SparkMoeRouterConfig>({ ...defaultRouterConfig });
	let profile = $state<SparkMoeModelProfile>({ ...defaultProfile });
	let profileIsOverride = $state(false);
	let selectedModel = $state('');
	let loadedProfileModel = $state('');
	let connected = $state(false);
	let busy = $state('');
	let error = $state('');
	let notice = $state('');
	let metrics = $state<Record<string, unknown> | null>(null);
	let runtimeConfig = $state<Record<string, unknown> | null>(null);
	let hardware = $state<Record<string, number | string> | null>(null);
	let logSessions = $state<SparkMoeLogSession[]>([]);
	let selectedLogSession = $state('');
	let logContent = $state('');
	let logDirectory = $state('');

	let selectedModelEntry = $derived(
		modelsStore.routerModels.find((model) => model.id === selectedModel)
	);
	let selectedModelStatus = $derived(
		selectedModelEntry?.status.value ?? ServerModelStatus.UNLOADED
	);
	let selectedModelLoaded = $derived(
		selectedModelStatus === ServerModelStatus.LOADED ||
			selectedModelStatus === ServerModelStatus.SLEEPING
	);
	let selectedModelBusy = $derived(
		selectedModelStatus === ServerModelStatus.LOADING ||
			modelsStore.status.isOperationInProgress(selectedModel)
	);
	let apiExample = $derived(
		JSON.stringify(
			{
				chat_template_kwargs: { enable_thinking: false },
				messages: [{ content: 'Hello', role: 'user' }],
				model: selectedModel || 'your-model-id',
				reasoning_effort: 'none'
			},
			null,
			2
		)
	);

	function describeManagerStatus() {
		if (!managerStatus) return 'Manager unavailable';

		if (managerStatus.server_ready) return 'llama.cpp router ready';

		if (managerStatus.running) return 'llama.cpp router starting';

		if (managerStatus.phase === 'exited') return 'llama.cpp router exited';

		return 'llama.cpp router stopped';
	}

	function formatBytes(value: number) {
		if (!Number.isFinite(value) || value <= 0) return '0 B';

		const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
		const index = Math.min(Math.floor(Math.log(value) / Math.log(1024)), units.length - 1);

		return (value / 1024 ** index).toFixed(index === 0 ? 0 : 1) + ' ' + units[index];
	}

	function metricNumber(key: string) {
		const value = metrics?.[key];

		return typeof value === 'number' ? value : 0;
	}

	async function refreshManager() {
		try {
			const [status, config, recommendation] = await Promise.all([
				SparkMoeService.status(),
				SparkMoeService.routerConfig(),
				SparkMoeService.recommendation()
			]);

			managerStatus = status;
			routerConfig = { ...defaultRouterConfig, ...config };
			hardware = recommendation;
			connected = true;
		} catch (cause) {
			connected = false;
			managerStatus = null;

			throw cause;
		}
	}

	async function refreshModels() {
		await modelsStore.fetch(true);
		await modelsStore.fetchRouterModels();

		if (!selectedModel || !modelsStore.routerModels.some((model) => model.id === selectedModel)) {
			selectedModel =
				modelsStore.selectedModelName &&
				modelsStore.routerModels.some((model) => model.id === modelsStore.selectedModelName)
					? modelsStore.selectedModelName
					: modelsStore.routerModels[0]?.id || '';
		}

		if (selectedModel && loadedProfileModel !== selectedModel) {
			await loadProfile();
		}
	}

	async function refreshRuntime() {
		if (!selectedModel || !selectedModelLoaded) {
			metrics = null;
			runtimeConfig = null;

			return;
		}

		try {
			[metrics, runtimeConfig] = await Promise.all([
				SparkMoeService.paging(selectedModel),
				SparkMoeService.runtimeConfig(selectedModel)
			]);
		} catch {
			metrics = null;
			runtimeConfig = null;
		}
	}

	async function loadProfile() {
		if (!selectedModel) {
			profile = { ...defaultProfile };
			profileIsOverride = false;
			loadedProfileModel = '';

			return;
		}

		const response = await SparkMoeService.modelProfile(selectedModel);

		profile = { ...defaultProfile, ...response.profile };
		profileIsOverride = response.is_override;
		loadedProfileModel = selectedModel;
		await refreshRuntime();
	}

	async function handleModelSelection() {
		error = '';
		notice = '';
		loadedProfileModel = '';

		try {
			await loadProfile();
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
		}
	}

	async function initialize() {
		try {
			await refreshManager();
			await refreshModels();
			await refreshLogs();
			modelsStore.status.subscribe();
			error = '';
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
		}
	}

	async function runAction(name: string, action: () => Promise<void>) {
		busy = name;
		error = '';
		notice = '';

		try {
			await action();
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
		} finally {
			busy = '';
		}
	}

	async function chooseModelsDirectory() {
		await runAction('browse', async () => {
			const result = await SparkMoeService.chooseModelsDirectory();

			if (!result.cancelled && result.directory) {
				routerConfig.model_directory = result.directory;
				notice = 'Folder selected. Apply server settings to rescan its models.';
			}
		});
	}

	async function waitForManagerReady(port: number) {
		for (let attempt = 0; attempt < 120; attempt += 1) {
			const status = await SparkMoeService.status();

			managerStatus = status;

			if (status.server_ready) {
				if (window.location.port !== String(port)) {
					window.location.href = 'http://127.0.0.1:' + port + '/#/sparkmoe';

					return false;
				}

				return true;
			}

			if (!status.running && status.phase === 'exited') {
				throw new Error(status.last_error || 'The llama.cpp router exited during startup.');
			}

			await new Promise((resolve) => setTimeout(resolve, 500));
		}

		throw new Error('The llama.cpp router did not become ready within one minute.');
	}

	async function saveRouterConfig() {
		await runAction('router-config', async () => {
			const result = await SparkMoeService.saveRouterConfig(routerConfig);
			const staysHere = await waitForManagerReady(result.port);

			if (staysHere) {
				await refreshModels();
				notice = 'Server settings saved. The model library was rescanned.';
			}
		});
	}

	async function startRouter() {
		await runAction('router-start', async () => {
			await SparkMoeService.start();
			await waitForManagerReady(routerConfig.port);
			await refreshModels();
		});
	}

	async function restartRouter() {
		await runAction('router-restart', async () => {
			await SparkMoeService.restart();
			await waitForManagerReady(routerConfig.port);
			await refreshModels();
			notice = 'The llama.cpp router restarted.';
		});
	}

	async function stopRouter() {
		await runAction('router-stop', async () => {
			await SparkMoeService.stop();
			await refreshManager();
			metrics = null;
			runtimeConfig = null;
			notice = 'The router and all loaded model processes were stopped.';
		});
	}

	async function toggleSelectedModel() {
		if (!selectedModel) return;

		await runAction('model', async () => {
			if (selectedModelLoaded) {
				await modelsStore.status.unload(selectedModel);
				metrics = null;
				runtimeConfig = null;
			} else {
				await modelsStore.status.load(selectedModel);
				await refreshRuntime();
			}
		});
	}

	async function saveProfile() {
		if (!selectedModel) return;

		await runAction('profile', async () => {
			const wasLoaded = selectedModelLoaded;

			await SparkMoeService.saveModelProfile(selectedModel, profile);
			profileIsOverride = true;
			await modelsStore.fetchRouterModels();

			if (wasLoaded) {
				await modelsStore.status.load(selectedModel);
				await refreshRuntime();
				notice = 'Profile saved and the model reloaded with the new settings.';
			} else {
				notice = 'Profile saved. It will be used the next time this model loads.';
			}
		});
	}

	async function resetProfile() {
		if (!selectedModel) return;

		await runAction('profile-reset', async () => {
			const wasLoaded = selectedModelLoaded;

			await SparkMoeService.resetModelProfile(selectedModel);
			await modelsStore.fetchRouterModels();
			loadedProfileModel = '';
			await loadProfile();

			if (wasLoaded) {
				await modelsStore.status.load(selectedModel);
				await refreshRuntime();
			}

			notice = 'The model now uses the SparkMoE default profile.';
		});
	}

	async function resizeCache() {
		if (!selectedModel) return;

		await runAction('cache-resize', async () => {
			await SparkMoeService.resizeCache(selectedModel, profile.cache);
			await refreshRuntime();
			notice = 'The hybrid cache resize was accepted and will apply before the next decode.';
		});
	}

	async function resetMetrics() {
		if (!selectedModel) return;

		await runAction('metrics-reset', async () => {
			await SparkMoeService.resetMetrics(selectedModel);
			await refreshRuntime();
		});
	}

	async function refreshLogs() {
		const response = await SparkMoeService.logSessions();

		logSessions = response.sessions;
		logDirectory = response.directory;

		if (
			!selectedLogSession ||
			!logSessions.some((session) => session.name === selectedLogSession)
		) {
			selectedLogSession =
				logSessions.find((session) => session.current)?.name || logSessions[0]?.name || '';
		}

		if (!selectedLogSession) {
			logContent = '';

			return;
		}

		const chunk = await SparkMoeService.logChunk(selectedLogSession);

		logContent = chunk.content;
	}

	onMount(() => {
		void initialize();
		const timer = window.setInterval(() => {
			void refreshManager().catch(() => {
				connected = false;
			});

			if (activeTab === 'overview' || activeTab === 'metrics') void refreshRuntime();

			if (activeTab === 'logs') void refreshLogs().catch(() => {});
		}, 2000);

		return () => {
			window.clearInterval(timer);
			modelsStore.status.unsubscribe();
		};
	});
</script>

<svelte:head>
	<title>SparkMoE</title>
</svelte:head>

<div class="mx-auto w-full max-w-6xl space-y-6 p-4 pb-24 md:p-8">
	<div
		class="flex flex-col gap-4 border-b border-border/40 pb-6 sm:flex-row sm:items-center sm:justify-between"
	>
		<div class="flex items-start gap-3">
			<div class="rounded-xl bg-primary/10 p-2.5 text-primary">
				<Sparkles class="h-5 w-5" />
			</div>
			<div>
				<h1 class="text-xl font-semibold tracking-tight">SparkMoE</h1>
				<p class="mt-1 text-sm text-muted-foreground">
					Model loading, expert paging, context, MTP, diagnostics, and local server control.
				</p>
			</div>
		</div>
		<Button variant="outline" size="sm" onclick={() => void initialize()} disabled={busy !== ''}>
			<RefreshCw class={busy === 'refresh' ? 'animate-spin' : ''} />
			Refresh
		</Button>
	</div>

	<Card.Root class="gap-4">
		<Card.Content class="grid gap-4 px-6 sm:grid-cols-[1fr_auto] sm:items-center">
			<div class="flex min-w-0 items-center gap-3">
				{#if managerStatus?.server_ready}
					<CheckCircle2 class="h-5 w-5 shrink-0 text-emerald-500" />
				{:else if managerStatus?.running}
					<LoaderCircle class="h-5 w-5 shrink-0 animate-spin text-amber-500" />
				{:else}
					<AlertCircle class="h-5 w-5 shrink-0 text-muted-foreground" />
				{/if}
				<div class="min-w-0">
					<p class="font-medium">{describeManagerStatus()}</p>
					<p class="truncate text-xs text-muted-foreground">
						{connected
							? 'API and Web UI on port ' +
								routerConfig.port +
								' | PID ' +
								(managerStatus?.pid || '-')
							: 'Start the app with SparkMoE.exe to enable local management.'}
					</p>
				</div>
			</div>
			<div class="flex flex-wrap gap-2">
				{#if managerStatus?.running}
					<Button
						variant="outline"
						size="sm"
						onclick={() => void restartRouter()}
						disabled={busy !== ''}
					>
						<RefreshCw class={busy === 'router-restart' ? 'animate-spin' : ''} />
						Restart
					</Button>
					<Button
						variant="destructive"
						size="sm"
						onclick={() => void stopRouter()}
						disabled={busy !== ''}
					>
						<CircleStop />
						Stop
					</Button>
				{:else}
					<Button size="sm" onclick={() => void startRouter()} disabled={!connected || busy !== ''}>
						<Play />
						Start
					</Button>
				{/if}
			</div>
		</Card.Content>
	</Card.Root>

	<div class="flex w-full gap-1 overflow-x-auto rounded-lg bg-muted/50 p-1">
		<Button
			class="shrink-0"
			variant={activeTab === 'overview' ? 'secondary' : 'ghost'}
			onclick={() => (activeTab = 'overview')}
		>
			<Activity />
			Overview
		</Button>
		<Button
			class="shrink-0"
			variant={activeTab === 'models' ? 'secondary' : 'ghost'}
			onclick={() => (activeTab = 'models')}
		>
			<Server />
			Models
		</Button>
		<Button
			class="shrink-0"
			variant={activeTab === 'engine' ? 'secondary' : 'ghost'}
			onclick={() => (activeTab = 'engine')}
		>
			<Sparkles />
			SparkMoE engine
		</Button>
		<Button
			class="shrink-0"
			variant={activeTab === 'context' ? 'secondary' : 'ghost'}
			onclick={() => (activeTab = 'context')}
		>
			<Database />
			Context & caches
		</Button>
		<Button
			class="shrink-0"
			variant={activeTab === 'mtp' ? 'secondary' : 'ghost'}
			onclick={() => (activeTab = 'mtp')}
		>
			<BrainCircuit />
			MTP & reasoning
		</Button>
		<Button
			class="shrink-0"
			variant={activeTab === 'metrics' ? 'secondary' : 'ghost'}
			onclick={() => (activeTab = 'metrics')}
		>
			<ChartNoAxesCombined />
			Metrics
		</Button>
		<Button
			class="shrink-0"
			variant={activeTab === 'logs' ? 'secondary' : 'ghost'}
			onclick={() => (activeTab = 'logs')}
		>
			<TerminalSquare />
			Logs & diagnostics
		</Button>
	</div>

	{#if error}
		<div
			class="rounded-lg border border-destructive/40 bg-destructive/10 p-3 text-sm text-destructive"
		>
			{error}
		</div>
	{/if}
	{#if notice}
		<div
			class="rounded-lg border border-emerald-500/30 bg-emerald-500/10 p-3 text-sm text-foreground"
		>
			{notice}
		</div>
	{/if}

	{#if ['overview', 'models', 'engine', 'metrics'].includes(activeTab)}
		<div
			class={activeTab === 'overview' || activeTab === 'metrics'
				? 'grid gap-6 lg:grid-cols-[minmax(0,1.2fr)_minmax(18rem,0.8fr)]'
				: 'space-y-6'}
		>
			<div class="space-y-6">
				{#if activeTab === 'overview'}
					<Card.Root>
						<Card.Header>
							<Card.Title>Overview and status</Card.Title>
							<Card.Description>
								A compact view of the Router, model library, and selected model process.
							</Card.Description>
						</Card.Header>
						<Card.Content class="grid gap-3 sm:grid-cols-2">
							<div class="rounded-lg bg-muted/50 p-3">
								<span class="text-xs text-muted-foreground">Selected model</span>
								<b class="mt-1 block truncate">{selectedModel || 'No model selected'}</b>
							</div>
							<div class="rounded-lg bg-muted/50 p-3">
								<span class="text-xs text-muted-foreground">Model status</span>
								<b class="mt-1 block">{selectedModel ? selectedModelStatus : '-'}</b>
							</div>
							<div class="rounded-lg bg-muted/50 p-3">
								<span class="text-xs text-muted-foreground">Available models</span>
								<b class="mt-1 block">{modelsStore.routerModels.length.toLocaleString()}</b>
							</div>
							<div class="rounded-lg bg-muted/50 p-3">
								<span class="text-xs text-muted-foreground">Maximum loaded models</span>
								<b class="mt-1 block">{routerConfig.models_max || 'Unlimited'}</b>
							</div>
						</Card.Content>
						<Card.Footer class="flex-wrap justify-end gap-2">
							<Button variant="outline" onclick={() => (activeTab = 'models')}>Manage models</Button
							>
							<Button onclick={() => (activeTab = 'metrics')}>View metrics</Button>
						</Card.Footer>
					</Card.Root>
				{/if}

				{#if activeTab === 'models'}
					<Card.Root>
						<Card.Header>
							<Card.Title>llama.cpp router</Card.Title>
							<Card.Description>
								The Web UI and API keep one stable address while model processes load on demand.
							</Card.Description>
						</Card.Header>
						<Card.Content class="grid gap-4">
							<label class="grid gap-2 text-sm">
								<span class="font-medium">Models folder</span>
								<div class="flex min-w-0 gap-2">
									<input
										class="min-w-0 flex-1 rounded-md border border-input bg-background px-3 py-2"
										dir="ltr"
										placeholder="Choose the folder that contains GGUF models"
										readonly
										bind:value={routerConfig.model_directory}
									/>
									<Button
										variant="outline"
										onclick={() => void chooseModelsDirectory()}
										disabled={busy !== ''}
									>
										{#if busy === 'browse'}<LoaderCircle class="animate-spin" />{:else}<FolderOpen
											/>{/if}
										Choose
									</Button>
								</div>
							</label>
							<div class="grid gap-4 sm:grid-cols-2">
								<label class="grid gap-2 text-sm">
									<span class="font-medium">Maximum loaded models</span>
									<input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="0"
										max="64"
										bind:value={routerConfig.models_max}
									/>
									<span class="text-xs text-muted-foreground">
										0 means unlimited. The router unloads the least recently used idle model at the
										limit.
									</span>
								</label>
								<label class="grid gap-2 text-sm">
									<span class="font-medium">Web UI and API port</span>
									<input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="1024"
										max="65535"
										bind:value={routerConfig.port}
									/>
									<span class="text-xs text-muted-foreground"
										>Changing this moves the browser and API address.</span
									>
								</label>
							</div>
							<label
								class="flex items-start justify-between gap-4 rounded-lg border border-border/60 p-3 text-sm"
							>
								<span>
									<b>Load models automatically for API requests</b>
									<span class="mt-1 block text-xs text-muted-foreground">
										When enabled, a request containing a model ID loads that model if it is not
										already active.
									</span>
								</span>
								<input
									class="mt-1 h-4 w-4"
									type="checkbox"
									bind:checked={routerConfig.models_autoload}
								/>
							</label>
						</Card.Content>
						<Card.Footer class="justify-end">
							<Button onclick={() => void saveRouterConfig()} disabled={!connected || busy !== ''}>
								{#if busy === 'router-config'}<LoaderCircle class="animate-spin" />{/if}
								Apply server settings
							</Button>
						</Card.Footer>
					</Card.Root>
				{/if}

				{#if activeTab === 'models' || activeTab === 'engine' || activeTab === 'metrics'}
					<Card.Root>
						<Card.Header>
							<Card.Title
								>{activeTab === 'engine'
									? 'SparkMoE engine profile'
									: activeTab === 'metrics'
										? 'Metrics target'
										: 'Models'}</Card.Title
							>
							<Card.Description>
								{activeTab === 'engine'
									? 'Each model keeps its own lossless expert-loading and memory policy.'
									: 'Select a model and control whether its process is loaded.'}
							</Card.Description>
						</Card.Header>
						<Card.Content class="space-y-5">
							<label class="grid gap-2 text-sm">
								<span class="font-medium">Model</span>
								<select
									class="min-w-0 rounded-md border border-input bg-background px-3 py-2"
									bind:value={selectedModel}
									onchange={() => void handleModelSelection()}
								>
									{#if modelsStore.routerModels.length === 0}
										<option value="">No models found</option>
									{/if}
									{#each modelsStore.routerModels as model (model.id)}
										<option value={model.id}>{model.id} ({model.status.value})</option>
									{/each}
								</select>
							</label>

							{#if selectedModel}
								<div
									class="flex flex-wrap items-center justify-between gap-3 rounded-lg bg-muted/40 p-3"
								>
									<div>
										<p class="text-sm font-medium">
											{profileIsOverride ? 'Custom SparkMoE profile' : 'SparkMoE default profile'}
										</p>
										<p class="text-xs text-muted-foreground">Status: {selectedModelStatus}</p>
									</div>
									<div class="flex flex-wrap gap-2">
										<Button
											variant={selectedModelLoaded ? 'destructive' : 'outline'}
											size="sm"
											onclick={() => void toggleSelectedModel()}
											disabled={busy !== '' || selectedModelBusy}
										>
											{#if selectedModelBusy}<LoaderCircle
													class="animate-spin"
												/>{:else if selectedModelLoaded}<CircleStop />{:else}<Play />{/if}
											{selectedModelLoaded ? 'Unload' : 'Load'}
										</Button>
										{#if profileIsOverride}
											<Button
												variant="ghost"
												size="sm"
												onclick={() => void resetProfile()}
												disabled={busy !== ''}
											>
												<RotateCcw />
												Use defaults
											</Button>
										{/if}
									</div>
								</div>
							{/if}

							{#if activeTab === 'engine'}
								<div class="grid gap-4 sm:grid-cols-2">
									<label class="grid gap-2 text-sm">
										<span class="font-medium">Loading method</span>
										<select
											class="rounded-md border border-input bg-background px-3 py-2"
											bind:value={profile.mode}
										>
											<option value="auto">Automatic - recommended</option>
											<option value="os">Standard mmap</option>
											<option value="explicit">SparkMoE expert paging</option>
											<option value="off">Fully resident / no mmap</option>
										</select>
									</label>
									<label class="grid gap-2 text-sm">
										<span class="font-medium">Expert RAM cache (MiB)</span>
										<input
											class="rounded-md border border-input bg-background px-3 py-2"
											type="number"
											min="0"
											bind:value={profile.cache}
										/>
									</label>
									<label class="grid gap-2 text-sm">
										<span class="font-medium">SSD read workers</span>
										<input
											class="rounded-md border border-input bg-background px-3 py-2"
											type="number"
											min="1"
											max="128"
											bind:value={profile.io_threads}
										/>
									</label>
									<label class="grid gap-2 text-sm">
										<span class="font-medium">Expert slots per layer</span>
										<input
											class="rounded-md border border-input bg-background px-3 py-2"
											type="number"
											min="0"
											bind:value={profile.slots}
										/>
									</label>
								</div>

								<details class="rounded-lg border border-border/60 p-4">
									<summary class="cursor-pointer text-sm font-medium"
										>Advanced loading policies</summary
									>
									<div class="mt-4 grid gap-4 sm:grid-cols-2">
										<label class="grid gap-2 text-sm"
											><span>Expert storage I/O</span><select
												class="rounded-md border border-input bg-background px-3 py-2"
												bind:value={profile.io_mode}
												><option value="buffered">Buffered</option><option value="direct"
													>Direct with fallback</option
												></select
											></label
										>
										<label class="grid gap-2 text-sm"
											><span>Cache policy</span><select
												class="rounded-md border border-input bg-background px-3 py-2"
												bind:value={profile.cache_policy}
												><option value="lru">Per-layer LRU</option><option value="hybrid"
													>Hybrid global / per-layer</option
												></select
											></label
										>
										<label class="grid gap-2 text-sm"
											><span>Compute / I/O overlap</span><select
												class="rounded-md border border-input bg-background px-3 py-2"
												bind:value={profile.overlap}
												><option value="off">Off</option><option value="on">On</option></select
											></label
										>
										<label class="grid gap-2 text-sm"
											><span>Expert prefetch</span><select
												class="rounded-md border border-input bg-background px-3 py-2"
												bind:value={profile.prefetch}
												><option value="off">Off</option><option value="temporal">Temporal</option
												></select
											></label
										>
										<label class="grid gap-2 text-sm"
											><span>Dense / shared weights</span><select
												class="rounded-md border border-input bg-background px-3 py-2"
												bind:value={profile.dense_residency}
												><option value="auto">Automatic</option><option value="mmap">mmap</option
												><option value="resident">Resident buffers</option><option value="locked"
													>Lock in RAM</option
												></select
											></label
										>
										<label class="grid gap-2 text-sm"
											><span>First layers to page (0 = all)</span><input
												class="rounded-md border border-input bg-background px-3 py-2"
												type="number"
												min="0"
												max="4096"
												bind:value={profile.paged_layers}
											/></label
										>
										<label class="grid gap-2 text-sm"
											><span>PLE page cache (MiB)</span><input
												class="rounded-md border border-input bg-background px-3 py-2"
												type="number"
												min="1"
												max="4096"
												bind:value={profile.ple_cache_mib}
											/></label
										>
										<label class="grid gap-2 text-sm"
											><span>Inference CPU threads</span><input
												class="rounded-md border border-input bg-background px-3 py-2"
												type="number"
												min="0"
												max="128"
												bind:value={profile.inference_threads}
											/></label
										>
										<label class="grid gap-2 text-sm"
											><span>CPU weight repacking</span><select
												class="rounded-md border border-input bg-background px-3 py-2"
												bind:value={profile.cpu_repack}
												><option value="auto">Automatic</option><option value="on">Enabled</option
												><option value="off">Disabled</option></select
											></label
										>
									</div>
								</details>
							{/if}
						</Card.Content>
						{#if activeTab === 'engine'}
							<Card.Footer class="flex-wrap justify-between gap-2">
								<Button
									variant="outline"
									onclick={() => void resizeCache()}
									disabled={!selectedModelLoaded ||
										profile.cache_policy !== 'hybrid' ||
										busy !== ''}
								>
									Apply hybrid cache now
								</Button>
								<Button onclick={() => void saveProfile()} disabled={!selectedModel || busy !== ''}>
									{#if busy === 'profile'}<LoaderCircle class="animate-spin" />{/if}
									Save model profile
								</Button>
							</Card.Footer>
						{/if}
					</Card.Root>
				{/if}
			</div>

			{#if activeTab === 'overview' || activeTab === 'metrics'}
				<div class="space-y-6">
					{#if activeTab === 'metrics'}
						<Card.Root>
							<Card.Header>
								<Card.Title class="flex items-center gap-2"
									><Activity class="h-4 w-4" /> Live expert paging</Card.Title
								>
								<Card.Description>Runtime counters for the selected loaded model.</Card.Description>
							</Card.Header>
							<Card.Content>
								{#if selectedModelLoaded && metrics}
									<div class="grid grid-cols-2 gap-3">
										<div class="rounded-lg bg-muted/50 p-3">
											<span class="text-xs text-muted-foreground">Cache hits</span><b
												class="mt-1 block">{metricNumber('cache_hits').toLocaleString()}</b
											>
										</div>
										<div class="rounded-lg bg-muted/50 p-3">
											<span class="text-xs text-muted-foreground">Cache misses</span><b
												class="mt-1 block">{metricNumber('cache_misses').toLocaleString()}</b
											>
										</div>
										<div class="rounded-lg bg-muted/50 p-3">
											<span class="text-xs text-muted-foreground">Hit rate</span><b
												class="mt-1 block">{(metricNumber('cache_hit_rate') * 100).toFixed(1)}%</b
											>
										</div>
										<div class="rounded-lg bg-muted/50 p-3">
											<span class="text-xs text-muted-foreground">SSD read</span><b
												class="mt-1 block">{formatBytes(metricNumber('bytes_read'))}</b
											>
										</div>
										<div class="rounded-lg bg-muted/50 p-3">
											<span class="text-xs text-muted-foreground">Cache committed</span><b
												class="mt-1 block">{formatBytes(metricNumber('cache_committed_bytes'))}</b
											>
										</div>
										<div class="rounded-lg bg-muted/50 p-3">
											<span class="text-xs text-muted-foreground">Process RAM</span><b
												class="mt-1 block">{formatBytes(metricNumber('process_rss_bytes'))}</b
											>
										</div>
										<div class="rounded-lg bg-muted/50 p-3">
											<span class="text-xs text-muted-foreground">Evictions</span><b
												class="mt-1 block">{metricNumber('evictions').toLocaleString()}</b
											>
										</div>
										<div class="rounded-lg bg-muted/50 p-3">
											<span class="text-xs text-muted-foreground">Avg. read</span><b
												class="mt-1 block"
												>{(metricNumber('average_read_latency_us') / 1000).toFixed(2)} ms</b
											>
										</div>
									</div>
								{:else}
									<p class="text-sm text-muted-foreground">
										Load the selected model to see its live memory and expert paging metrics.
									</p>
								{/if}
							</Card.Content>
							<Card.Footer class="justify-end">
								<Button
									variant="ghost"
									size="sm"
									onclick={() => void resetMetrics()}
									disabled={!selectedModelLoaded || busy !== ''}
								>
									<RotateCcw />
									Reset metrics
								</Button>
							</Card.Footer>
						</Card.Root>
					{/if}

					<Card.Root>
						<Card.Header>
							<Card.Title>Effective runtime</Card.Title>
							<Card.Description>Values reported by the loaded model process.</Card.Description>
						</Card.Header>
						<Card.Content class="space-y-2 text-sm">
							<div class="flex justify-between gap-4">
								<span class="text-muted-foreground">Paging mode</span><b
									>{String(runtimeConfig?.mode ?? '-')}</b
								>
							</div>
							<div class="flex justify-between gap-4">
								<span class="text-muted-foreground">Architecture</span><b
									>{String(runtimeConfig?.model_architecture ?? '-')}</b
								>
							</div>
							<div class="flex justify-between gap-4">
								<span class="text-muted-foreground">Context per slot</span><b
									>{Number(runtimeConfig?.context_size_per_slot ?? 0).toLocaleString()}</b
								>
							</div>
							<div class="flex justify-between gap-4">
								<span class="text-muted-foreground">Active experts</span><b
									>{String(runtimeConfig?.active_expert_count ?? '-')}</b
								>
							</div>
							<div class="flex justify-between gap-4">
								<span class="text-muted-foreground">MTP</span><b
									>{runtimeConfig?.mtp_enabled ? 'Enabled' : 'Disabled'}</b
								>
							</div>
						</Card.Content>
					</Card.Root>

					<Card.Root>
						<Card.Header>
							<Card.Title>System memory</Card.Title>
							<Card.Description>Live machine-wide RAM, not a model-fit estimate.</Card.Description>
						</Card.Header>
						<Card.Content class="space-y-3">
							{@const totalRam = Number(hardware?.total_ram_mib ?? 0) * 1024 * 1024}
							{@const usedRam = Number(hardware?.used_ram_mib ?? 0) * 1024 * 1024}
							{@const usedPercent = totalRam > 0 ? Math.min(100, (usedRam / totalRam) * 100) : 0}
							<div class="h-2 overflow-hidden rounded-full bg-muted">
								<div
									class="h-full rounded-full bg-primary transition-[width]"
									style:width={usedPercent + '%'}
								></div>
							</div>
							<div class="flex justify-between gap-3 text-xs text-muted-foreground">
								<span>Used {formatBytes(usedRam)}</span>
								<span
									>Available {formatBytes(
										Number(hardware?.available_ram_mib ?? 0) * 1024 * 1024
									)}</span
								>
							</div>
						</Card.Content>
					</Card.Root>
				</div>
			{/if}
		</div>
	{:else if activeTab === 'context' || activeTab === 'mtp'}
		<div class="space-y-6">
			<Card.Root>
				<Card.Content class="grid gap-3 px-6 sm:grid-cols-[1fr_auto] sm:items-end">
					<label class="grid gap-2 text-sm">
						<span class="font-medium">Model profile</span>
						<select
							class="rounded-md border border-input bg-background px-3 py-2"
							bind:value={selectedModel}
							onchange={() => void handleModelSelection()}
						>
							{#if modelsStore.routerModels.length === 0}<option value="">No models found</option
								>{/if}
							{#each modelsStore.routerModels as model (model.id)}
								<option value={model.id}>{model.id}</option>
							{/each}
						</select>
					</label>
					<span class="pb-2 text-xs text-muted-foreground"
						>{profileIsOverride ? 'Custom profile' : 'SparkMoE defaults'}</span
					>
				</Card.Content>
			</Card.Root>

			{#if activeTab === 'context'}
				<Card.Root>
					<Card.Header>
						<Card.Title>Context and request defaults</Card.Title>
						<Card.Description>
							These values belong to the selected model process and take effect when that model
							loads.
						</Card.Description>
					</Card.Header>
					<Card.Content class="space-y-5">
						<div class="grid gap-4 sm:grid-cols-2">
							<label class="grid gap-2 text-sm">
								<span class="font-medium">Context tokens</span>
								<input
									class="rounded-md border border-input bg-background px-3 py-2"
									type="number"
									min="0"
									max="1048576"
									step="1024"
									bind:value={profile.ctx_size}
								/>
								<span class="text-xs text-muted-foreground">0 uses the model default.</span>
							</label>
							<label class="grid gap-2 text-sm">
								<span class="font-medium">When context is full</span>
								<select
									class="rounded-md border border-input bg-background px-3 py-2"
									value={profile.context_shift ? 'shift' : 'stop'}
									onchange={(event) =>
										(profile.context_shift = event.currentTarget.value === 'shift')}
								>
									<option value="shift">Forget oldest messages and continue</option>
									<option value="stop">Stop at the context limit</option>
								</select>
							</label>
							<label class="grid gap-2 text-sm">
								<span class="font-medium">Parallel chat slots</span>
								<input
									class="rounded-md border border-input bg-background px-3 py-2"
									type="number"
									min="1"
									max="32"
									bind:value={profile.parallel}
								/>
								<span class="text-xs text-muted-foreground"
									>Expert paging currently requires one slot.</span
								>
							</label>
						</div>
						<div class="flex flex-wrap gap-2">
							{#each CONTEXT_PRESETS as tokens (tokens)}
								<Button
									variant={profile.ctx_size === tokens ? 'secondary' : 'outline'}
									size="sm"
									onclick={() => (profile.ctx_size = tokens)}
								>
									{tokens / 1024}K
								</Button>
							{/each}
						</div>
						<details class="rounded-lg border border-border/60 p-4">
							<summary class="cursor-pointer text-sm font-medium">Advanced context memory</summary>
							<div class="mt-4 grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
								<label class="grid gap-2 text-sm"
									><span>Logical batch size</span><input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="1"
										max="16384"
										bind:value={profile.batch_size}
									/></label
								>
								<label class="grid gap-2 text-sm"
									><span>Physical microbatch</span><input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="1"
										max="16384"
										bind:value={profile.ubatch_size}
									/></label
								>
								<label class="grid gap-2 text-sm"
									><span>Prompt cache RAM (MiB)</span><input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="-1"
										max="1048576"
										bind:value={profile.cache_ram_mib}
									/></label
								>
								<label class="grid gap-2 text-sm"
									><span>Context checkpoints</span><input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="0"
										max="128"
										bind:value={profile.ctx_checkpoints}
									/></label
								>
								<label class="grid gap-2 text-sm"
									><span>K cache precision</span><select
										class="rounded-md border border-input bg-background px-3 py-2"
										bind:value={profile.cache_type_k}
										>{#each CACHE_TYPES as type (type)}<option value={type}>{type}</option
											>{/each}</select
									></label
								>
								<label class="grid gap-2 text-sm"
									><span>V cache precision</span><select
										class="rounded-md border border-input bg-background px-3 py-2"
										bind:value={profile.cache_type_v}
										>{#each CACHE_TYPES as type (type)}<option value={type}>{type}</option
											>{/each}</select
									></label
								>
							</div>
						</details>
					</Card.Content>
				</Card.Root>
			{/if}

			{#if activeTab === 'mtp'}
				<Card.Root>
					<Card.Header>
						<Card.Title>Reasoning defaults</Card.Title>
						<Card.Description>
							Choose the default for this model. Chat and API requests can override it
							independently.
						</Card.Description>
					</Card.Header>
					<Card.Content>
						<label class="grid gap-2 text-sm">
							<span class="font-medium">Default reasoning behavior</span>
							<select
								class="rounded-md border border-input bg-background px-3 py-2"
								bind:value={profile.reasoning}
							>
								<option value="auto">Automatic - model template default</option>
								<option value="on">Enabled by default</option>
								<option value="off">Disabled by default</option>
							</select>
							<span class="text-xs text-muted-foreground">
								This is a model-process default, not a restriction on API callers.
							</span>
						</label>
					</Card.Content>
				</Card.Root>

				<Card.Root>
					<Card.Header>
						<Card.Title>Multi-Token Prediction (MTP)</Card.Title>
						<Card.Description>
							Optional speculative generation for models with a compatible embedded head or sidecar
							GGUF.
						</Card.Description>
					</Card.Header>
					<Card.Content class="space-y-5">
						<label
							class="flex items-start justify-between gap-4 rounded-lg border border-border/60 p-4 text-sm"
						>
							<span>
								<b>Enable MTP for this model</b>
								<span class="mt-1 block text-xs text-muted-foreground">
									Disable it if startup reports that no compatible MTP head is available.
								</span>
							</span>
							<input class="mt-1 h-4 w-4" type="checkbox" bind:checked={profile.mtp_enabled} />
						</label>
						{#if profile.mtp_enabled}
							<label class="grid gap-2 text-sm">
								<span class="font-medium">MTP sidecar GGUF path (optional)</span>
								<input
									class="rounded-md border border-input bg-background px-3 py-2"
									dir="ltr"
									bind:value={profile.mtp_model}
								/>
								<span class="text-xs text-muted-foreground"
									>Leave empty to use an MTP head included in the target model.</span
								>
							</label>
							<div class="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
								<label class="grid gap-2 text-sm"
									><span>Draft tokens per step</span><input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="1"
										max="64"
										bind:value={profile.mtp_draft_tokens}
									/></label
								>
								<label class="grid gap-2 text-sm"
									><span>Minimum accepted draft tokens</span><input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="0"
										max="64"
										bind:value={profile.mtp_min_tokens}
									/></label
								>
								<label class="grid gap-2 text-sm"
									><span>Minimum draft probability</span><input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="0"
										max="1"
										step="0.01"
										bind:value={profile.mtp_p_min}
									/></label
								>
								<label class="grid gap-2 text-sm"
									><span>MTP expert cache (MiB)</span><input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="1"
										max="65536"
										bind:value={profile.mtp_cache_mib}
									/></label
								>
								<label class="grid gap-2 text-sm"
									><span>MTP CPU threads</span><input
										class="rounded-md border border-input bg-background px-3 py-2"
										type="number"
										min="0"
										max="128"
										bind:value={profile.mtp_threads}
									/></label
								>
								<label
									class="flex items-center gap-3 self-end rounded-lg border border-border/60 p-3 text-sm"
									><input
										class="h-4 w-4"
										type="checkbox"
										bind:checked={profile.mtp_backend_sampling}
									/> Backend sampling</label
								>
							</div>
						{/if}
					</Card.Content>
				</Card.Root>
			{/if}

			<Card.Root class="border-primary/30 bg-primary/5">
				<Card.Header>
					<Card.Title>External API and thinking control</Card.Title>
					<Card.Description>
						Loading settings come from this model profile. Generation settings come from each API
						request.
					</Card.Description>
				</Card.Header>
				<Card.Content class="space-y-3 text-sm">
					<p>
						SmartiAI can use the normal OpenAI-compatible endpoint on port {routerConfig.port}. Send
						the selected model ID in the request. If it is not loaded, the Router loads it with the
						profile above.
					</p>
					<p>
						To disable thinking for one chat request, send both
						<code class="rounded bg-muted px-1 py-0.5">reasoning_effort: "none"</code> and
						<code class="rounded bg-muted px-1 py-0.5"
							>chat_template_kwargs: {disableThinkingKwargs}</code
						>. This overrides the profile default without restarting or reloading the model.
					</p>
					<pre
						class="overflow-x-auto rounded-lg border border-border/60 bg-muted/40 p-4 text-xs"><code
							>{apiExample}</code
						></pre>
				</Card.Content>
			</Card.Root>

			<div class="flex flex-wrap justify-end gap-2">
				{#if profileIsOverride}
					<Button
						variant="ghost"
						onclick={() => void resetProfile()}
						disabled={!selectedModel || busy !== ''}
					>
						<RotateCcw />
						Use defaults
					</Button>
				{/if}
				<Button onclick={() => void saveProfile()} disabled={!selectedModel || busy !== ''}>
					{#if busy === 'profile'}<LoaderCircle class="animate-spin" />{/if}
					{activeTab === 'context'
						? 'Save context and cache profile'
						: 'Save MTP and reasoning profile'}
				</Button>
			</div>
		</div>
	{:else}
		<div class="space-y-6">
			<Card.Root>
				<Card.Header>
					<Card.Title class="flex items-center gap-2"
						><TerminalSquare class="h-4 w-4" /> Server logs</Card.Title
					>
					<Card.Description>
						Complete combined llama.cpp Router and model-process output. Session files remain on
						this computer.
					</Card.Description>
				</Card.Header>
				<Card.Content class="space-y-4">
					<div class="flex flex-col gap-3 sm:flex-row">
						<select
							class="min-w-0 flex-1 rounded-md border border-input bg-background px-3 py-2 text-sm"
							bind:value={selectedLogSession}
							onchange={() => void refreshLogs()}
						>
							{#if logSessions.length === 0}<option value="">No log sessions</option>{/if}
							{#each logSessions as session (session.name)}
								<option value={session.name}>
									{session.name}{session.current ? ' (current)' : ''} - {formatBytes(session.size)}
								</option>
							{/each}
						</select>
						<Button variant="outline" onclick={() => void refreshLogs()} disabled={!connected}>
							<RefreshCw />
							Refresh
						</Button>
						<Button
							variant="outline"
							href={selectedLogSession
								? SparkMoeService.logDownloadUrl(selectedLogSession)
								: undefined}
							disabled={!selectedLogSession}
						>
							<Download />
							Download full log
						</Button>
					</div>
					<pre
						class="min-h-80 max-h-[60vh] overflow-auto rounded-lg border border-border/60 bg-muted/35 p-4 font-mono text-xs leading-relaxed whitespace-pre-wrap"
						dir="ltr">{logContent || 'No log output yet.'}</pre>
					<p class="break-all text-xs text-muted-foreground">Stored in: {logDirectory || '-'}</p>
				</Card.Content>
			</Card.Root>

			<Card.Root>
				<Card.Header>
					<Card.Title>Diagnostics</Card.Title>
					<Card.Description>
						The diagnostic export excludes prompts, generated text, API keys, usernames, model
						contents, and private paths.
					</Card.Description>
				</Card.Header>
				<Card.Footer class="justify-end">
					<Button variant="outline" href={SparkMoeService.diagnosticsUrl()}>
						<Download />
						Download diagnostics
					</Button>
				</Card.Footer>
			</Card.Root>
		</div>
	{/if}
</div>
