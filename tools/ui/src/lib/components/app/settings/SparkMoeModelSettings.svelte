<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
<!-- Required Notice: Copyright (c) 2026 SparkMoE contributors. -->
<script lang="ts">
	import { t } from '$lib/i18n/locale.svelte';
	import { Check, LoaderCircle, RefreshCw, RotateCcw } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Card from '$lib/components/ui/card';
	import { SparkMoeService, type SparkMoeModelProfile, type EnginePackage } from '$lib/services/sparkmoe.service';
	import { sparkMoeProfileStore } from '$lib/stores/sparkmoe-profile.svelte';
	import { untrack } from 'svelte';

	let {
		available = true,
		loaded = false,
		model,
		onApplied
	}: {
		available?: boolean;
		loaded?: boolean;
		model: string;
		onApplied?: () => Promise<void>;
	} = $props();
	let engines = $state<EnginePackage[]>([]);
	let loadError = $state('');
	let action = $state('');
	let actionMessage = $state('');
	let draft = $derived(sparkMoeProfileStore.drafts.get(model));
	let dirty = $derived(draft ? sparkMoeProfileStore.dirty(draft) : false);
	let supportsReasoning = $derived(draft?.recommendation.supports_reasoning === true);

    let supportsMtp = $derived(draft?.recommendation.supports_mtp);
    const speculativeMethods = [
        'draft-simple', 'draft-eagle3', 'draft-mtp', 'draft-dflash', 'draft-dflash2', 'draft-dspark',
        'draft-lilicorr', 'ngram-simple', 'ngram-map-k', 'ngram-map-k4v', 'ngram-mod', 'ngram-cache'
    ];
    let speculation = $derived((draft?.value.speculative_types || (draft?.value.mtp_enabled ? 'draft-mtp' : 'none')).split(','));

    const ngramMethods = ['ngram-simple', 'ngram-map-k', 'ngram-map-k4v', 'ngram-mod'];
    const ngramMapFields = [
        { key: 'size-n', label: 'Lookup length', min: 1, max: 1024, initial: 12 },
        { key: 'size-m', label: 'Draft length', min: 1, max: 1024, initial: 48 },
        { key: 'min-hits', label: 'Minimum matches', min: 1, max: 65535, initial: 1 }
    ];
    const ngramModFields = [
        { key: 'n-match', label: 'Lookup length', min: 1, max: 1024, initial: 24 },
        { key: 'n-min', label: 'Minimum draft length', min: 0, max: 1024, initial: 48 },
        { key: 'n-max', label: 'Maximum draft length', min: 0, max: 1024, initial: 64 }
    ];

    function changeNgram(key: string, value: number) {
        if (!draft || !Number.isInteger(value)) return;
        change('ngram_options', { ...draft.value.ngram_options, [key]: value });
    }

    function toggleSpeculation(method: string, selected: boolean) {
        const remaining = speculation.filter((type) => type !== 'none' && type !== method);
        const compatible = selected && method.startsWith('draft-') ? remaining.filter((type) => !type.startsWith('draft-')) : remaining;
        const methods = selected ? [...compatible, method] : compatible;
        change('speculative_types', methods.join(',') || 'none');
    }
	type Field = {
		key: keyof SparkMoeModelProfile;
		label: string;
		options?: string[];
		type?: 'boolean' | 'text';
		min?: number;
		max?: number;
		step?: number;
		help?: string;
	};
	const runMode: Field = {
		key: 'mode',
		label: 'Model run mode',
		options: ['auto', 'os', 'explicit', 'off'],
		help: 'Automatic uses standard loading when the model fits in available memory, and streams experts from SSD for larger MoE models. Changes take effect when the model is loaded.'
	};
	const optionLabels: Record<string, string> = {
		auto: 'Automatic (recommended)', default: 'Model default', manual: 'Manual',
		off: 'Off', on: 'On', low: 'Low', medium: 'Medium', high: 'High', max: 'Max',
		lru: 'Per-layer LRU', hybrid: 'Hybrid', buffered: 'Buffered', direct: 'Direct',
		mmap: 'Memory mapping', resident: 'Resident', locked: 'Locked', temporal: 'Temporal'
	};
	const modeLabels: Record<string, string> = {
		auto: 'Automatic (recommended)', os: 'Standard (memory mapping)',
		explicit: 'Expert streaming from SSD', off: 'Fully resident in memory'
	};

	const cacheTypes = ['f16', 'bf16', 'q8_0', 'q5_1', 'q5_0', 'q4_1', 'q4_0', 'iq4_nl'];
	const reasoning: Field = {
		key: 'reasoning',
		label: 'Reasoning effort',
		options: ['default', 'off', 'low', 'medium', 'high', 'max'],
		help: 'Applies to supported reasoning models when the model is loaded.'
	};
	const groups: { title: string; description: string; fields: Field[] }[] = [
		{
			title: 'Memory and expert loading',
			description: 'Control how model weights move between storage and memory.',
			fields: [
				{ key: 'slots', label: 'Expert slots per layer (0 = automatic)', min: 0 },
				{ key: 'cache_policy', label: 'Cache policy', options: ['lru', 'hybrid'] },
				{ key: 'cache_ram_mib', label: 'Resident cache reserve (MiB, -1 = automatic)', min: -1 },
				{
					key: 'dense_residency',
					label: 'Dense and shared weights',
					options: ['auto', 'mmap', 'resident', 'locked']
				},
				{ key: 'paged_layers', label: 'First layers to page (0 = all)', min: 0, max: 4096 },
				{ key: 'ple_cache_mib', label: 'PLE page cache (MiB)', min: 1, max: 4096 },
				{ key: 'cpu_repack', label: 'CPU weight repacking', options: ['auto', 'on', 'off'] },
				{ key: 'gpu_expert_cache_mib', label: 'Expert VRAM cache (MiB)', min: 0, max: 262144 },
				{ key: 'gpu_layers', label: 'GPU layers (-1 = automatic, 0 = CPU)', min: -1, max: 4096 }
			]
		},
		{
			title: 'Context and batching',
			description: 'Larger context and batches can increase memory use.',
			fields: [
				{ key: 'context_shift', label: 'Allow context shifting', type: 'boolean' },
				{ key: 'ctx_checkpoints', label: 'Context checkpoints', min: 0, max: 128 },
				{ key: 'cache_type_k', label: 'Key cache type', options: cacheTypes },
				{ key: 'cache_type_v', label: 'Value cache type', options: cacheTypes },
				{ key: 'batch_size', label: 'Batch size', min: 1, max: 16384 },
				{ key: 'ubatch_size', label: 'Micro-batch size', min: 1, max: 16384 },
				{ key: 'parallel', label: 'Parallel request slots', min: 1, max: 32 }
			]
		},
		{
			title: 'CPU and storage',
			description: 'Tune workers, storage reads and overlap for this computer.',
			fields: [
				{
					key: 'inference_threads',
					label: 'Inference CPU threads (0 = automatic)',
					min: 0,
					max: 128
				},
				{ key: 'io_threads', label: 'SSD read workers', min: 1, max: 128 },
				{ key: 'io_mode', label: 'Storage I/O', options: ['buffered', 'direct'] },
				{ key: 'overlap', label: 'Compute and I/O overlap', options: ['off', 'on'] },
				{ key: 'prefetch', label: 'Expert prefetch', options: ['off', 'temporal'] }
			]
		},
		{
			title: 'Speculative decoding',
			description:
				'Requires a compatible model and draft weights. Unsupported combinations are rejected on load.',
			fields: [
				{ key: 'mtp_model', label: 'Draft model path (empty = discover companion)', type: 'text' },
				{ key: 'mtp_draft_tokens', label: 'Maximum draft tokens', min: 1, max: 64 },
				{ key: 'mtp_min_tokens', label: 'Minimum draft tokens', min: 0, max: 64 },
				{ key: 'mtp_p_min', label: 'Minimum draft probability', min: 0, max: 1, step: 0.01 },
				{ key: 'mtp_cache_mib', label: 'Draft cache (MiB)', min: 1, max: 65536 },
				{ key: 'mtp_threads', label: 'Draft CPU threads (0 = automatic)', min: 0, max: 128 },
				{ key: 'mtp_backend_sampling', label: 'Backend draft sampling', type: 'boolean' }
			]
		}
	];

	$effect(() => {
		void SparkMoeService.engines().then((status) => { engines = status.installed; }).catch(() => {});
	});
	$effect(() => {
		const id = model;
		loadError = '';
		void untrack(() => sparkMoeProfileStore.load(id, true)).catch((cause) => {
			if (model === id) loadError = String(cause.message ?? cause);
		});
	});

	function change(key: keyof SparkMoeModelProfile, value: unknown) {
		sparkMoeProfileStore.change(model, key, value);
		if (key === 'ctx_size') sparkMoeProfileStore.change(model, 'context_mode', 'manual');
		if (key === 'cache') sparkMoeProfileStore.change(model, 'memory_mode', 'manual');
	}

	async function run(name: string, operation: (id: string) => Promise<void>) {
		const id = model;
		action = name;
		actionMessage = '';
		try {
			await operation(id);
		} catch (cause) {
			actionMessage = cause instanceof Error ? cause.message : String(cause);
		} finally {
			action = '';
		}
	}

	async function apply(id: string) {
		if (!(await sparkMoeProfileStore.save(id))) return;
		await SparkMoeService.applyModel(id);
		await sparkMoeProfileStore.reload(id);
		await onApplied?.();
	}
