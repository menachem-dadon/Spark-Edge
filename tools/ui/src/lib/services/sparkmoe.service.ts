// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

import { base } from '$app/paths';
import { getAuthHeaders, getJsonHeaders } from '$lib/utils/api-headers';

const MANAGER_URL = 'http://127.0.0.1:18080';
let resolvedManagerUrl: string | undefined;
let managerDiscovery: Promise<string> | undefined;

function managerUrl(): string {
	return resolvedManagerUrl ?? MANAGER_URL;
}

async function discoverManager(): Promise<string> {
	if (resolvedManagerUrl) return resolvedManagerUrl;
	if (managerDiscovery) return managerDiscovery;
	managerDiscovery = (async () => {
		if (typeof window !== 'undefined') {
			try {
				const response = await fetch(window.location.origin + '/api/status', {
					signal: AbortSignal.timeout(1500)
				});
				const status = await response.json();
				if (response.ok && typeof status.manager_port === 'number') return window.location.origin;
			} catch {
				/* Try the local manager when this UI is served by the router. */
			}
		}
		return MANAGER_URL;
	})().then((url) => {
		resolvedManagerUrl = url;
		return url;
	});
	return managerDiscovery;
}

export interface SparkMoeDownload {
	id: string;
	url: string;
	filename: string;
	status: 'queued' | 'downloading' | 'paused' | 'failed' | 'completed';
	downloaded_bytes: number;
	total_bytes: number;
	error: string;
	path: string;
}

export interface SparkMoeLibraryModel {
	id: string;
	display_name: string;
	path: string;
	favorite: boolean;
	draft_only?: boolean;
	available: boolean;
	size_bytes: number;
	legacy_ids?: string[];
	metadata?: Record<string, unknown>;
}

export interface EnginePackage {
	id: string;
	backend: string;
	bytes?: number;
}

export interface EngineStatus {
	installed: EnginePackage[];
	available?: EnginePackage[];
	active: string;
	application_version?: string;
	bundled_commit?: string;
}

export interface SparkMoeApplication {
	locale?: 'he' | 'en';
	background_mode: boolean;
	launch_at_login: boolean;
	default_model: string;
}

export interface SparkMoeConnector {
	id: string;
	name: string;
	installed: boolean;
	executable: string;
	version: string;
	support: string;
	launch_supported: boolean;
}
export interface SparkMoeConnection {
	id: string;
	connector: string;
	model_id: string;
	project_dir: string;
	include_favorites: boolean;
	experimental: boolean;
	config_dir: string;
}
export interface SparkMoeConnectionInput {
	connector: string;
	model_id: string;
	project_dir: string;
	include_favorites: boolean;
	experimental: boolean;
	id?: string;
}
export interface SparkMoeConnectionPreview {
	connector: string;
	model_id: string;
	files: { name: string; content: string }[];
	arguments: string[];
	warnings: string[];
	launch_supported: boolean;
	connection?: SparkMoeConnection;
}

export interface SparkMoeRouterConfig {
	model_directory: string;
	models_autoload: boolean;
	models_max: number;
	port: number;
}

export interface SparkMoeManagerStatus {
	configuration_warning?: string;
	pending?: boolean;
	catalog_revision?: number;
	config: SparkMoeRouterConfig;
	last_error: string;
	last_exit_code: number | null;
	log_session: string;
	manager_port: number;
	phase: 'ready' | 'starting' | 'stopped' | 'exited';
	pid: number;
	port: number;
	running: boolean;
	server_ready: boolean;
	started_at_ms: number;
}

export interface SparkMoeModelProfile {
	context_mode: 'auto' | 'manual';
	memory_mode: 'auto' | 'manual';
	gpu_layers: number;
	gpu_expert_cache_mib?: number;
	worker_engine?: string;
	batch_size: number;
	cache: number;
	cache_policy: 'lru' | 'hybrid';
	cache_ram_mib: number;
	cache_type_k: string;
	cache_type_v: string;
	context_shift: boolean;
	cpu_repack: 'auto' | 'on' | 'off';
	ctx_checkpoints: number;
	ctx_size: number;
	dense_residency: 'auto' | 'mmap' | 'resident' | 'locked';
	inference_threads: number;
	io_mode: 'buffered' | 'direct';
	io_threads: number;
	mode: 'auto' | 'os' | 'explicit' | 'off';
	mtp_backend_sampling: boolean;
	mtp_cache_mib: number;
	mtp_draft_tokens: number;
	speculative_types?: string;
	ngram_options?: Record<string, number>;
	mtp_enabled: boolean;
	mtp_min_tokens: number;
	mtp_model: string;
	mtp_p_min: number;
	mtp_threads: number;
	overlap: 'on' | 'off';
	paged_layers: number;
	parallel: number;
	ple_cache_mib: number;
	prefetch: 'off' | 'temporal';
	reasoning: 'default' | 'off' | 'low' | 'medium' | 'high' | 'max';
	slots: number;
	ubatch_size: number;
}

