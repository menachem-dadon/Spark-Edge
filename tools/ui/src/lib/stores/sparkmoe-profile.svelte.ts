// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

import {
	SparkMoeRequestError,
	SparkMoeService,
	type SparkMoeModelProfile,
	type SparkMoeProfileResponse
} from '$lib/services/sparkmoe.service';
import { SvelteMap } from 'svelte/reactivity';
import { browser } from '$app/environment';

export type ModelSettingsDraft = {
	model: string;
	value: SparkMoeModelProfile;
	saved: SparkMoeModelProfile;
	revision: number;
	pending: boolean;
	isOverride: boolean;
	applied: SparkMoeModelProfile | null;
	recommendation: Record<string, unknown>;
	saving: boolean;
	error: string;
};

const limits: Partial<Record<keyof SparkMoeModelProfile, [number, number]>> = {
	batch_size: [1, 16384],
	cache: [0, 2147483647],
	cache_ram_mib: [-1, 1048576],
	ctx_checkpoints: [0, 128],
	ctx_size: [0, 1048576],
	inference_threads: [0, 128],
	io_threads: [1, 128],
	mtp_cache_mib: [1, 65536],
	mtp_draft_tokens: [1, 64],
	mtp_min_tokens: [0, 64],
	mtp_p_min: [0, 1],
	mtp_threads: [0, 128],
	paged_layers: [0, 4096],
	parallel: [1, 32],
	ple_cache_mib: [1, 4096],
	slots: [0, 2147483647],
	ubatch_size: [1, 16384],
	gpu_layers: [-1, 4096]
};

export function validateModelSettings(profile: SparkMoeModelProfile): string {
	for (const [key, range] of Object.entries(limits)) {
		const value = profile[key as keyof SparkMoeModelProfile];
		if (
			typeof value !== 'number' ||
			!Number.isFinite(value) ||
			value < range[0] ||
			value > range[1] ||
			(key !== 'mtp_p_min' && !Number.isInteger(value))
		)
			return `${key.replaceAll('_', ' ')} must be ${key === 'mtp_p_min' ? 'a number' : 'a whole number'} from ${range[0]} to ${range[1]}.`;
	}
	if (profile.ubatch_size > profile.batch_size) return 'Micro-batch size cannot exceed batch size.';
	if (profile.mtp_min_tokens > profile.mtp_draft_tokens)
		return 'Minimum draft tokens cannot exceed maximum draft tokens.';
	if (['auto', 'explicit'].includes(profile.mode) && profile.parallel !== 1)
		return 'Expert paging requires one parallel request slot.';
	if (['auto', 'explicit'].includes(profile.mode) && profile.cache === 0 && profile.slots === 0)
		return 'Expert paging needs an expert cache budget or slot count.';
	return '';
}

export class SparkMoeProfileStore {
	drafts = new SvelteMap<string, ModelSettingsDraft>();
	private timers = new Map<string, ReturnType<typeof setTimeout>>();
	private loads = new Map<string, Promise<ModelSettingsDraft>>();

	constructor() {
		if (browser)
			window.addEventListener('beforeunload', (event) => {
				if ([...this.drafts.values()].some((draft) => draft.saving || this.dirty(draft))) {
					event.preventDefault();
					event.returnValue = '';
				}
			});
	}

	dirty(draft: ModelSettingsDraft): boolean {
		return JSON.stringify(draft.value) !== JSON.stringify(draft.saved);
	}

	async load(model: string, refresh = false): Promise<ModelSettingsDraft> {
		const existing = this.drafts.get(model);
		if (existing) {
			if (refresh && !existing.saving && !this.dirty(existing)) {
				const response = await SparkMoeService.modelProfile(model);
				if (!existing.saving && !this.dirty(existing)) {
					this.accept(existing, response);
					existing.value = { ...response.profile };
				}
			}
			return existing;
		}
		const loading = this.loads.get(model);
		if (loading) return loading;
		const request = SparkMoeService.modelProfile(model)
			.then((response) => {
				const draft = $state<ModelSettingsDraft>({
					model,
					value: { ...response.profile },
					saved: { ...response.profile },
					revision: response.revision,
					pending: Boolean(response.pending),
					applied: response.applied_profile ?? null,
					isOverride: response.is_override,
					recommendation: response.recommendation ?? {},
					saving: false,
					error: ''
				});
				if (browser) {
					try {
						const stored = JSON.parse(
							localStorage.getItem(this.storageKey(model)) ?? 'null'
						) as Pick<ModelSettingsDraft, 'value' | 'saved' | 'revision'> | null;
						if (stored && stored.value && stored.saved && Number.isInteger(stored.revision)) {
							if (JSON.stringify(stored.value) !== JSON.stringify(response.profile)) {
								draft.value = stored.value;
								draft.saved = stored.saved;
								draft.revision = stored.revision;
								draft.error =
									stored.revision === response.revision
										? validateModelSettings(draft.value)
										: 'Saved settings changed while this draft was closed. Your draft is kept; discard it to use the newer settings.';
							} else localStorage.removeItem(this.storageKey(model));
						}
					} catch {
						draft.error = 'The local model draft could not be restored.';
					}
				}
				this.drafts.set(model, draft);
				return draft;
			})
			.finally(() => this.loads.delete(model));
		this.loads.set(model, request);
		return request;
	}

