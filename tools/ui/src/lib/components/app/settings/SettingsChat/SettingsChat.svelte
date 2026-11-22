<script lang="ts" module>
	const pendingDraft: Partial<SettingsConfigType> = {};
</script>

<script lang="ts">
	import { t } from '$lib/i18n/locale.svelte';
	import { RefreshCw } from '@lucide/svelte';
	import { page } from '$app/state';
	import {
		SettingsChatDesktopSidebar,
		SettingsChatFields,
		SettingsChatImportExportTab,
		SettingsChatMobileHeader,
		SettingsChatToolsTab,
		SettingsFooter
	} from '$lib/components/app/settings';
	import SparkMoeApplicationSettings from '$lib/components/app/settings/SparkMoeApplicationSettings.svelte';
	import { Button } from '$lib/components/ui/button';
	import {
		NUMERIC_FIELDS,
		POSITIVE_INTEGER_FIELDS,
		SETTINGS_CHAT_SECTIONS,
		SETTINGS_SECTION_SLUGS
	} from '$lib/constants';
	import { ColorMode } from '$lib/enums/ui.enums';
	import { RouterService } from '$lib/services/router.service';
	import { modelsStore, serverStore, settingsStore } from '$lib/stores';
	import type { SettingsSection } from '$lib/types';
	import { setMode } from 'mode-watcher';
	import { onDestroy, untrack } from 'svelte';
	import { fade } from 'svelte/transition';
	interface Props {
		initialSection?: string;
		getSectionHref?: (section: SettingsSection) => string;
	}
	let { getSectionHref, initialSection }: Props = $props();
	let activeSlug = $derived(
		initialSection ?? (page.params as Record<string, string | undefined>).section ?? 'general'
	);
	let currentSection = $derived(
		SETTINGS_CHAT_SECTIONS.find((section) => section.slug === activeSlug) ||
			SETTINGS_CHAT_SECTIONS[0]
	);
	let localConfig: SettingsConfigType = $state({ ...settingsStore.config, ...pendingDraft });
	let dirtyKeys = $state<string[]>(Object.keys(pendingDraft));
	let saveError = $state('');
	let saveStatus = $state(Object.keys(pendingDraft).length ? 'Unsaved changes' : 'Saved');
	let mobileHeader: { updateCarousel: () => void } | undefined;
	let fetchInitiated = false;
	let saveTimer: ReturnType<typeof setTimeout> | undefined;
	let basicFields = $derived(currentSection.fields?.filter((field) => !field.isAdvanced) ?? []);
	let advancedFields = $derived(currentSection.fields?.filter((field) => field.isAdvanced) ?? []);

	$effect(() => {
		const source = { ...settingsStore.config };
		untrack(() => {
			for (const key of dirtyKeys) source[key] = localConfig[key];
			localConfig = source;
		});
	});
	$effect(() => {
		if (serverStore.isRouterMode && currentSection.fields?.length && !fetchInitiated) {
			fetchInitiated = true;
			void modelsStore
				.fetch()
				.then(() => modelsStore.fetchRouterModels())
				.then(() => modelsStore.props.fetchModalitiesForLoadedModels())
				.then(() => modelsStore.ensureFirstModelSelected());
		}
	});
	function handleThemeChange(theme: string) {
		setMode(theme as ColorMode);
		handleConfigChange('theme', theme);
	}
	function handleConfigChange(key: string, value: string | boolean) {
		localConfig[key] = value;
		pendingDraft[key] = value;
		if (!dirtyKeys.includes(key)) dirtyKeys = [...dirtyKeys, key];
		saveStatus = 'Unsaved changes';
		saveError = '';
		clearTimeout(saveTimer);
		saveTimer = setTimeout(handleSave, 450);
	}
	function handleReset() {
		clearTimeout(saveTimer);
		for (const key of Object.keys(pendingDraft)) delete pendingDraft[key];
		dirtyKeys = [];
		saveError = '';
		saveStatus = 'Saved';
		localConfig = { ...settingsStore.config };
		setMode(localConfig.theme as ColorMode);
		mobileHeader?.updateCarousel();
	}
	function handleSave() {
		clearTimeout(saveTimer);
		const updates: Partial<SettingsConfigType> = {};
		const errors: string[] = [];
		for (const key of dirtyKeys) {
			let value = localConfig[key];
			if (key === 'customJson' && typeof value === 'string' && value.trim()) {
				try {
					const parsed = JSON.parse(value);
					if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed))
						throw new Error();
				} catch {
					errors.push('Custom parameters must be a valid JSON object.');
					continue;
				}
			}
			if (
				(NUMERIC_FIELDS as readonly string[]).includes(key) &&
				value !== '' &&
				value !== undefined
			) {
				const numeric = Number(value);
				const field = SETTINGS_CHAT_SECTIONS.flatMap((section) => section.fields ?? []).find(
					(entry) => entry.key === key
				);
				if (
					!Number.isFinite(numeric) ||
					((POSITIVE_INTEGER_FIELDS as readonly string[]).includes(key) &&
						!Number.isInteger(numeric)) ||
					(field?.min !== undefined && numeric < field.min) ||
					(field?.max !== undefined && numeric > field.max)
				) {
					errors.push('Enter a valid value for ' + (field?.label ?? key) + '.');
					continue;
				}
				value = numeric;
			}
			updates[key] = value;
		}
		if (Object.keys(updates).length) {
			if (settingsStore.updateMultipleConfig(updates)) {
				for (const key of Object.keys(updates)) {
					localConfig[key] = updates[key];
					delete pendingDraft[key];
				}
				dirtyKeys = dirtyKeys.filter((key) => !(key in updates));
			} else errors.push('Browser storage is unavailable. Your draft has been kept; retry saving.');
		}
		saveError = errors.join(' ');
		saveStatus = dirtyKeys.length ? 'Unsaved changes' : 'Saved';
	}
	onDestroy(() => {
		if (dirtyKeys.length) handleSave();
	});
	export function reset() {
		handleReset();
	}
