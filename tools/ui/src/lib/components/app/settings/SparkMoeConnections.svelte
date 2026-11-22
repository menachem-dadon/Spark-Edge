<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
<!-- Required Notice: Copyright (c) 2026 SparkMoE contributors. -->
<script lang="ts">
	import { t } from '$lib/i18n/locale.svelte';
	import { Copy, Eye, FolderOpen, LoaderCircle, Play, Plus, Trash2 } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Card from '$lib/components/ui/card';
	import {
		SparkMoeService,
		type SparkMoeConnection,
		type SparkMoeConnectionInput,
		type SparkMoeConnectionPreview,
		type SparkMoeConnector,
		type SparkMoeLibraryModel
	} from '$lib/services/sparkmoe.service';
	import { onMount } from 'svelte';
	let { models, selectedModel }: { models: SparkMoeLibraryModel[]; selectedModel: string } =
		$props();
	let connectors = $state<SparkMoeConnector[]>([]);
	let connections = $state<SparkMoeConnection[]>([]);
	let connector = $state('');
	let model = $state('');
	let project = $state('');
	let favorites = $state(false);
	let experimental = $state(false);
	let preview = $state<SparkMoeConnectionPreview | null>(null);
	let previewInput = $state('');
	let busy = $state('');
	let message = $state('');
	let error = $state('');
	let input = $derived<SparkMoeConnectionInput>({
		connector,
		model_id: model,
		project_dir: project,
		include_favorites: favorites,
		experimental
	});
	let previewMatches = $derived(previewInput === JSON.stringify(input));
	let currentConnector = $derived(connectors.find((item) => item.id === connector));

	async function refresh() {
		const result = await SparkMoeService.connections();
		connectors = result.connectors;
		connections = result.connections;
	}
	async function run(name: string, work: () => Promise<void>) {
		busy = name;
		error = '';
		message = '';
		try {
			await work();
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
		} finally {
			busy = '';
		}
	}
	async function showPreview() {
		const snapshot = { ...input };
		preview = await SparkMoeService.previewConnection(snapshot);
		previewInput = JSON.stringify(snapshot);
	}
	async function configure() {
		if (!previewMatches) return;
		const result = await SparkMoeService.configureConnection({ ...input });
		preview = result;
		await refresh();
		message = 'Connection saved. You can now launch it or copy its configuration.';
	}
	onMount(() => {
		model = selectedModel;
		void run('refresh', refresh);
	});
</script>

