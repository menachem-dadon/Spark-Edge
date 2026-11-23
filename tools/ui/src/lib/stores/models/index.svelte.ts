/**
 * modelsStore - Model management for MODEL and ROUTER modes
 *
 * Owns model lists, selection, favorites and load/unload state. Composes the
 * per-model props cache (modalities, thinking detection) as
 * {@link ModelsStore.props} and the /models/sse status feed as
 * {@link ModelsStore.status}; tracks which conversations use which models.
 */

import { FAVORITE_MODELS_LOCALSTORAGE_KEY } from '$lib/constants';
import { ServerModelStatus } from '$lib/enums';
import { ModelsService } from '$lib/services/models.service';
import { SparkMoeService } from '$lib/services/sparkmoe.service';
// direct imports between stores, not via the barrel, to avoid circular deps
import { type ModelPropsHost, ModelPropsManager } from '$lib/stores/models/props.svelte';
import { type ModelStatusHost, ModelStatusManager } from '$lib/stores/models/status.svelte';
import { serverStore } from '$lib/stores/server.svelte';
import { SvelteSet } from 'svelte/reactivity';
import { toast } from 'svelte-sonner';

class ModelsStore implements ModelPropsHost, ModelStatusHost {
	error = $state<string | null>(null);
	favoriteModelIds = $state<Set<string>>(this.loadFavoritesFromStorage());
	loading = $state(false);
	models = $state<ModelOption[]>([]);
	routerModels = $state<ApiModelDataEntry[]>([]);
	selectedModelId = $state<string | null>(null);
	selectedModelName = $state<string | null>(null);

	updating = $state(false);

	/** Per-model props cache, modalities and thinking detection, composed here. */
	private _props = new ModelPropsManager(this);

	/** Load/unload operations and the /models/sse status feed, composed here. */
	private _status = new ModelStatusManager(this);

	// Dedup concurrent fetch() callers — all awaiters share the same inflight promise.
	// Without this, ?model=<name> URL handler races an in-progress fetch and sees an empty list.
	private inflightFetch: Promise<void> | null = null;
	private favoriteUpdates = new Set<string>();

	/**
	 * Global selection in router mode, or the single served model.
	 */
	get activeModelId(): string | null {
		if (!serverStore.isRouterMode) {
			return this.models.length > 0 ? this.models[0].model : this.singleModelName;
		}

		if (this.selectedModelId) {
			const selected = this.models.find((m) => m.id === this.selectedModelId);

			if (selected) return selected.model;
		}

		return null;
	}

	get loadedModelIds(): string[] {
		return this.routerModels
			.filter(
				(m) =>
					m.status.value === ServerModelStatus.LOADED ||
					m.status.value === ServerModelStatus.SLEEPING
			)
			.map((m) => m.id);
	}

	get props() {
		return this._props;
	}

	get selectedModel(): ModelOption | null {
		if (!this.selectedModelId) return null;

		return this.models.find((m) => m.id === this.selectedModelId) ?? null;
	}

	get selectedModelContextSize(): number | null {
		if (!this.selectedModelName) return null;

		return this.props.getModelContextSize(this.selectedModelName);
	}

	/**
	 * Get model name in MODEL mode (single model).
	 * Extracts from model_path or model_alias from server props.
	 * In ROUTER mode, returns null (model is per-conversation).
	 */
	get singleModelName(): string | null {
		if (serverStore.isRouterMode) return null;

		const props = serverStore.props;

		if (props?.model_alias) return props.model_alias;

		if (!props?.model_path) return null;

		return props.model_path.split(/(\\|\/)/).pop() || null;
	}

	get status() {
		return this._status;
	}

	clearSelection(): void {
		this.selectedModelId = null;
		this.selectedModelName = null;
	}

