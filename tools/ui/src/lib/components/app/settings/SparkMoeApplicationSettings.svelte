<!-- SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 -->
<!-- Required Notice: Copyright (c) 2026 SparkMoE contributors. -->
<script lang="ts" module>
	let draftCache: SparkMoeApplication | null = null;
	let savedCache: SparkMoeApplication | null = null;
</script>

<script lang="ts">
	import { appLocale, t, type AppLocale } from '$lib/i18n/locale.svelte';
	import { LoaderCircle, RefreshCw } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import {
		SparkMoeService,
		type SparkMoeApplication,
		type SparkMoeLibraryModel,
		type SparkMoeRouterConfig
	} from '$lib/services/sparkmoe.service';
	import { onMount } from 'svelte';
	async function changeLanguage(language: AppLocale) {
		appLocale.select(language);
		await SparkMoeService.saveApplication({ locale: language }).catch(() => {});
	}

	let config = $state<SparkMoeApplication | null>(draftCache);
	let saved = $state<SparkMoeApplication | null>(savedCache);
	let models = $state<SparkMoeLibraryModel[]>([]);
	let router = $state<SparkMoeRouterConfig | null>(null);
	let savedRouter = $state('');
	let saving = $state(false);
	let available = $state(false);
	let error = $state('');
	let message = $state('');
	let dirty = $derived(config !== null && JSON.stringify(config) !== JSON.stringify(saved));
	let routerPending = $state(false);
	let routerTimer: ReturnType<typeof setTimeout> | undefined;
	let routerDirty = $derived(router !== null && JSON.stringify(router) !== savedRouter);
	async function load() {
		try {
			const [application, library, server] = await Promise.all([
				SparkMoeService.application(),
				SparkMoeService.library(),
				SparkMoeService.routerConfig()
			]);
			const next = {
				background_mode: application.background_mode,
				launch_at_login: application.launch_at_login,
				default_model: application.default_model
			};
			if (!dirty) config = next;
			saved = next;
			savedCache = next;
			models = library.models;
			router = server;
			savedRouter = JSON.stringify(server);
			routerPending = Boolean((await SparkMoeService.status()).pending);
			available = true;
			error = '';
		} catch {
			available = false;
		}
	}
	async function save() {
		if (!config || !saved || saving) return;
		const snapshot = { ...config };
		draftCache = snapshot;
		const changes = Object.fromEntries(
			Object.entries(snapshot).filter(
				([key, value]) => value !== saved![key as keyof SparkMoeApplication]
			)
		);
		if (!Object.keys(changes).length) return;
		saving = true;
		error = '';
		message = '';
		try {
			await SparkMoeService.saveApplication(changes);
			saved = snapshot;
			savedCache = snapshot;
			message = 'Saved';
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
		} finally {
			saving = false;
		}
	}
	function changeRouter(key: keyof SparkMoeRouterConfig, value: unknown) {
		if (!router) return;
		router = { ...router, [key]: value };
		clearTimeout(routerTimer);
		routerTimer = setTimeout(() => void saveServer(), 500);
	}
	async function saveServer() {
		if (!router || saving) return;
		if (
			!Number.isInteger(router.port) ||
			router.port < 1024 ||
			router.port > 65535 ||
			!Number.isInteger(router.models_max) ||
			router.models_max < 0 ||
			router.models_max > 64
		) {
			error = 'Enter a valid internal port and model limit.';
			return;
		}
		const snapshot = { ...router };
		saving = true;
		error = '';
		try {
			const result = await SparkMoeService.saveRouterConfig(snapshot);
			savedRouter = JSON.stringify(snapshot);
			routerPending = Boolean(result.status.pending);
			message = 'Server settings saved. Restart the server to apply them.';
		} catch (cause) {
			error = cause instanceof Error ? cause.message : String(cause);
		} finally {
			saving = false;
		}
	}
	onMount(() => {
		void load();
	});
</script>

