import { CONFIG_LOCALSTORAGE_KEY } from '$lib/constants';
import { serverStore } from '$lib/stores/server.svelte';
import { settingsStore } from '$lib/stores/settings/index.svelte';
import { SettingsService } from '$lib/services/settings.service';
import {
	SparkMoeService,
	type SparkMoeModelProfile,
	type SparkMoeProfileResponse
} from '$lib/services/sparkmoe.service';
import { SparkMoeProfileStore } from '$lib/stores/sparkmoe-profile.svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

function mockProps(uiSettings: Record<string, string | number | boolean>) {
	Object.defineProperty(serverStore, 'props', {
		configurable: true,
		get: () =>
			({
				default_generation_settings: { params: { temperature: 0.8 } },
				ui_settings: uiSettings
			}) as unknown as typeof serverStore.props
	});
}

describe('server ui_settings application semantics', () => {
	beforeEach(() => {
		localStorage.removeItem(CONFIG_LOCALSTORAGE_KEY);
	});

	it('applies the admin defaults once for a new user', () => {
		settingsStore.initialize();
		mockProps({ apiKey: '', theme: 'dark' });

		settingsStore.syncWithServerDefaults();

		expect(settingsStore.config.theme).toBe('dark');
	});

	it('never reapplies on later loads: the user config diverges freely', () => {
		settingsStore.initialize();
		settingsStore.updateConfig('theme', 'light');
		settingsStore.updateConfig('apiKey', 'sk-user-key');

		// simulated F5: config now exists in localStorage
		settingsStore.initialize();
		mockProps({ apiKey: '', theme: 'dark' });

		settingsStore.syncWithServerDefaults();
		settingsStore.syncWithServerDefaults();

		expect(settingsStore.config.theme).toBe('light');
		expect(settingsStore.config.apiKey).toBe('sk-user-key');
		const stored = JSON.parse(localStorage.getItem(CONFIG_LOCALSTORAGE_KEY) ?? '{}');

		expect(stored.apiKey).toBe('sk-user-key');
	});

	it('keeps a value the user sets before the baseline is reachable', () => {
		settingsStore.initialize();
		// the splash is the only way in when the server runs with --api-key,
		// so the first user write lands before the first successful /props
		settingsStore.updateConfig('apiKey', 'sk-user-key');
		mockProps({ apiKey: 'admin-placeholder', theme: 'dark' });

		settingsStore.syncWithServerDefaults();

		expect(settingsStore.config.apiKey).toBe('sk-user-key');
		expect(settingsStore.config.theme).toBe('dark');
	});

	it('Reset to Default reapplies the full baseline, api key included', () => {
		settingsStore.initialize();
		settingsStore.updateConfig('theme', 'light');
		settingsStore.updateConfig('apiKey', 'sk-user-key');
		mockProps({ apiKey: '', theme: 'dark' });

		settingsStore.forceSyncWithServerDefaults();

		expect(settingsStore.config.theme).toBe('dark');
		expect(settingsStore.config.apiKey).toBe('');
	});
});

describe('syncable scope (spec section 4)', () => {
	it('sampling params keep their live server twin, ui settings carry none', async () => {
		const { ParameterSyncService } = await import('$lib/services/parameter-sync.service');

		expect(ParameterSyncService.canSyncParameter('temperature')).toBe(true);
		expect(ParameterSyncService.canSyncParameter('samplers')).toBe(true);
		expect(ParameterSyncService.canSyncParameter('theme')).toBe(false);
		expect(ParameterSyncService.canSyncParameter('systemMessage')).toBe(false);
		expect(ParameterSyncService.canSyncParameter('apiKey')).toBe(false);
		expect(ParameterSyncService.canSyncParameter('customCss')).toBe(false);
	});
});