	/**
	 * Auto-selects the first available model if none is selected.
	 * Prioritizes:
	 * Loaded model first, then the most recently loaded model or application default.
	 */
	async ensureFirstModelSelected(): Promise<void> {
		if (this.selectedModelName) return;

		const availableModels = this.getVisibleModels();

		if (availableModels.length === 0) return;

		const lastLoaded = this.routerModels.find((model) => model.sparkmoe?.last_loaded);
		const lastOption = availableModels.find((model) => model.id === lastLoaded?.id);
		const loadedModel = lastOption && this.isModelLoaded(lastOption.model)
			? lastOption
			: availableModels.find((model) => this.isModelLoaded(model.model));

		if (loadedModel) {
			await this.selectModelById(loadedModel.id);
			await this.props.fetchModelProps(loadedModel.model);

			return;
		}

		const preferred = this.routerModels.find((model) => model.sparkmoe?.is_default);
		const startup = lastOption ?? availableModels.find((model) => model.id === preferred?.id);
		if (startup) await this.selectModelById(startup.id);
		else if (!this.routerModels.some((model) => model.sparkmoe) || availableModels.length === 1) {
			await this.selectModelById(availableModels[0].id);
		}
	}

	/**
	 * Fetch list of models from server and detect server role.
	 * Also fetches modalities for MODEL mode (single model).
	 */
	async fetch(force = false): Promise<void> {
		if (this.inflightFetch) return this.inflightFetch;

		if (this.models.length > 0 && !force) return;

		this.inflightFetch = this.runFetch();
		try {
			await this.inflightFetch;
		} finally {
			this.inflightFetch = null;
		}
	}

	/**
	 * Fetch router models with full metadata (ROUTER mode only).
	 * Used by the model status feed and when the picker opens.
	 */
	async fetchRouterModels(): Promise<void> {
		if (!serverStore.isRouterMode) return;

		try {
			const response = await ModelsService.listRouter();

			this.updateRouterModels(response);
			await this.props.fetchModalitiesForLoadedModels();

		} catch (error) {
			console.warn('Failed to fetch router models:', error);
			this.routerModels = [];
		}
	}

	findModelById(modelId: string): ModelOption | null {
		return this.models.find((model) => model.id === modelId) ?? null;
	}

	findModelByName(modelName: string): ModelOption | null {
		return (
			this.models.find(
				(model) =>
					model.model === modelName || model.id === modelName || model.aliases?.includes(modelName)
			) ?? null
		);
	}

	getModelStatus(modelId: string): ServerModelStatus | null {
		const model = this.routerModels.find((m) => m.id === modelId);

		return model?.status.value ?? null;
	}

	hasModel(modelName: string): boolean {
		return this.models.some((model) => model.model === modelName);
	}

	isFavorite(modelId: string): boolean {
		return this.favoriteModelIds.has(modelId);
	}

	isModelLoaded(modelId: string): boolean {
		const model = this.routerModels.find((m) => m.id === modelId);

		return (
			model?.status.value === ServerModelStatus.LOADED ||
			model?.status.value === ServerModelStatus.SLEEPING
		);
	}

	async selectModelById(modelId: string): Promise<void> {
		if (!modelId || this.updating) return;

		if (this.selectedModelId === modelId) return;

		const option = this.models.find((model) => model.id === modelId);

		if (!option) throw new Error('Selected model is not available');

		this.updating = true;
		this.error = null;

		try {
			this.selectedModelId = option.id;
			this.selectedModelName = option.model;
		} finally {
			this.updating = false;
		}
	}

