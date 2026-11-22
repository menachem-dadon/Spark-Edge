<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
<!-- Required Notice: Copyright (c) 2026 SparkMoE contributors. -->
<script lang="ts">
	import { t } from '$lib/i18n/locale.svelte';
	import {
		Activity,
		CircleStop,
		Download,
		FolderOpen,
		Library,
		Link,
		LoaderCircle,
		MessageSquare,
		Play,
		Plus,
		Power,
		RefreshCw,
		Search,
		Settings,
		Sparkles,
		Star,
		Trash2
	} from '@lucide/svelte';
	import { goto } from '$app/navigation';
	import { DialogConfirmation } from '$lib/components/app';
	import SparkMoeConnections from '$lib/components/app/settings/SparkMoeConnections.svelte';
	import SparkMoeDownloads from '$lib/components/app/settings/SparkMoeDownloads.svelte';
	import SparkMoeModelSettings from '$lib/components/app/settings/SparkMoeModelSettings.svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Card from '$lib/components/ui/card';
	import { ROUTES } from '$lib/constants';
	import { ServerModelStatus } from '$lib/enums';
	import { RouterService } from '$lib/services/router.service';
	import {
		SparkMoeService,
		type SparkMoeLibraryModel,
		type SparkMoeLogSession,
		type SparkMoeManagerStatus,
		type SparkMoeRouterConfig,
		type EngineStatus
	} from '$lib/services/sparkmoe.service';
	import { modelsStore } from '$lib/stores';
	import { sparkMoeProfileStore } from '$lib/stores/sparkmoe-profile.svelte';
	import { onMount } from 'svelte';

	type Tab = 'models' | 'connections' | 'activity' | 'engines';
	type MetricItem = { label: string; value: string };
	let activeTab = $state<Tab>('models');
	let library = $state<SparkMoeLibraryModel[]>([]);
	let selectedModel = $state('');
	let engineStatus = $state<EngineStatus | null>(null);
	let managerStatus = $state<SparkMoeManagerStatus | null>(null);
	let routerConfig = $state<SparkMoeRouterConfig>({
		model_directory: '',
		models_autoload: true,
		models_max: 1,
		port: 8080
	});
	let savedRouterConfig = $state<SparkMoeRouterConfig>({
		model_directory: '',
		models_autoload: true,
		models_max: 1,
		port: 8080
	});
	let initialized = $state(false);
	let search = $state('');
	let favoritesOnly = $state(false);
	let importPath = $state('');
	let importOpen = $state(false);
	let downloadsOpen = $state(false);
	let rename = $state('');
	let busy = $state('');
	let error = $state('');
	let notice = $state('');
	let removeDialogOpen = $state(false);
	let exitDialogOpen = $state(false);
	let metrics = $state<Record<string, unknown> | null>(null);
	let runtimeConfig = $state<Record<string, unknown> | null>(null);
	let hardware = $state<Record<string, number | string> | null>(null);
	let metricsError = $state('');
	let logSessions = $state<SparkMoeLogSession[]>([]);
	let selectedLogSession = $state('');
	let logContent = $state('');
	let logsError = $state('');
	let logDirectory = $state('');
	let refreshing = false;
	let selected = $derived(library.find((model) => model.id === selectedModel));
	let selectedModelStatus = $derived(
		modelsStore.routerModels.find((model) => model.id === selectedModel)?.status.value ??
			ServerModelStatus.UNLOADED
	);
	let selectedModelLoaded = $derived(
		selectedModelStatus === ServerModelStatus.LOADED ||
			selectedModelStatus === ServerModelStatus.SLEEPING
	);
	let selectedModelBusy = $derived(
		selectedModelStatus === ServerModelStatus.LOADING ||
			modelsStore.status.isOperationInProgress(selectedModel)
	);
	let routerConfigDirty = $derived(
		JSON.stringify(routerConfig) !== JSON.stringify(savedRouterConfig)
	);
	let filteredModels = $derived(
		library
			.filter(
				(model) =>
					(!favoritesOnly || model.favorite) &&
					(model.display_name + ' ' + model.path).toLowerCase().includes(search.toLowerCase())
			)
			.toSorted(
				(a, b) =>
					Number(b.favorite) - Number(a.favorite) || a.display_name.localeCompare(b.display_name)
			)
	);

	let cacheMetrics = $derived<MetricItem[]>([
		{
			label: 'Cumulative generation rate',
			value: metricNumber('generation_tokens_per_second').toFixed(2) + ' tok/s'
		},
		{ label: 'Cache hit rate', value: (metricNumber('cache_hit_rate') * 100).toFixed(1) + '%' },
		{ label: 'Cache hits', value: metricNumber('cache_hits').toLocaleString() },
		{ label: 'Cache misses', value: metricNumber('cache_misses').toLocaleString() },
		{ label: 'Evictions', value: metricNumber('evictions').toLocaleString() },
		{ label: 'Cache waits', value: metricNumber('waits').toLocaleString() },
		{ label: 'Duplicate requests', value: metricNumber('duplicate_requests').toLocaleString() },
		{ label: 'Resident experts', value: metricNumber('resident_experts').toLocaleString() },
		{ label: 'Pinned experts', value: metricNumber('pinned_experts').toLocaleString() }
	]);
	let memoryMetrics = $derived<MetricItem[]>([
		{ label: 'Process RAM', value: formatBytes(metricNumber('process_rss_bytes')) },
		{ label: 'Peak process RAM', value: formatBytes(metricNumber('process_peak_rss_bytes')) },
		{ label: 'Private process memory', value: formatBytes(metricNumber('process_private_bytes')) },
		{ label: 'Cache committed', value: formatBytes(metricNumber('cache_committed_bytes')) },
		{ label: 'Cache limit', value: formatBytes(metricNumber('cache_limit_bytes')) },
		{ label: 'Logical SSD read', value: formatBytes(metricNumber('bytes_read')) },
		{ label: 'Physical SSD read', value: formatBytes(metricNumber('physical_bytes')) },
		{ label: 'Direct SSD read', value: formatBytes(metricNumber('direct_bytes')) },
		{ label: 'Read operations', value: metricNumber('read_count').toLocaleString() },
		{ label: 'Average read', value: formatMilliseconds(metricNumber('average_read_latency_us')) },
		{ label: 'Buffered fallbacks', value: metricNumber('fallback_reads').toLocaleString() }
	]);
	let pipelineMetrics = $derived<MetricItem[]>([
		{ label: 'I/O queue wait', value: formatMilliseconds(metricNumber('io_queue_us')) },
		{ label: 'Remap wait', value: formatMilliseconds(metricNumber('remap_wait_us')) },
		{ label: 'Compute I/O wait', value: formatMilliseconds(metricNumber('compute_wait_us')) },
		{ label: 'Async experts', value: metricNumber('async_experts').toLocaleString() },
		{
			label: 'Compute with I/O pending',
			value: metricNumber('compute_with_io_pending').toLocaleString()
		},
		{ label: 'Resize pending', value: formatBytes(metricNumber('resize_pending_bytes')) },
		{ label: 'Resize failures', value: metricNumber('resize_failures').toLocaleString() }
	]);
	let prefetchMetrics = $derived<MetricItem[]>([
		{ label: 'Prefetch reads', value: metricNumber('prefetch_reads').toLocaleString() },
		{ label: 'Prefetch hits', value: metricNumber('prefetch_hits').toLocaleString() },
		{ label: 'Prefetch wasted', value: metricNumber('prefetch_wasted').toLocaleString() },
		{ label: 'Prefetch promoted', value: metricNumber('prefetch_promoted').toLocaleString() },
		{ label: 'Prefetch skipped', value: metricNumber('prefetch_skipped').toLocaleString() },
		{ label: 'PLE cache', value: formatBytes(metricNumber('ple_cache_bytes')) },
		{ label: 'PLE cache hits', value: metricNumber('ple_cache_hits').toLocaleString() },
		{ label: 'PLE pages read', value: metricNumber('ple_pages_read').toLocaleString() },
		{ label: 'PLE bytes read', value: formatBytes(metricNumber('ple_bytes_read')) },
		{ label: 'PLE evictions', value: metricNumber('ple_evictions').toLocaleString() }
	]);

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
	function formatMilliseconds(value: number) {
		return (value / 1000).toFixed(2) + ' ms';
	}
	function statusFor(id: string) {
		return modelsStore.routerModels.find((model) => model.id === id)?.status.value ?? 'unloaded';
	}
	function selectSettingsModel(id: string) {
		selectedModel = id;
		rename = library.find((model) => model.id === id)?.display_name ?? '';
		metrics = null;
		runtimeConfig = null;
	}

	async function refreshManager() {
		const baseline = JSON.stringify(savedRouterConfig);
		const status = await SparkMoeService.status();
		managerStatus = status;
		if (baseline !== JSON.stringify(savedRouterConfig)) return;
		if (!routerConfigDirty) routerConfig = { ...status.config };
		savedRouterConfig = { ...status.config };
	}
	async function refreshLibrary(rescan = false) {
		const result = await SparkMoeService.library(rescan);
		library = result.models;
		if (managerStatus?.server_ready) {
			await modelsStore.fetch(true);
			await modelsStore.fetchRouterModels();
			modelsStore.status.subscribe();
		}
		if (!library.some((model) => model.id === selectedModel)) {
			const active = modelsStore.selectedModelId;
			const ready = modelsStore.routerModels.find((model) => model.status.value === ServerModelStatus.LOADED);
			const recent = modelsStore.routerModels.find((model) => model.sparkmoe?.last_loaded);
			const preferred = modelsStore.routerModels.find((model) => model.sparkmoe?.is_default);
			const id = [active, ready?.id, recent?.id, preferred?.id].find((value) =>
				library.some((model) => model.id === value)
			);
			selectSettingsModel(id ?? (library.length === 1 ? library[0].id : ''));
		}
	}
	async function refreshRuntime() {
		const id = selectedModel;
		if (!id || !selectedModelLoaded) {
			metrics = null;
			runtimeConfig = null;
			return;
		}
		const results = await Promise.allSettled([
			SparkMoeService.paging(id),
			SparkMoeService.runtimeConfig(id),
			SparkMoeService.prometheusMetrics(id)
		]);
		if (id !== selectedModel) return;
		const [paging, runtime, rates] = results;
		metricsError = paging.status === 'rejected' ? String(paging.reason) : '';
		metrics = paging.status === 'fulfilled' ? paging.value : null;
		runtimeConfig = runtime.status === 'fulfilled' ? runtime.value : null;
		if (metrics && rates.status === 'fulfilled') {
			const count = rates.value['llamacpp:tokens_predicted_total'] ?? 0;
			const seconds = rates.value['llamacpp:tokens_predicted_seconds_total'] ?? 0;
			metrics.generation_tokens_per_second = seconds > 0 ? count / seconds : 0;
		}
	}
	async function refreshLogs() {
		try {
			const result = await SparkMoeService.logSessions();
			logSessions = result.sessions;
			logDirectory = result.directory;
			if (!logSessions.some((session) => session.name === selectedLogSession))
				selectedLogSession =
					logSessions.find((session) => session.current)?.name ?? logSessions[0]?.name ?? '';
			const session = selectedLogSession;
			if (session) {
				const chunk = await SparkMoeService.logChunk(session);
				if (session === selectedLogSession) logContent = chunk.content;
			}
			logsError = '';
		} catch (cause) {
			logsError = cause instanceof Error ? cause.message : String(cause);
		}
	}
	async function initialize() {
		await refreshManager();
		await refreshLibrary();
		hardware = await SparkMoeService.recommendation();
		initialized = true;
	}
	async function run(name: string, work: () => Promise<void>) {
		if (busy) return;
		busy = name;
		error = '';
		notice = '';
		try {
			await work();
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
		} finally {
			busy = '';
		}
	}
	async function importModel(path: string) {
		if (!path.trim()) return;
		await SparkMoeService.importModel(path);
		importPath = '';
		importOpen = false;
		await refreshLibrary();
		notice = 'Model added to the library.';
	}
	async function chooseFile() {
		const result = await SparkMoeService.chooseModelFile();
		if (!result.cancelled && result.path) await importModel(result.path);
	}
	async function chooseFolder() {
		const result = await SparkMoeService.chooseModelsDirectory();
		if (!result.cancelled && result.directory) await importModel(result.directory);
	}
	async function openChat() {
		const id = selectedModel;
		if (!id) return;
		const draft = await sparkMoeProfileStore.load(id, true);
		if (draft.saving || !(await sparkMoeProfileStore.save(id)))
			throw new Error('Finish saving the model settings before starting a chat.');
		await modelsStore.selectModelById(id);
		if (draft.pending) await SparkMoeService.applyModel(id);
		await modelsStore.fetch(true);
		if (!modelsStore.isModelLoaded(id)) await modelsStore.status.load(id);
		await goto(ROUTES.START);
	}
	async function removeModel() {
		const id = selectedModel;
		removeDialogOpen = false;
		await SparkMoeService.removeLibraryModel(id);
		await refreshLibrary();
		notice = 'Removed from the library. The model files were kept.';
	}
	async function updateModel(
		changes: Partial<Pick<SparkMoeLibraryModel, 'display_name' | 'favorite' | 'path'>>
	) {
		await SparkMoeService.updateLibraryModel(selectedModel, changes);
		await refreshLibrary();
	}
	async function lifecycle(action: 'start' | 'stop' | 'restart') {
		await SparkMoeService[action]();
		await refreshManager();
		if (managerStatus?.server_ready) await refreshLibrary();
	}
	onMount(() => {
		void run('initialize', initialize);
		const timer = setInterval(() => {
			if (refreshing) return;
			refreshing = true;
			void (async () => {
				const wasReady = managerStatus?.server_ready;
				await refreshManager();
				if (!wasReady && managerStatus?.server_ready) await refreshLibrary();
				if (activeTab === 'activity') await Promise.all([refreshRuntime(), refreshLogs()]);
			})()
				.catch(() => {
					managerStatus = null;
				})
				.finally(() => {
					refreshing = false;
				});
		}, 2500);
		return () => clearInterval(timer);
	});
