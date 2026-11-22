<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
<!-- Required Notice: Copyright (c) 2026 SparkMoE contributors. -->
<script lang="ts">
	import { t } from '$lib/i18n/locale.svelte';
	import { Download, LoaderCircle, Pause, Play, Search } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import { SparkMoeService, type SparkMoeDownload } from '$lib/services/sparkmoe.service';
	import { onMount } from 'svelte';
	let { onComplete }: { onComplete: () => Promise<void> } = $props();
	let query = $state('');
	let url = $state('');
	let results = $state<{ id: string; downloads: number }[]>([]);
	let files = $state<{ name: string; url: string; size_bytes: number }[]>([]);
	let jobs = $state<SparkMoeDownload[]>([]);
	let busy = $state('');
	let error = $state('');
	let searched = $state(false);
	let selectedRepo = $state('');
	let refreshing = false;
	function size(bytes: number) {
		return bytes > 0 ? (bytes / 1024 ** 3).toFixed(2) + ' GiB' : t('Size unknown');
	}
	async function refresh() {
		if (refreshing) return;
		refreshing = true;
		try {
			const result = await SparkMoeService.downloads();
			const completed = result.jobs.some(
				(job) =>
					job.status === 'completed' &&
					jobs.find((previous) => previous.id === job.id)?.status !== 'completed'
			);
			jobs = result.jobs;
			if (completed) await onComplete();
		} finally {
			refreshing = false;
		}
	}
	async function run(name: string, work: () => Promise<void>) {
		busy = name;
		error = '';
		try {
			await work();
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
		} finally {
			busy = '';
		}
	}
	onMount(() => {
		void run('refresh', refresh);
		const timer = setInterval(() => void refresh().catch(() => {}), 2500);
		return () => clearInterval(timer);
	});
</script>

<div class="space-y-4">
	<p class="text-sm text-muted-foreground">
		Find GGUF models on Hugging Face or paste a file download link. Completed models are added to
		your library.
	</p>
	<form
		class="flex gap-2"
		onsubmit={(event) => {
			event.preventDefault();
			void run('search', async () => {
				results = (await SparkMoeService.searchDownloads(query)).models;
				searched = true;
				files = [];
			});
		}}
	>
		<input
			aria-label={t("Search Hugging Face models")}
			class="min-w-0 flex-1 rounded-md border border-input bg-background px-3 py-2 text-sm"
			placeholder={t("Search models")}
			bind:value={query}
		/><Button disabled={!query.trim() || busy !== ''} type="submit" variant="outline"
			><Search />{t("Search")}</Button
		>
	</form>
	{#if searched && !results.length}<p class="text-sm text-muted-foreground">
			No models found.
		</p>{/if}
	{#if results.length}<div
			class="max-h-56 space-y-1 overflow-y-auto rounded-lg border border-border p-2"
		>
			{#each results as result}<button
					class="block w-full rounded-md p-2 text-left text-sm hover:bg-muted"
					onclick={() =>
						void run('catalog', async () => {
							selectedRepo = result.id;
							files = (await SparkMoeService.downloadCatalog(result.id)).files;
						})}
					type="button"
					>{result.id}<span class="ml-2 text-xs text-muted-foreground"
						>{result.downloads.toLocaleString()} downloads</span
					></button
				>{/each}
		</div>{/if}
	{#if files.length}<div class="space-y-2">
			<p class="text-sm font-medium">{selectedRepo}</p>
			<div class="max-h-64 space-y-2 overflow-y-auto">
				{#each files as file}<div
						class="flex items-center justify-between gap-3 rounded-lg border border-border p-3"
					>
						<div class="min-w-0">
							<p class="break-all text-sm">{file.name}</p>
							<p class="text-xs text-muted-foreground">{size(file.size_bytes)}</p>
						</div>
						<Button
							aria-label={'Download ' + file.name}
							disabled={busy !== ''}
							onclick={() =>
								void run('download', async () => {
									await SparkMoeService.download(file.url);
									await refresh();
								})}
							size="icon"
							variant="outline"><Download /></Button
						>
					</div>{/each}
			</div>
		</div>{/if}
	<details>
		<summary class="cursor-pointer text-sm font-medium">{t("Download from a link")}</summary>
		<form
			class="mt-3 flex gap-2"
			onsubmit={(event) => {
				event.preventDefault();
				void run('download', async () => {
					await SparkMoeService.download(url);
					url = '';
					await refresh();
				});
			}}
		>
			<input
				aria-label={t("GGUF file download link")}
				class="min-w-0 flex-1 rounded-md border border-input bg-background px-3 py-2 text-sm"
				dir="ltr"
				placeholder="https://huggingface.co/.../resolve/main/model.gguf"
				type="url"
				bind:value={url}
			/><Button disabled={!url.trim() || busy !== ''} type="submit"><Download />{t("Download")}</Button>
		</form>
	</details>
	{#if busy}<p class="flex items-center gap-2 text-sm text-muted-foreground">
			<LoaderCircle class="h-4 w-4 animate-spin" />{t("Working...")}
		</p>{/if}
	{#if error}<p class="text-sm text-destructive" role="alert">{error}</p>{/if}
	{#each jobs as job}<div class="space-y-2 rounded-lg border border-border p-3">
			<div class="flex items-center justify-between gap-3">
				<div class="min-w-0">
					<p class="truncate text-sm font-medium">{job.filename}</p>
					<p class="text-xs text-muted-foreground">
						{job.status} | {size(job.downloaded_bytes)}{job.total_bytes > 0
							? ' / ' + size(job.total_bytes)
							: ''}
					</p>
				</div>
				{#if job.status !== 'completed'}<Button
						disabled={busy !== ''}
						onclick={() =>
							void run('job', async () => {
								await SparkMoeService.downloadAction(
									job.status === 'downloading' || job.status === 'queued' ? 'pause' : 'resume',
									job.id
								);
								await refresh();
							})}
						size="sm"
						variant="outline"
						>{#if job.status === 'downloading' || job.status === 'queued'}<Pause />{t('Pause')}{:else}<Play
							/>{t('Resume')}{/if}</Button
					>{/if}
			</div>
			{#if job.total_bytes > 0}<progress
					aria-label={job.filename}
					class="h-1.5 w-full accent-primary"
					max={job.total_bytes}
					value={job.downloaded_bytes}
				></progress>{/if}{#if job.error}<p class="text-xs text-destructive">{job.error}</p>{/if}
		</div>{/each}
</div>