</script>

<div class="mx-auto flex h-full w-full flex-col md:pl-8" in:fade={{ duration: 150 }}>
	<div class="flex flex-1 flex-col gap-4 md:flex-row">
		<SettingsChatDesktopSidebar
			sections={SETTINGS_CHAT_SECTIONS}
			isActive={(section: SettingsSection) => section.slug === activeSlug}
			getHref={getSectionHref ??
				((section: SettingsSection) => RouterService.settings(section.slug))}
		/>

		<SettingsChatMobileHeader
			sections={SETTINGS_CHAT_SECTIONS}
			isActive={(section: SettingsSection) => section.slug === activeSlug}
			getHref={getSectionHref ??
				((section: SettingsSection) => RouterService.settings(section.slug))}
			bind:this={mobileHeader}
		/>

		<div class="mx-auto max-w-3xl flex-1">
			<div class="space-y-6 p-4 md:p-6 md:pt-28">
				<div class="grid">
					<div class="mb-6 flex items-center gap-2 border-b border-border/30 pb-6 md:flex">
						<currentSection.icon class="h-5 w-5" />
						<h3 class="text-lg font-semibold">{currentSection.title}</h3>
					</div>

					{#if currentSection.slug === SETTINGS_SECTION_SLUGS.TOOLS}
						<SettingsChatToolsTab />
					{:else if currentSection.slug === SETTINGS_SECTION_SLUGS.IMPORT_EXPORT}
						<SettingsChatImportExportTab />
					{:else if currentSection.fields}
						<div class="space-y-6">
							<SettingsChatFields
								fields={basicFields}
								{localConfig}
								onConfigChange={handleConfigChange}
								onThemeChange={handleThemeChange}
							/>

							{#if advancedFields.length}
								<details class="rounded-lg border border-border/60 p-4">
									<summary class="cursor-pointer text-sm font-medium">{t("Advanced settings")}</summary>
									<div class="mt-5 space-y-6">
										<SettingsChatFields
											fields={advancedFields}
											{localConfig}
											onConfigChange={handleConfigChange}
											onThemeChange={handleThemeChange}
										/>
									</div>
								</details>
							{/if}
							{#if currentSection.slug === SETTINGS_SECTION_SLUGS.GENERAL}
								<SparkMoeApplicationSettings />
								<div class="flex justify-end">
									<Button variant="outline" onclick={() => window.location.reload()}>
										<RefreshCw class="h-3 w-3" />
										Reload app
									</Button>
								</div>
							{/if}
						</div>
					{/if}
				</div>

				<div class="mt-8 border-t border-border/30 pt-6">
					<p class="text-xs text-muted-foreground">
						Chat preferences save automatically in this browser.
					</p>
				</div>
			</div>

			<div aria-live="polite" class="px-6 text-sm">
				{#if saveError}<p class="text-destructive">Not saved: {saveError}</p>
					<div class="mt-2 flex gap-2">
						<Button onclick={handleSave} size="sm" variant="outline">Retry saving</Button><Button
							onclick={handleReset}
							size="sm"
							variant="ghost">{t("Discard draft")}</Button
						>
					</div>{:else}<p class="text-muted-foreground">{saveStatus}</p>{/if}
			</div>
			<SettingsFooter onReset={handleReset} />
		</div>
	</div>
</div>