<div class="space-y-6">
	<div>
		<h2 class="text-lg font-semibold">Connect your tools</h2>
		<p class="mt-1 text-sm text-muted-foreground">
			Choose a local model, preview the configuration, then save a connection for your project.
		</p>
	</div>
	{#if error}<p class="rounded-lg bg-destructive/10 p-3 text-sm text-destructive" role="alert">
			{error}
		</p>{/if}
	{#if message}<p class="rounded-lg bg-muted/50 p-3 text-sm" role="status">{message}</p>{/if}
	<div class="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
		{#each connectors as item}<button
				class={[
					'rounded-xl border p-4 text-left transition-colors hover:bg-muted/50',
					connector === item.id ? 'border-primary bg-primary/5' : 'border-border'
				]}
				disabled={item.id === 'codex-desktop'}
				onclick={() => {
					connector = item.id;
					experimental = false;
				}}
				type="button"
				><span class="font-medium">{item.name}</span><span
					class="mt-2 block text-xs text-muted-foreground"
					>{item.installed ? 'Detected on this computer' : 'Not detected'}</span
				><span class="mt-1 block text-xs text-muted-foreground"
					>{item.support}{item.version ? ' | ' + item.version : ''}</span
				></button
			>{/each}
	</div>
	{#if connector}
		<Card.Root
			><Card.Header
				><Card.Title>{currentConnector?.name} connection</Card.Title><Card.Description
					>Configuration is kept in SparkMoE. Your existing tool settings stay available.</Card.Description
				></Card.Header
			><Card.Content class="space-y-4">
				<label class="grid gap-2 text-sm"
					><span class="font-medium">{t("Model")}</span><select
						class="rounded-md border border-input bg-background p-2"
						bind:value={model}
						><option value="">Choose a model</option
						>{#each models.filter((item) => item.available) as item}<option value={item.id}
								>{item.display_name}</option
							>{/each}</select
					></label
				>
				<label class="grid gap-2 text-sm"
					><span class="font-medium">Project folder</span><input
						class="rounded-md border border-input bg-background p-2"
						dir="ltr"
						placeholder="C:\Projects\my-project"
						bind:value={project}
					/></label
				>
				<label class="flex items-center gap-3 text-sm"
					><input bind:checked={favorites} type="checkbox" />Include favorite models where supported</label
				>
				{#if connector === 'claude-code'}<label
						class="flex items-start gap-3 rounded-lg border border-amber-500/40 p-3 text-sm"
						><input class="mt-1" bind:checked={experimental} type="checkbox" /><span
							>Enable experimental Claude Code compatibility. Tool behavior depends on the model and
							does not imply Claude model support.</span
						></label
					>{/if}
				{#if currentConnector && !currentConnector.launch_supported}<p
						class="text-xs text-muted-foreground"
					>
						This connector exports configuration. Open the tool and add the generated settings
						manually.
					</p>{/if}
				<Button
					disabled={!model || busy !== '' || (connector === 'claude-code' && !experimental)}
					onclick={() => void run('preview', showPreview)}
					variant="outline"><Eye />Preview configuration</Button
				>
				{#if preview && previewMatches}<div class="space-y-3 rounded-lg border border-border p-4">
						{#each preview.warnings as warning}<p
								class="text-sm text-amber-600 dark:text-amber-400"
							>
								{warning}
							</p>{/each}
						{#each preview.files as file}<details>
								<summary class="cursor-pointer text-sm font-medium">{file.name}</summary>
								<pre
									class="my-3 max-h-72 overflow-auto rounded-md bg-muted/50 p-3 text-xs"
									dir="ltr">{file.content}</pre>
								<Button
									onclick={() =>
										void run('copy', async () => {
											await navigator.clipboard.writeText(file.content);
											message = 'Configuration copied.';
										})}
									size="sm"
									variant="ghost"><Copy />{t("Copy")}</Button
								>
							</details>{/each}
						{#if preview.arguments?.length}<details>
								<summary class="cursor-pointer text-sm font-medium">Launch arguments</summary>
								<pre class="mt-3 overflow-auto text-xs">{preview.arguments.join('\n')}</pre>
							</details>{/if}
						<Button disabled={busy !== ''} onclick={() => void run('save', configure)}
							>{#if busy === 'save'}<LoaderCircle class="animate-spin" />{:else}<Plus />{/if}Save
							connection</Button
						>
					</div>{/if}
			</Card.Content></Card.Root
		>
	{/if}
	<Card.Root
		><Card.Header><Card.Title>Saved connections</Card.Title></Card.Header><Card.Content
			class="space-y-3"
		>
			{#if !connections.length}<p class="text-sm text-muted-foreground">
					Your saved project connections will appear here.
				</p>{/if}
			{#each connections as connection}<div
					class="flex flex-wrap items-center justify-between gap-3 rounded-lg border border-border/60 p-4"
				>
					<div class="min-w-0">
						<p class="font-medium">
							{connectors.find((item) => item.id === connection.connector)?.name ??
								connection.connector}
						</p>
						<p class="mt-1 break-all text-sm text-muted-foreground">
							<FolderOpen class="mr-1 inline h-3 w-3" />{connection.project_dir}
						</p>
						<p class="mt-1 text-xs text-muted-foreground">
							{models.find((item) => item.id === connection.model_id)?.display_name ??
								connection.model_id}
						</p>
					</div>
					<div class="flex gap-2">
						<Button
							disabled={busy !== '' ||
								!connectors.find((item) => item.id === connection.connector)?.launch_supported}
							onclick={() =>
								void run('launch', async () => {
									await SparkMoeService.connectionAction('launch', connection.id);
									message = 'Connection launched.';
								})}
							size="sm"><Play />Launch</Button
						><Button
							aria-label="Remove saved connection"
							disabled={busy !== ''}
							onclick={() =>
								void run('remove', async () => {
									await SparkMoeService.connectionAction('remove', connection.id);
									await refresh();
								})}
							size="icon"
							variant="ghost"><Trash2 /></Button
						>
					</div>
				</div>{/each}
		</Card.Content></Card.Root
	>
	<p class="text-xs text-muted-foreground">
		Codex desktop model selection depends on the installed desktop version. A Codex CLI connection
		does not add a model to the desktop model picker.
	</p>
</div>