	change(model: string, key: keyof SparkMoeModelProfile, value: unknown): void {
		const draft = this.drafts.get(model);
		if (!draft) return;
		draft.value = { ...draft.value, [key]: value };
		draft.error = validateModelSettings(draft.value);
		this.persist(draft);
		clearTimeout(this.timers.get(model));
		if (!draft.error)
			this.timers.set(
				model,
				setTimeout(() => void this.save(model), 600)
			);
	}

	async save(model: string): Promise<boolean> {
		clearTimeout(this.timers.get(model));
		const draft = this.drafts.get(model);
		if (!draft || draft.saving) return false;
		if (!this.dirty(draft)) return !draft.error;
		draft.error = validateModelSettings(draft.value);
		if (draft.error) return false;
		const snapshot = { ...draft.value };
		const baseline = { ...draft.saved };
		const changes = Object.fromEntries(
			Object.entries(snapshot).filter(
				([key, value]) => value !== draft.saved[key as keyof SparkMoeModelProfile]
			)
		);
		draft.saving = true;
		try {
			let response: SparkMoeProfileResponse;
			try {
				response = await SparkMoeService.saveModelProfile(model, changes, draft.revision);
			} catch (cause) {
				if (!(cause instanceof SparkMoeRequestError) || cause.status !== 409) throw cause;
				const current = await SparkMoeService.modelProfile(model);
				if (JSON.stringify(current.profile) !== JSON.stringify(baseline)) throw cause;
				response = await SparkMoeService.saveModelProfile(model, changes, current.revision);
			}
			this.accept(draft, response, snapshot);
		} catch (cause) {
			draft.error = cause instanceof Error ? cause.message : String(cause);
		} finally {
			draft.saving = false;
			this.persist(draft);
			if (!draft.error && this.dirty(draft))
				this.timers.set(
					model,
					setTimeout(() => void this.save(model), 600)
				);
		}
		return !draft.error && !this.dirty(draft);
	}

	async reload(model: string): Promise<void> {
		const draft = this.drafts.get(model);
		if (draft?.saving) return;
		clearTimeout(this.timers.get(model));
		const response = await SparkMoeService.modelProfile(model);
		if (draft) {
			this.accept(draft, response);
			draft.value = { ...response.profile };
			draft.error = '';
			this.persist(draft);
		}
	}

	async reset(model: string): Promise<void> {
		const draft = this.drafts.get(model);
		if (!draft || draft.saving) return;
		clearTimeout(this.timers.get(model));
		const response = await SparkMoeService.resetModelProfile(model, draft.revision);
		this.accept(draft, response);
		draft.value = { ...response.profile };
		draft.error = '';
		this.persist(draft);
	}

	private storageKey(model: string): string {
		return 'sparkmoe.model-draft.v1.' + encodeURIComponent(model);
	}

	private persist(draft: ModelSettingsDraft): void {
		if (!browser) return;
		try {
			if (this.dirty(draft))
				localStorage.setItem(
					this.storageKey(draft.model),
					JSON.stringify(
						{ value: draft.value, saved: draft.saved, revision: draft.revision },
						(_key, value) => (value === undefined ? null : value)
					)
				);
			else localStorage.removeItem(this.storageKey(draft.model));
		} catch {
			if (this.dirty(draft))
				draft.error =
					'This draft cannot be preserved when the window closes. Keep SparkMoE open and retry saving.';
		}
	}

	private accept(
		draft: ModelSettingsDraft,
		response: SparkMoeProfileResponse,
		snapshot?: SparkMoeModelProfile
	): void {
		if (snapshot)
			for (const key of Object.keys(snapshot) as (keyof SparkMoeModelProfile)[]) {
				if (draft.value[key] === snapshot[key])
					draft.value = { ...draft.value, [key]: response.profile[key] };
			}
		draft.saved = { ...response.profile };
		draft.revision = response.revision;
		draft.pending = Boolean(response.pending);
		draft.applied = response.applied_profile ?? null;
		draft.isOverride = response.is_override;
		draft.recommendation = response.recommendation ?? {};
	}
}

export const sparkMoeProfileStore = new SparkMoeProfileStore();