</script>

<svelte:head><title>Spark Edge</title></svelte:head>

<div class="mx-auto w-full max-w-6xl space-y-6 p-4 pb-24 md:p-8">
	<header class="flex flex-wrap items-center justify-between gap-4 border-b border-border/40 pb-5">
		<div class="flex items-center gap-3">
			<div class="rounded-xl bg-primary/10 p-2.5 text-primary"><Sparkles class="h-5 w-5" /></div>
			<div>
				<h1 class="text-xl font-semibold tracking-tight">Spark Edge</h1>
				<p class="mt-1 text-sm text-muted-foreground">Your local models, ready to use.</p>
			</div>
		</div>
		<div class="flex items-center gap-2">
			<span class="text-xs text-muted-foreground"
				>{managerStatus?.server_ready
					? 'Ready'
					: managerStatus?.running
						? 'Starting...'
						: managerStatus
							? 'Server stopped'
							: 'Manager unavailable'}</span
			><Button
				aria-label="Application settings"
				href={RouterService.settings('general')}
				size="icon"
				variant="ghost"><Settings /></Button
			><Button
				aria-label={t("Refresh Spark Edge")}
				disabled={busy !== ''}
				onclick={() => void run('refresh', initialize)}
				size="icon"
				variant="ghost"><RefreshCw class={busy === 'refresh' ? 'animate-spin' : ''} /></Button
			>
		</div>
	</header>
	<nav aria-label="Spark Edge sections" class="flex gap-1 rounded-lg bg-muted/50 p-1">
		{#each [{ id: 'models', label: 'Models', icon: Library }, { id: 'connections', label: 'Connections', icon: Link }, { id: 'activity', label: 'Activity', icon: Activity }, { id: 'engines', label: 'Engines', icon: Settings }] as tab}<Button
				aria-current={activeTab === tab.id ? 'page' : undefined}
				class="flex-1 sm:flex-none"
				onclick={() => {
					activeTab = tab.id as Tab;
					if (activeTab === 'engines') void run('engines', async () => {
						engineStatus = await SparkMoeService.engines();
					});
					if (activeTab === 'activity')
						void run('activity', async () => {
							await Promise.all([refreshLogs(), refreshRuntime()]);
						});
				}}
				variant={activeTab === tab.id ? 'secondary' : 'ghost'}><tab.icon />{t(tab.label)}</Button
			>{/each}
	</nav>
	{#if managerStatus?.configuration_warning}<div
			class="rounded-lg border border-amber-500/30 bg-amber-500/10 p-3 text-sm"
			role="status"
		>
			{managerStatus.configuration_warning}
		</div>{/if}
	{#if error}<div
			class="rounded-lg border border-destructive/30 bg-destructive/10 p-3 text-sm text-destructive"
			role="alert"
		>
			{error}
		</div>{/if}
	{#if notice}<div class="rounded-lg bg-muted/50 p-3 text-sm" role="status">{notice}</div>{/if}
	{#if !managerStatus}<Card.Root
			><Card.Content class="space-y-3"
				><p class="text-sm">Open Spark Edge from its shortcut to manage local models.</p>
				<Button
					disabled={busy !== ''}
					onclick={() => void run('retry', initialize)}
					variant="outline">{t("Reconnect")}</Button
				></Card.Content
			></Card.Root
		>{/if}

	{#if activeTab === 'models'}
		<div class="flex flex-wrap items-center justify-between gap-3">
			<div>
				<h2 class="text-lg font-semibold">{t("Model library")}</h2>
				<p class="text-xs text-muted-foreground">
					{library.length} models | Chat model: {modelsStore.selectedModelName
						? (library.find((item) => item.id === modelsStore.selectedModelName)?.display_name ??
							modelsStore.selectedModelName)
						: 'None'}
				</p>
			</div>
			<div class="flex gap-2">
				<Button
					disabled={!managerStatus}
					onclick={() => (importOpen = !importOpen)}
					variant="outline"><Plus />{t("Add local model")}</Button
				><Button
					disabled={!managerStatus}
					onclick={() => (downloadsOpen = !downloadsOpen)}
					variant="outline"><Download />{t("Download")}</Button
				>
			</div>
		</div>
		{#if importOpen || (initialized && library.length === 0)}<Card.Root
				><Card.Header
					><Card.Title>{library.length ? 'Add a local model' : 'Add your first model'}</Card.Title
					><Card.Description
						>Select a GGUF file or a folder. Split files and companion weights are grouped
						automatically.</Card.Description
					></Card.Header
				><Card.Content class="space-y-4"
					><div class="flex flex-wrap gap-2">
						<Button disabled={busy !== ''} onclick={() => void run('import', chooseFile)}
							><Plus />Choose GGUF file</Button
						><Button
							disabled={busy !== ''}
							onclick={() => void run('import', chooseFolder)}
							variant="outline"><FolderOpen />Import folder</Button
						>
					</div>
					<form
						class="flex gap-2"
						onsubmit={(event) => {
							event.preventDefault();
							void run('import', () => importModel(importPath));
						}}
					>
						<input
							aria-label="Local model file or folder path"
							class="min-w-0 flex-1 rounded-md border border-input bg-background px-3 py-2 text-sm"
							dir="ltr"
							placeholder="Or paste a local file or folder path"
							bind:value={importPath}
						/><Button disabled={!importPath.trim() || busy !== ''} type="submit" variant="outline"
							>{t("Add")}</Button
						>
					</form></Card.Content
				></Card.Root
			>{/if}
		{#if downloadsOpen}<Card.Root
				><Card.Header><Card.Title>Download models</Card.Title></Card.Header><Card.Content
					><SparkMoeDownloads onComplete={() => refreshLibrary()} /></Card.Content
				></Card.Root
			>{/if}
		{#if library.length}<div class="grid items-start gap-6 lg:grid-cols-[18rem_minmax(0,1fr)]">
				<aside aria-label={t("Model library")} class="space-y-3">
					<div class="flex gap-2">
						<div class="relative min-w-0 flex-1">
							<Search class="absolute top-3 left-3 h-4 w-4 text-muted-foreground" /><input
								aria-label="Search model library"
								class="w-full rounded-md border border-input bg-background py-2 pr-3 pl-9 text-sm"
								placeholder="Find a model"
								bind:value={search}
							/>
						</div>
						<Button
							aria-label="Show favorites only"
							aria-pressed={favoritesOnly}
							onclick={() => (favoritesOnly = !favoritesOnly)}
							size="icon"
							variant={favoritesOnly ? 'secondary' : 'outline'}
							><Star class={favoritesOnly ? 'fill-current' : ''} /></Button
						>
					</div>
					<div class="max-h-[65vh] space-y-2 overflow-y-auto">
						{#each filteredModels as model}<button
								aria-pressed={selectedModel === model.id}
								class={[
									'w-full rounded-lg border p-3 text-left transition-colors hover:bg-muted/50',
									selectedModel === model.id ? 'border-primary bg-primary/5' : 'border-border/60'
								]}
								onclick={() => selectSettingsModel(model.id)}
								type="button"
								><span class="block break-words text-sm font-medium"
									>{#if model.favorite}<Star
											class="mr-1 inline h-3 w-3 fill-current"
										/>{/if}{model.display_name}</span
								><span class="mt-2 block text-xs text-muted-foreground"
									>{model.available ? statusFor(model.id) : 'File missing'} | {formatBytes(
										model.size_bytes
									)}</span
								></button
							>{/each}{#if !filteredModels.length}<p
								class="py-6 text-center text-sm text-muted-foreground"
							>
								No matching models.
							</p>{/if}
					</div>
				</aside>
				<div class="min-w-0 space-y-5">
					{#if selected}<Card.Root
							><Card.Header
								><div class="flex items-start justify-between gap-3">
									<div class="min-w-0">
										<Card.Title class="break-words">{selected.display_name}</Card.Title
										><Card.Description class="mt-2"
											>{selected.available
												? selectedModelStatus
												: 'File missing'}{modelsStore.selectedModelName === selectedModel
												? ' | Selected for chat'
												: ''}</Card.Description
										>
									</div>
									<Button
										aria-label={selected.favorite ? 'Remove favorite' : 'Add favorite'}
										disabled={busy !== ''}
										onclick={() =>
											void run('favorite', () => updateModel({ favorite: !selected!.favorite }))}
										size="icon"
										variant="ghost"><Star class={selected.favorite ? 'fill-current' : ''} /></Button
									>
								</div></Card.Header
							><Card.Content class="space-y-4"
								><div class="flex flex-wrap gap-2">
									<Button
										disabled={!selected.available ||
											!managerStatus ||
											busy !== '' ||
											selectedModelBusy}
										onclick={() => void run('chat', openChat)}
										>{#if busy === 'chat'}<LoaderCircle class="animate-spin" />{:else}<MessageSquare
											/>{/if}Use in chat</Button
									>{#if selectedModelLoaded}<Button
											disabled={busy !== '' || selectedModelBusy}
											onclick={() =>
												void run('unload', () => modelsStore.status.unload(selectedModel))}
											variant="outline"><CircleStop />{t("Unload")}</Button
										>{/if}
								</div>
								{#if !selected.available}<div class="rounded-lg bg-amber-500/10 p-3 text-sm">
										The model file is missing. Locate it to keep this model's saved settings.<Button
											class="mt-2"
											disabled={busy !== ''}
											onclick={() =>
												void run('locate', async () => {
													const file = await SparkMoeService.chooseModelFile();
													if (file.path && !file.cancelled) await updateModel({ path: file.path });
												})}
											size="sm"
											variant="outline"><FolderOpen />Locate file</Button
										>
									</div>{/if}
								<details class="text-sm">
									<summary class="cursor-pointer font-medium">Model information</summary>
									<div class="mt-3 space-y-3">
										<p class="break-all text-xs text-muted-foreground" dir="ltr">{selected.path}</p>
										<dl class="grid grid-cols-2 gap-2 text-xs">
											<dt class="text-muted-foreground">Size</dt>
											<dd>{formatBytes(selected.size_bytes)}</dd>
											{#each Object.entries(selected.metadata ?? {}) as [key, value]}{#if typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean'}<dt
														class="break-words text-muted-foreground"
													>
														{key.replaceAll('_', ' ')}
													</dt>
													<dd class="break-words">{String(value)}</dd>{/if}{/each}
										</dl>
										<form
											class="flex gap-2"
											onsubmit={(event) => {
												event.preventDefault();
												void run('rename', () => updateModel({ display_name: rename }));
											}}
										>
											<input
												aria-label="Model display name"
												class="min-w-0 flex-1 rounded-md border border-input bg-background px-3 py-2"
												bind:value={rename}
											/><Button
												disabled={!rename.trim() || rename === selected.display_name || busy !== ''}
												type="submit"
												variant="outline">{t("Rename")}</Button
											>
										</form>
										<Button
											disabled={busy !== ''}
											onclick={() => (removeDialogOpen = true)}
											size="sm"
											variant="ghost"><Trash2 />Remove from library</Button
										>
									</div>
								</details></Card.Content
							></Card.Root
						><SparkMoeModelSettings
							available={selected.available && Boolean(managerStatus)}
							loaded={selectedModelLoaded}
							model={selectedModel}
							onApplied={() => refreshLibrary()}
						/>{/if}
				</div>
			</div>{/if}
		<details class="rounded-lg border border-border/60 p-4">
			<summary class="cursor-pointer text-sm font-medium">Library folder</summary>
			<p class="my-3 text-xs text-muted-foreground">
				Save a discovery folder, then rescan to update the library.
			</p>
			<div class="flex gap-2">
				<input
					aria-label="Model discovery folder"
					class="min-w-0 flex-1 rounded-md border border-input bg-background px-3 py-2 text-sm"
					dir="ltr"
					bind:value={routerConfig.model_directory}
				/><Button
					disabled={busy !== ''}
					onclick={() =>
						void run('folder', async () => {
							const folder = await SparkMoeService.chooseModelsDirectory();
							if (folder.directory && !folder.cancelled)
								routerConfig.model_directory = folder.directory;
						})}
					size="icon"
					variant="outline"><FolderOpen /></Button
				>
			</div>
			<div class="mt-3 flex flex-wrap gap-2">
				<Button
					disabled={!routerConfigDirty || busy !== ''}
					onclick={() =>
						void run('folder-save', async () => {
							await SparkMoeService.saveRouterConfig({
								model_directory: routerConfig.model_directory
							});
							savedRouterConfig = { ...routerConfig };
							notice = 'Folder saved. Rescan when ready.';
						})}
					size="sm"
					variant="outline">Save folder</Button
				><Button
					disabled={routerConfigDirty || busy !== ''}
					onclick={() => void run('rescan', () => refreshLibrary(true))}
					size="sm"
					variant="outline"><RefreshCw />Rescan saved folder</Button
				>
			</div>
		</details>
	{:else if activeTab === 'connections'}<SparkMoeConnections models={library} {selectedModel} />
	{:else if activeTab === 'engines'}
		<Card.Root>
			<Card.Header>
				<Card.Title>{t('Installed engines')}</Card.Title>
				<Card.Description>{t('Active engine')}: <bdi>{engineStatus?.active || t('Bundled CPU engine')}</bdi></Card.Description>
			</Card.Header>
			<Card.Content class="space-y-3">
				<Button disabled={busy !== ''} onclick={() => void run('engine-check', async () => {
					engineStatus = await SparkMoeService.engines(true);
				})} variant="outline"><RefreshCw />{t('Check for engine updates')}</Button>
				{#each [{ id: '', backend: t('Bundled CPU engine') }, ...(engineStatus?.installed ?? [])] as engine (engine.id)}
					<div class="flex flex-wrap items-center justify-between gap-2 rounded-md border p-3">
						<span><bdi>{engine.backend}</bdi>{#if engine.id}<bdi class="ms-2 text-xs">{engine.id.slice(engine.backend.length + 1, engine.backend.length + 9)}</bdi>{/if}</span>
						<Button disabled={busy !== '' || (engineStatus?.active ?? '') === engine.id}
							onclick={() => void run('engine-apply', async () => {
								try { await SparkMoeService.applyEngine(engine.id); }
								finally { engineStatus = await SparkMoeService.engines(); }
								await refreshManager();
								await refreshLibrary();
							})}>{t('Apply engine')}</Button>
					</div>
				{/each}
				{#if engineStatus?.available?.length === 0}<p class="text-sm text-muted-foreground">{t('No engine packages are available.')}</p>{/if}
				{#each engineStatus?.available?.filter((engine) => !engineStatus?.installed.some((row) => row.id === engine.id)) ?? [] as engine (engine.id)}
					<div class="flex flex-wrap items-center justify-between gap-2 rounded-md border p-3">
						<span><bdi>{engine.id}</bdi></span>
						<Button disabled={busy !== ''} variant="outline" onclick={() => void run('engine-download', async () => {
							await SparkMoeService.downloadEngine(engine.id);
							engineStatus = await SparkMoeService.engines(true);
						})}>{t('Install engine')}</Button>
					</div>
				{/each}
			</Card.Content>
		</Card.Root>
	{:else}
		<Card.Root
			><Card.Header
				><Card.Title>Server activity</Card.Title><Card.Description
					>{managerStatus?.server_ready
						? 'Ready for requests'
						: managerStatus?.running
							? 'Starting the server'
							: 'Server stopped'}</Card.Description
				></Card.Header
			><Card.Content class="space-y-4"
				>{#if managerStatus?.last_error}<p class="text-sm text-destructive">
						{managerStatus.last_error}
					</p>{/if}
				<div class="flex flex-wrap gap-2">
					{#if managerStatus?.running}<Button
							disabled={busy !== ''}
							onclick={() => void run('restart', () => lifecycle('restart'))}
							variant="outline"><RefreshCw />Restart server</Button
						><Button
							disabled={busy !== ''}
							onclick={() => void run('stop', () => lifecycle('stop'))}
							variant="outline"><CircleStop />Stop server</Button
						>{:else}<Button
							disabled={!managerStatus || busy !== ''}
							onclick={() => void run('start', () => lifecycle('start'))}
							><Play />Start server</Button
						>{/if}<Button
						disabled={!managerStatus || busy !== ''}
						onclick={() => (exitDialogOpen = true)}
						variant="ghost"><Power />{t("Exit Spark Edge")}</Button
					>
				</div>
				<p class="text-xs text-muted-foreground">
					Application: {SparkMoeService.managerUrl} | Model API port: {managerStatus?.port ?? '-'} | PID:
					{managerStatus?.pid || '-'}
				</p></Card.Content
			></Card.Root
		>
		<Card.Root
			><Card.Header><Card.Title>Model activity</Card.Title></Card.Header><Card.Content
				class="space-y-4"
				><label class="grid gap-2 text-sm"
					><span>{t("Model")}</span><select
						class="rounded-md border border-input bg-background px-3 py-2"
						onchange={(event) => {
							selectSettingsModel(event.currentTarget.value);
							void refreshRuntime();
						}}
						value={selectedModel}
						>{#each library as model}<option value={model.id}
								>{model.display_name} ({statusFor(model.id)})</option
							>{/each}</select
					></label
				>{#if selectedModelLoaded && metrics}<div class="grid gap-3 sm:grid-cols-3">
						{#each [cacheMetrics[0], memoryMetrics[0], cacheMetrics[1]] as metric}<div
								class="rounded-lg bg-muted/50 p-3"
							>
								<span class="text-xs text-muted-foreground">{metric.label}</span><strong
									class="mt-1 block">{metric.value}</strong
								>
							</div>{/each}
					</div>
					<details>
						<summary class="cursor-pointer text-sm font-medium">Detailed counters</summary>
						<div class="mt-4 space-y-5">
							{#each [{ title: 'Cache and experts', items: cacheMetrics }, { title: 'Memory and storage', items: memoryMetrics }, { title: 'I/O pipeline', items: pipelineMetrics }, { title: 'Prefetch and PLE', items: prefetchMetrics }] as group}<section
								>
									<h3 class="mb-2 text-sm font-medium">{group.title}</h3>
									<dl class="grid grid-cols-2 gap-3">
										{#each group.items as metric}<div class="rounded-lg bg-muted/30 p-3">
												<dt class="text-xs text-muted-foreground">{metric.label}</dt>
												<dd class="mt-1 text-sm">{metric.value}</dd>
											</div>{/each}
									</dl>
								</section>{/each}
						</div>
						<Button
							class="mt-4"
							disabled={busy !== ''}
							onclick={() =>
								void run('reset-metrics', async () => {
									await SparkMoeService.resetMetrics(selectedModel);
									await refreshRuntime();
								})}
							size="sm"
							variant="ghost">Reset counters</Button
						>
					</details>{:else}<p class="text-sm text-muted-foreground">
						Load a model to see its runtime measurements.
					</p>{/if}{#if metricsError}<p class="text-xs text-muted-foreground">
						{metricsError}
					</p>{/if}
				<details>
					<summary class="cursor-pointer text-sm font-medium"
						>Effective runtime and hardware</summary
					>
					<pre
						class="mt-3 max-h-80 overflow-auto rounded-md bg-muted/50 p-3 text-xs"
						dir="ltr">{JSON.stringify({ runtime: runtimeConfig, hardware }, null, 2)}</pre>
				</details></Card.Content
			></Card.Root
		>
		<Card.Root
			><Card.Header
				><Card.Title>Logs and diagnostics</Card.Title><Card.Description
					>Inspect the current session or download logs for troubleshooting.</Card.Description
				></Card.Header
			><Card.Content class="space-y-4"
				><div class="flex flex-wrap gap-2">
					<select
						aria-label="Log session"
						class="min-w-0 flex-1 rounded-md border border-input bg-background px-3 py-2 text-sm"
						onchange={() => void refreshLogs()}
						bind:value={selectedLogSession}
						>{#each logSessions as session}<option value={session.name}
								>{session.name}{session.current ? ' (current)' : ''}</option
							>{/each}</select
					><Button
						disabled={!selectedLogSession}
						href={selectedLogSession
							? SparkMoeService.logDownloadUrl(selectedLogSession)
							: undefined}
						size="sm"
						variant="outline"><Download />Log</Button
					><Button href={SparkMoeService.diagnosticsUrl()} size="sm" variant="outline"
						><Download />Diagnostics</Button
					>
				</div>
				{#if logsError}<p class="text-sm text-destructive">{logsError}</p>{/if}
				<pre
					class="max-h-[32rem] min-h-40 overflow-auto rounded-lg border border-border bg-muted/30 p-4 text-xs"
					dir="ltr">{logContent || 'No log output yet.'}</pre>
				<p class="break-all text-xs text-muted-foreground">{logDirectory}</p></Card.Content
			></Card.Root
		>
	{/if}
</div>

<DialogConfirmation
	bind:open={removeDialogOpen}
	cancelText="Cancel"
	confirmText="Remove from library"
	description="Remove this library entry and keep the model files on disk."
	icon={Trash2}
	onCancel={() => (removeDialogOpen = false)}
	onConfirm={() => void run('remove', removeModel)}
	title="Remove model from library"
	variant="destructive"
/>
<DialogConfirmation
	bind:open={exitDialogOpen}
	cancelText="Cancel"
	confirmText={t("Exit Spark Edge")}
	description="Stop the server and all loaded models, then close Spark Edge."
	icon={Power}
	onCancel={() => (exitDialogOpen = false)}
	onConfirm={() => {
		exitDialogOpen = false;
		void run('exit', async () => {
			await SparkMoeService.shutdown();
			managerStatus = null;
			notice = 'Spark Edge has exited. You can close this tab.';
		});
	}}
	title={t("Exit Spark Edge")}
	variant="destructive"
/>