export interface SparkMoeProfileResponse {
	is_override: boolean;
	model: string;
	profile: SparkMoeModelProfile;
	revision: number;
	applied_profile?: SparkMoeModelProfile | null;
	pending?: boolean;
	recommendation?: Record<string, unknown>;
}

export interface SparkMoeLogSession {
	current: boolean;
	name: string;
	size: number;
}

export interface SparkMoeLogSessionsResponse {
	directory: string;
	sessions: SparkMoeLogSession[];
}

export interface SparkMoeLogChunk {
	content: string;
	next_offset: number;
	offset: number;
	session: string;
	total_size: number;
	truncated: boolean;
}

export class SparkMoeRequestError extends Error {
	constructor(
		message: string,
		readonly status: number
	) {
		super(message);
	}
}

async function managerFetch<T>(path: string, init?: RequestInit): Promise<T> {
	const response = await fetch((await discoverManager()) + path, init);
	const payload = (await response.json()) as T & { error?: string };

	if (!response.ok) {
		throw new SparkMoeRequestError(
			payload.error || 'Spark Edge manager returned HTTP ' + response.status,
			response.status
		);
	}

	return payload;
}

async function routerFetch<T>(path: string, init?: RequestInit): Promise<T> {
	const response = await fetch(base + path, init);
	const payload = (await response.json()) as T & {
		error?: string | { message?: string };
	};

	if (!response.ok) {
		const message =
			typeof payload.error === 'string'
				? payload.error
				: payload.error?.message || response.statusText;

		throw new Error(message || 'llama.cpp router returned HTTP ' + response.status);
	}

	return payload;
}