	toDisplayName(id: string): string {
		const segments = id.split(/\\|\//);
		const candidate = segments.pop();

		return candidate && candidate.trim().length > 0 ? candidate : id;
	}

	async toggleFavorite(modelId: string): Promise<void> {
		if (this.favoriteUpdates.has(modelId)) return;
		const next = new SvelteSet(this.favoriteModelIds);

		if (next.has(modelId)) {
			next.delete(modelId);
		} else {
			next.add(modelId);
		}

		this.favoriteModelIds = next;
		if (this.routerModels.find((model) => model.id === modelId)?.sparkmoe) {
			this.favoriteUpdates.add(modelId);
			try {
				await SparkMoeService.updateLibraryModel(modelId, { favorite: next.has(modelId) });
			} catch (error) {
				const restored = new SvelteSet(this.favoriteModelIds);
				if (next.has(modelId)) restored.delete(modelId);
				else restored.add(modelId);
				this.favoriteModelIds = restored;
				toast.error(error instanceof Error ? error.message : 'Could not save favorite');
			} finally {
				this.favoriteUpdates.delete(modelId);
			}
			return;
		}

		try {
			localStorage.setItem(FAVORITE_MODELS_LOCALSTORAGE_KEY, JSON.stringify([...next]));
		} catch {
			toast.error('Failed to save favorite models to local storage');
		}
	}

	private updateRouterModels(response: ApiRouterModelsListResponse): void {
		const previouslyLoaded = new Set(this.loadedModelIds);
		this.routerModels = response.data;
		this.models = this.buildModelOptions(response);
		this.syncSparkMoeFavorites();
		if (this.selectedModelId && !this.selectedModel) this.clearSelection();

		const newlyLoaded = this.routerModels.filter(
			(model) => model.sparkmoe && this.isModelLoaded(model.id) && !previouslyLoaded.has(model.id)
		);
		const latest = newlyLoaded.find((model) => model.sparkmoe?.last_loaded) ?? newlyLoaded.at(-1);
		if (latest) void this.selectModelById(latest.id);
	}

	onModelLoaded(modelId: string): void {
		const row = this.routerModels.find((model) => model.id === modelId);
		if (row?.sparkmoe && this.findModelById(modelId)) void this.selectModelById(modelId);
	}

	private syncSparkMoeFavorites(): void {
		if (!this.routerModels.some((model) => model.sparkmoe)) return;
		const favorites = new SvelteSet(
			this.routerModels.filter((model) => model.sparkmoe?.favorite).map((model) => model.id)
		);
		for (const id of this.favoriteUpdates) {
			if (this.favoriteModelIds.has(id)) favorites.add(id);
			else favorites.delete(id);
		}
		this.favoriteModelIds = favorites;
	}

	/**
	 * Build ModelOption[] from an API response.
	 * Both MODEL and ROUTER modes share the same mapping logic;
	 * they differ only in which endpoint is called.
	 */
	private buildModelOptions(
		response: ApiModelListResponse | ApiRouterModelsListResponse
	): ModelOption[] {
		return response.data.map((item: ApiModelDataEntry, index: number) => {
			const details = response.models?.[index];
			const rawCapabilities = Array.isArray(details?.capabilities) ? details?.capabilities : [];
			const displayNameSource =
				details?.name && details.name.trim().length > 0 ? details.name : item.name || item.id;
			const modelId = details?.model || item.id;

			return {
				aliases: item.aliases ?? [],
				capabilities: rawCapabilities.filter((value: unknown): value is string => Boolean(value)),
				description: details?.description,
				details: details?.details,
				id: item.id,
				meta: item.meta ?? null,
				modalities: this.props.buildArchitectureModalities(item.architecture),
				model: modelId,
				name: this.toDisplayName(displayNameSource),
				parsedId: ModelsService.parseModelId(modelId),
				tags: item.tags ?? []
			};
		});
	}

	/** Fetch models in MODEL mode (single model, standard OpenAI-compatible). */
	private async fetchModelModeInternal(): Promise<ModelOption[]> {
		const response = await ModelsService.list();

		return this.buildModelOptions(response);
	}

	/**
	 * Filter to models visible in the UI (ui !== false).
	 */
	private getVisibleModels(): ModelOption[] {
		return this.models.filter((option) => this.props.getModelProps(option.model)?.ui !== false);
	}

	private loadFavoritesFromStorage(): Set<string> {
		try {
			const raw = localStorage.getItem(FAVORITE_MODELS_LOCALSTORAGE_KEY);

			return raw ? new Set(JSON.parse(raw) as string[]) : new Set();
		} catch {
			toast.error('Failed to load favorite models from local storage');

			return new Set();
		}
	}

	private async runFetch(): Promise<void> {
		this.loading = true;
		this.error = null;

		try {
			if (!serverStore.props) {
				await serverStore.fetch();
			}

			const router = serverStore.isRouterMode;

			if (router) {
				const response = await ModelsService.listRouter();

				this.updateRouterModels(response);
				await this.props.fetchModalitiesForLoadedModels();
			} else {
				this.models = await this.fetchModelModeInternal();
			}
		} catch (error) {
			this.models = [];
			this.error = error instanceof Error ? error.message : 'Failed to load models';

			throw error;
		} finally {
			this.loading = false;
		}
	}
}

export const modelsStore = new ModelsStore();