{#if available && config}
	<section class="space-y-4 rounded-lg border border-border/60 p-4">
		<div>
			<h4 class="text-sm font-semibold">{t("Spark Edge application")}</h4>
            <label class="flex items-center justify-between gap-3 text-sm">
                <span>{t('Application language')}</span>
                <select class="rounded-md border bg-background px-3 py-1.5" value={appLocale.language}
                    onchange={(event) => void changeLanguage(event.currentTarget.value as AppLocale)}>
                    <option value="he">עברית</option>
                    <option value="en">English</option>
                </select>
            </label>

			<p class="mt-1 text-xs text-muted-foreground">
				Saved on this computer. Model settings are managed in the model library.
			</p>
		</div>
		<fieldset class="space-y-4" disabled={saving}>
			<label class="flex items-start justify-between gap-4 text-sm"
				><span
					>{t("Keep Spark Edge in the background")}<span class="mt-1 block text-xs text-muted-foreground"
						>Closing the window keeps the local server available. Exit from the tray to stop it.</span
					></span
				><input
					class="mt-1"
					checked={config.background_mode}
					onchange={(event) => {
						config!.background_mode = event.currentTarget.checked;
						void save();
					}}
					type="checkbox"
				/></label
			>
			<label class="flex items-center justify-between gap-4 text-sm"
				><span>{t("Launch Spark Edge when I sign in to Windows")}</span><input
					checked={config.launch_at_login}
					onchange={(event) => {
						config!.launch_at_login = event.currentTarget.checked;
						void save();
					}}
					type="checkbox"
				/></label
			>
			<label class="grid gap-2 text-sm"
				><span>{t("Default model")}</span><select
					class="rounded-md border border-input bg-background px-3 py-2"
					onchange={(event) => {
						config!.default_model = event.currentTarget.value;
						void save();
					}}
					value={config.default_model}
					><option value="">Choose when starting a chat</option>{#each models as model}<option
							value={model.id}
							>{model.display_name}{!model.available ? ' (file missing)' : ''}</option
						>{/each}</select
				></label
			>
		</fieldset>
		<div
			aria-live="polite"
			class={error ? 'text-sm text-destructive' : 'text-xs text-muted-foreground'}
		>
			{#if saving}<LoaderCircle
					class="mr-1 inline h-3 w-3 animate-spin"
				/>Saving...{:else if error}Not saved: {error}
				<Button
					onclick={() => {
						if (routerDirty) void saveServer();
						else void save();
					}}
					size="sm"
					variant="outline">{t("Retry")}</Button
				>{:else}{dirty ? 'Unsaved changes' : message || 'Saved'}{/if}
		</div>
		{#if router}<details class="border-t border-border/50 pt-4">
				<summary class="cursor-pointer text-sm font-medium">Advanced server settings</summary>
				<div class="mt-4 space-y-4">
					<label class="grid gap-2 text-sm"
						><span>Internal server port</span><input
							class="rounded-md border border-input bg-background p-2"
							max="65535"
							min="1024"
							disabled={saving}
							oninput={(event) =>
								changeRouter(
									'port',
									event.currentTarget.value === '' ? undefined : Number(event.currentTarget.value)
								)}
							type="number"
							value={router.port}
						/></label
					><label class="grid gap-2 text-sm"
						><span>Maximum loaded models (0 = unlimited)</span><input
							class="rounded-md border border-input bg-background p-2"
							max="64"
							min="0"
							disabled={saving}
							oninput={(event) =>
								changeRouter(
									'models_max',
									event.currentTarget.value === '' ? undefined : Number(event.currentTarget.value)
								)}
							type="number"
							value={router.models_max}
						/></label
					><label class="flex items-center gap-3 text-sm"
						><input
							checked={router.models_autoload}
							disabled={saving}
							onchange={(event) => changeRouter('models_autoload', event.currentTarget.checked)}
							type="checkbox"
						/>Load models automatically for API requests</label
					>
					<p class="text-xs text-muted-foreground">
						The Spark Edge application address stays the same. Restarting stops loaded models and
						active requests.
					</p>
					<div class="flex flex-wrap gap-2">
						{#if routerDirty}<span class="text-xs text-muted-foreground"
								>Unsaved server settings</span
							>{/if}<Button
							disabled={saving || routerDirty || !routerPending}
							onclick={() => {
								saving = true;
								void SparkMoeService.restart()
									.then(() => {
										message = 'Server restart requested.';
										routerPending = false;
									})
									.catch((cause) => (error = String(cause)))
									.finally(() => (saving = false));
							}}
							size="sm"
							variant="outline"><RefreshCw />Apply and restart server</Button
						>
					</div>
				</div>
			</details>{/if}
	</section>
{/if}