export class SparkMoeService {
	static async chooseModelFile(): Promise<{ cancelled: boolean; path?: string }> {
		return managerFetch('/api/browse-model-file', { method: 'POST' });
	}
	static async downloads(): Promise<{ jobs: SparkMoeDownload[]; directory: string }> {
		return managerFetch('/api/downloads');
	}
	static async download(url: string): Promise<unknown> {
		return managerFetch('/api/downloads', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ url })
		});
	}
	static async downloadAction(action: 'pause' | 'resume', id: string): Promise<unknown> {
		return managerFetch('/api/downloads/' + action, {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ id })
		});
	}
	static async downloadCatalog(
		repo: string
	): Promise<{ repo: string; files: { name: string; url: string; size_bytes: number }[] }> {
		return managerFetch('/api/downloads/catalog?repo=' + encodeURIComponent(repo));
	}
	static async searchDownloads(
		query: string
	): Promise<{ models: { id: string; downloads: number }[] }> {
		return managerFetch('/api/downloads/search?q=' + encodeURIComponent(query));
	}
	static get managerUrl(): string {
		return managerUrl();
	}

	static async library(
		rescan = false
	): Promise<{ models: SparkMoeLibraryModel[]; revision: number; catalog_revision?: number }> {
		return managerFetch('/api/library' + (rescan ? '?rescan=1' : ''));
	}

	static async importModel(path: string): Promise<unknown> {
		return managerFetch('/api/library', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ path })
		});
	}

	static async updateLibraryModel(
		id: string,
		changes: Partial<Pick<SparkMoeLibraryModel, 'display_name' | 'favorite' | 'path'>>
	): Promise<unknown> {
		return managerFetch('/api/library', {
			method: 'PATCH',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ id, ...changes })
		});
	}

	static async removeLibraryModel(id: string): Promise<unknown> {
		return managerFetch('/api/library?id=' + encodeURIComponent(id), { method: 'DELETE' });
	}

	static async engines(check = false): Promise<EngineStatus> {
		return managerFetch('/api/engines' + (check ? '?check=1' : ''));
	}

	static async downloadEngine(id: string): Promise<void> {
		await managerFetch('/api/engines/download', {
			method: 'POST',
			body: JSON.stringify({ id })
		});
	}

	static async applyEngine(id: string): Promise<void> {
		await managerFetch('/api/engines/apply', {
			method: 'POST',
			body: JSON.stringify({ id })
		});
	}

	static async application(): Promise<SparkMoeApplication> {
		return managerFetch('/api/application');
	}

	static async saveApplication(
		settings: Partial<SparkMoeApplication>
	): Promise<SparkMoeApplication> {
		return managerFetch('/api/application', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify(settings)
		});
	}

	static async applyModel(model: string): Promise<unknown> {
		return managerFetch('/api/model-apply', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ model })
		});
	}

	static async connections(): Promise<{
		connectors: SparkMoeConnector[];
		connections: SparkMoeConnection[];
	}> {
		return managerFetch('/api/connections');
	}

	static async previewConnection(
		input: SparkMoeConnectionInput
	): Promise<SparkMoeConnectionPreview> {
		return managerFetch('/api/connections/preview', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify(input)
		});
	}

	static async configureConnection(
		input: SparkMoeConnectionInput
	): Promise<SparkMoeConnectionPreview> {
		return managerFetch('/api/connections/configure', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify(input)
		});
	}

	static async connectionAction(action: 'launch' | 'remove', id: string): Promise<unknown> {
		return managerFetch('/api/connections/' + action, {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ id })
		});
	}

	static async chooseModelsDirectory(): Promise<{ cancelled: boolean; directory?: string }> {
		return managerFetch('/api/browse-model-directory', { method: 'POST' });
	}

	static diagnosticsUrl(): string {
		return managerUrl() + '/api/diagnostics';
	}

	static async logChunk(session: string, offset?: number): Promise<SparkMoeLogChunk> {
		const params = new URLSearchParams({ limit: '262144', session });

		if (offset !== undefined) params.set('offset', String(offset));

		return managerFetch('/api/logs?' + params.toString());
	}

	static logDownloadUrl(session: string): string {
		return managerUrl() + '/api/logs/download?session=' + encodeURIComponent(session);
	}

	static async logSessions(): Promise<SparkMoeLogSessionsResponse> {
		return managerFetch('/api/log-sessions');
	}

	static async modelProfile(model: string): Promise<SparkMoeProfileResponse> {
		return managerFetch('/api/model-profile?model=' + encodeURIComponent(model));
	}

	static async paging(model: string): Promise<Record<string, unknown>> {
		return routerFetch('/sparkmoe/v1/paging?model=' + encodeURIComponent(model), {
			headers: getAuthHeaders()
		});
	}

	static async prometheusMetrics(model: string): Promise<Record<string, number>> {
		const response = await fetch(base + '/metrics?model=' + encodeURIComponent(model), {
			headers: getAuthHeaders()
		});

		if (!response.ok) {
			throw new Error(response.statusText || 'llama.cpp router metrics are unavailable');
		}

		const metrics: Record<string, number> = {};

		for (const line of (await response.text()).split('\n')) {
			if (!line || line.startsWith('#')) continue;

			const match = line.match(/^([^\s{]+)(?:\{[^}]*\})?\s+([^\s]+)$/);
			const value = match ? Number(match[2]) : Number.NaN;

			if (match && Number.isFinite(value)) {
				metrics[match[1]] = (metrics[match[1]] || 0) + value;
			}
		}

		return metrics;
	}

	static async recommendation(): Promise<Record<string, number | string>> {
		return managerFetch('/api/recommendation');
	}

	static async reloadModels(): Promise<void> {
		await routerFetch('/models?reload=1', { headers: getAuthHeaders() });
	}

	static async resetMetrics(model: string): Promise<Record<string, unknown>> {
		return routerFetch('/sparkmoe/v1/paging/reset-stats', {
			body: JSON.stringify({ model }),
			headers: getJsonHeaders(),
			method: 'POST'
		});
	}

	static async resetModelProfile(
		model: string,
		revision?: number
	): Promise<SparkMoeProfileResponse> {
		return managerFetch(
			'/api/model-profile?model=' +
				encodeURIComponent(model) +
				(revision === undefined ? '' : '&revision=' + revision),
			{
				method: 'DELETE'
			}
		);
	}

	static async resizeCache(model: string, cacheMib: number): Promise<Record<string, unknown>> {
		return routerFetch('/sparkmoe/v1/paging/cache', {
			body: JSON.stringify({ cache_mib: cacheMib, model }),
			headers: getJsonHeaders(),
			method: 'POST'
		});
	}

	static async restart(): Promise<void> {
		await managerFetch('/api/restart', { method: 'POST' });
	}

	static async routerConfig(): Promise<SparkMoeRouterConfig> {
		return managerFetch('/api/config');
	}

	static async runtimeConfig(model: string): Promise<Record<string, unknown>> {
		return routerFetch('/sparkmoe/v1/config?model=' + encodeURIComponent(model), {
			headers: getAuthHeaders()
		});
	}

	static async saveModelProfile(
		model: string,
		profile: Partial<SparkMoeModelProfile>,
		revision?: number
	): Promise<SparkMoeProfileResponse> {
		return managerFetch('/api/model-profile', {
			body: JSON.stringify({ model, profile, revision }),
			headers: { 'content-type': 'application/json' },
			method: 'POST'
		});
	}

	static async saveRouterConfig(
		config: Partial<SparkMoeRouterConfig>
	): Promise<{ ok: boolean; port: number; status: SparkMoeManagerStatus }> {
		return managerFetch('/api/config', {
			body: JSON.stringify(config),
			headers: { 'content-type': 'application/json' },
			method: 'POST'
		});
	}

	static async shutdown(): Promise<void> {
		await managerFetch('/api/shutdown', { method: 'POST' });
	}

	static async start(): Promise<void> {
		await managerFetch('/api/start', { method: 'POST' });
	}

	static async status(): Promise<SparkMoeManagerStatus> {
		return managerFetch('/api/status');
	}

	static async stop(): Promise<void> {
		await managerFetch('/api/stop', { method: 'POST' });
	}
}