describe('autosave isolation and failures', () => {
	beforeEach(() => {
		for (const model of ['first', 'second'])
			localStorage.removeItem('sparkmoe.model-draft.v1.' + model);
	});
	afterEach(() => {
		vi.clearAllTimers();
		vi.useRealTimers();
		vi.restoreAllMocks();
	});

	function profile(): SparkMoeModelProfile {
		return {
			context_mode: 'manual',
			memory_mode: 'manual',
			gpu_layers: 0,
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
			reasoning: 'default',
			slots: 0,
			ubatch_size: 128
		};
	}
	function response(model: string, value = profile(), revision = 1): SparkMoeProfileResponse {
		return { model, profile: value, revision, is_override: true, pending: true };
	}

	it('keeps newer input and the second model draft when an older save completes', async () => {
		vi.useFakeTimers();
		vi.spyOn(SparkMoeService, 'modelProfile').mockImplementation(async (model) => response(model));
		let finish!: (value: SparkMoeProfileResponse) => void;
		const request = new Promise<SparkMoeProfileResponse>((resolve) => {
			finish = resolve;
		});
		const save = vi.spyOn(SparkMoeService, 'saveModelProfile').mockReturnValue(request);
		const store = new SparkMoeProfileStore();
		const first = await store.load('first');
		const second = await store.load('second');
		store.change('first', 'cache', 4096);
		const pending = store.save('first');
		store.change('first', 'cache', 8192);
		store.change('second', 'ctx_size', 4096);
		finish(response('first', { ...profile(), cache: 4096 }, 2));
		await pending;
		expect(save).toHaveBeenCalledWith('first', { cache: 4096 }, 1);
		expect(first.value.cache).toBe(8192);
		expect(first.saved.cache).toBe(4096);
		expect(second.value.ctx_size).toBe(4096);
		expect(second.saved.ctx_size).toBe(8192);
	});

	it('keeps invalid model input local and does not send a request', async () => {
		vi.useFakeTimers();
		vi.spyOn(SparkMoeService, 'modelProfile').mockResolvedValue(response('first'));
		const save = vi.spyOn(SparkMoeService, 'saveModelProfile');
		const store = new SparkMoeProfileStore();
		const draft = await store.load('first');
		store.change('first', 'ubatch_size', 2048);
		expect(await store.save('first')).toBe(false);
		expect(save).not.toHaveBeenCalled();
		expect(draft.value.ubatch_size).toBe(2048);
		expect(draft.saved.ubatch_size).toBe(128);
	});

	it('keeps the draft and revision on a conflicting save', async () => {
		vi.useFakeTimers();
		vi.spyOn(SparkMoeService, 'modelProfile').mockResolvedValue(response('first'));
		vi.spyOn(SparkMoeService, 'saveModelProfile').mockRejectedValue(
			new Error('Settings changed in another window.')
		);
		const store = new SparkMoeProfileStore();
		const draft = await store.load('first');
		store.change('first', 'cache', 4096);
		expect(await store.save('first')).toBe(false);
		expect(draft.revision).toBe(1);
		expect(draft.value.cache).toBe(4096);
		expect(draft.error).toContain('another window');
		expect(draft.saving).toBe(false);
	});

	it('restores a draft after window closure without accepting a newer server revision', async () => {
		vi.useFakeTimers();
		const fetch = vi.spyOn(SparkMoeService, 'modelProfile').mockResolvedValue(response('first'));
		const firstWindow = new SparkMoeProfileStore();
		await firstWindow.load('first');
		firstWindow.change('first', 'cache', 4096);
		fetch.mockResolvedValue(response('first', { ...profile(), cache: 3072 }, 2));
		const reopened = await new SparkMoeProfileStore().load('first');
		expect(reopened.value.cache).toBe(4096);
		expect(reopened.saved.cache).toBe(2048);
		expect(reopened.revision).toBe(1);
		expect(reopened.error).toContain('changed');
	});

	it('does not mark browser preferences saved when persistence fails', () => {
		const previous = { ...settingsStore.config };
		vi.spyOn(SettingsService, 'saveConfig').mockReturnValue(false);
		expect(
			settingsStore.updateMultipleConfig({ theme: previous.theme === 'dark' ? 'light' : 'dark' })
		).toBe(false);
		expect(settingsStore.config).toEqual(previous);
	});
});