</script>

{#snippet fieldControl(field: Field)}
	{#if draft}
		<label class="grid content-start gap-2 text-sm">
			<span class="font-medium">{t(field.label)}</span>
			{#if field.options}
				<select
					class="w-full rounded-md border border-input bg-background px-3 py-2"
					onchange={(event) => change(field.key, event.currentTarget.value)}
					value={String(draft.value[field.key])}
				>
					{#each field.options as option}<option value={option}
							>{t((field.key === 'mode' ? modeLabels[option] : optionLabels[option]) ?? option)}</option
						>{/each}
				</select>
			{:else if field.type === 'boolean'}
				<input
					checked={Boolean(draft.value[field.key])}
					class="h-4 w-4 accent-primary"
					onchange={(event) => change(field.key, event.currentTarget.checked)}
					type="checkbox"
				/>
			{:else}
				<input
					class="w-full rounded-md border border-input bg-background px-3 py-2"
					max={field.max}
					min={field.min}
					oninput={(event) =>
						change(
							field.key,
							field.type === 'text'
								? event.currentTarget.value
								: event.currentTarget.value === ''
									? undefined
									: Number(event.currentTarget.value)
						)}
					step={field.step ?? 1}
					type={field.type === 'text' ? 'text' : 'number'}
					value={draft.value[field.key] as string | number}
				/>
			{/if}
			{#if field.help}<span class="text-xs text-muted-foreground">{t(field.help)}</span>{/if}
		</label>
	{/if}
{/snippet}

<Card.Root>
	<Card.Header
		><Card.Title>{t("Model settings")}</Card.Title><Card.Description
			>{t("Changes save automatically for this model. Apply them when you are ready to load or reload it.")}</Card.Description
		></Card.Header
	>
	<Card.Content class="space-y-5">
		{#if loadError}<p class="text-sm text-destructive" role="alert">{loadError}</p>
			<Button
				onclick={() => {
					loadError = '';
					void sparkMoeProfileStore.load(model).catch((cause) => (loadError = String(cause)));
				}}
				variant="outline">{t("Retry")}</Button
			>
		{:else if !draft}<p class="flex items-center gap-2 text-sm text-muted-foreground">
				<LoaderCircle class="h-4 w-4 animate-spin" />{t("Loading model settings...")}
			</p>
		{:else}
			<div
				aria-live="polite"
				class="flex flex-wrap items-center justify-between gap-3 rounded-lg bg-muted/40 p-3 text-sm"
			>
				<span class={draft.error ? 'text-destructive' : 'text-muted-foreground'}
					>{#if draft.saving}<LoaderCircle
							class="mr-1 inline h-4 w-4 animate-spin"
						/>{t('Saving...')}{:else if draft.error}{t('Not saved')} · {draft.error}{:else if dirty}{t('Unsaved changes')}{:else}<Check
							class="mr-1 inline h-4 w-4"
						/>{t('Saved')}{/if}</span
				>
				<label class="flex flex-wrap items-center justify-between gap-2 text-sm">
				<span>{t('Engine')}</span>
				<select class="min-w-0 max-w-full rounded-md border bg-background ps-2 py-1.5"
					value={draft.value.worker_engine ?? ''}
					onchange={(event) => change('worker_engine', event.currentTarget.value)}>
					<option value="">{t('Application default')}</option>
					{#each engines as engine (engine.id)}
						<option value={engine.id}>{engine.backend} · {engine.id.slice(engine.backend.length + 1, engine.backend.length + 9)}</option>
					{/each}
				</select>
			</label>
			{#if draft.error || dirty}<Button
						disabled={draft.saving}
						onclick={() => void run('discard', (id) => sparkMoeProfileStore.reload(id))}
						size="sm"
						variant="ghost">{t("Discard draft")}</Button
					>{/if}
			</div>
			<fieldset class="space-y-5" disabled={action !== ''}>
				<div class="grid gap-4 sm:grid-cols-2">
					<div class="sm:col-span-2">{@render fieldControl(runMode)}</div>
					{@render fieldControl({
						key: 'context_mode',
						label: 'Context length',
						options: ['auto', 'manual'],
						help:
							draft.value.context_mode === 'auto'
								? 'Recommended: ' +
									Number(draft.recommendation.ctx_size ?? draft.value.ctx_size).toLocaleString() +
									' tokens'
								: 'Choose the token limit below.'
					})}
					{@render fieldControl({
						key: 'memory_mode',
						label: 'Memory budget',
						options: ['auto', 'manual'],
						help:
							draft.value.memory_mode === 'auto'
								? 'Recommended expert cache: ' +
									Number(draft.recommendation.cache_mib ?? draft.value.cache).toLocaleString() +
									' MiB'
								: 'Choose the expert cache below.'
					})}
					{#if draft.value.context_mode === 'manual'}{@render fieldControl({
							key: 'ctx_size',
							label: 'Context size (tokens)',
							min: 0,
							max: 1048576
						})}{/if}
					{#if draft.value.memory_mode === 'manual'}{@render fieldControl({
							key: 'cache',
							label: 'Expert RAM cache (MiB)',
							min: 0
						})}{/if}
					{#if supportsReasoning}{@render fieldControl(reasoning)}{/if}
				</div>
				{#if draft.recommendation.reason}<p class="text-xs text-muted-foreground">
						{t(String(draft.recommendation.reason))}
					</p>{/if}
				{#each groups as group}
					<details class="rounded-lg border border-border/60 p-4">
						<summary class="cursor-pointer text-sm font-medium">{t(group.title)}</summary>
						<p class="mt-2 text-xs text-muted-foreground">{t(group.description)}</p>
						{#if group.title === 'Speculative decoding'}
                        <fieldset class="grid grid-cols-2 gap-2 rounded-lg border p-3">
                            <legend class="px-1 text-sm">{t("Methods")}</legend>
                            {#each speculativeMethods as method (method)}
                                <label class="flex items-center gap-2 text-xs">
                                    <input type="checkbox" checked={speculation.includes(method)}
                                        onchange={(event) => toggleSpeculation(method, event.currentTarget.checked)} />
                                    <span dir="ltr">{method}</span>
                                </label>
                            {/each}
                        </fieldset>
                        {#each ngramMethods.filter((method) => speculation.includes(method)) as method (method)}
                            <fieldset class="mt-3 grid gap-3 rounded-lg border p-3 sm:grid-cols-3">
                                <legend class="px-1 text-xs" dir="ltr">{method}</legend>
                                {#each method === 'ngram-mod' ? ngramModFields : ngramMapFields as parameter (parameter.key)}
                                    {@const key = method + '-' + parameter.key}
                                    <label class="grid gap-1 text-xs">
                                        {t(parameter.label)}
                                        <input class="w-full rounded-md border border-input bg-background px-2 py-1"
                                            type="number" min={parameter.min} max={parameter.max}
                                            value={draft.value.ngram_options?.[key] ?? parameter.initial}
                                            onchange={(event) => {
                                                if (event.currentTarget.validity.valid) changeNgram(key, event.currentTarget.valueAsNumber);
                                            }} />
                                    </label>
                                {/each}
                            </fieldset>
                        {/each}
                    {/if}
                    {#if group.title === 'Speculative decoding' && speculation.includes('draft-mtp') && supportsMtp !== true}<p
								class="mt-2 text-xs text-amber-600 dark:text-amber-400"
							>
								{supportsMtp === false
									? t('This model does not report MTP support.')
									: t('MTP capability has not been confirmed for this model.')}
							</p>{/if}
						<div class="mt-4 grid gap-4 sm:grid-cols-2">
							{#if group.title !== 'Speculative decoding' || speculation.some((method) => method.startsWith('draft-'))}
                                {#each group.fields as field}{@render fieldControl(field)}{/each}
                            {/if}
						</div>
						{#if group.title === 'Memory and expert loading'}<Button
								class="mt-4"
								disabled={!loaded ||
									draft.value.cache_policy !== 'hybrid' ||
									dirty ||
									draft.saving ||
									Boolean(draft.error)}
								onclick={() =>
									void run('cache', async (id) => {
										await SparkMoeService.resizeCache(id, draft!.value.cache);
										actionMessage = t('Cache resize accepted; it applies before the next decode.');
									})}
								size="sm"
								variant="outline">{t("Apply hybrid cache now")}</Button
							>{/if}
					</details>
				{/each}
				{#if !supportsReasoning}<details class="rounded-lg border border-border/60 p-4">
						<summary class="cursor-pointer text-sm font-medium">{t("Reasoning compatibility")}</summary>
						<p class="my-3 text-xs text-muted-foreground">
							{t("Reasoning support has not been confirmed. Keep the model default unless its template supports these controls.")}
						</p>
						{@render fieldControl(reasoning)}
					</details>{/if}
			</fieldset>
			<div class="rounded-lg border border-border/60 p-3 text-sm">
				<p class="font-medium">
					{draft.pending
						? t('Saved changes are waiting to be applied')
						: loaded
							? t('Settings match the loaded model')
							: t('Settings will be used on the next load')}
				</p>
				{#if draft.applied}<p class="mt-1 text-xs text-muted-foreground">
						{t('Applied settings')} · {t(modeLabels[draft.applied.mode] ?? draft.applied.mode)} · {draft.applied.ctx_size.toLocaleString()} {t('tokens')} · <bdi>{draft.applied.cache.toLocaleString()} MiB</bdi>
						MiB.
					</p>{/if}
			</div>
			{#if actionMessage}<p class="text-sm" role="status">{actionMessage}</p>{/if}
		{/if}
	</Card.Content>
	{#if draft}<Card.Footer class="flex-wrap justify-between gap-2">
			<Button
				disabled={!draft.isOverride || draft.saving || action !== ''}
				onclick={() => void run('reset', (id) => sparkMoeProfileStore.reset(id))}
				size="sm"
				variant="ghost"><RotateCcw />{t("Use defaults")}</Button
			>
			<div class="flex gap-2">
				{#if draft.error}<Button
						disabled={draft.saving}
						onclick={() => void sparkMoeProfileStore.save(model)}
						variant="outline">{t("Retry saving")}</Button
					>{/if}<Button
					disabled={!available || draft.saving || action !== '' || Boolean(draft.error)}
					onclick={() => void run('apply', apply)}
					><RefreshCw class={action === 'apply' ? 'animate-spin' : ''} />{loaded
						? t('Apply and reload model')
						: t('Apply and load model')}</Button
				>
			</div>
		</Card.Footer>{/if}
</Card.Root>
