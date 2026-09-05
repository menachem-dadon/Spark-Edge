// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

import { base } from '$app/paths';
import { getAuthHeaders, getJsonHeaders } from '$lib/utils/api-headers';

const MANAGER_URL = 'http://127.0.0.1:18080';

export interface SparkMoeRouterConfig {
	model_directory: string;
	models_autoload: boolean;
	models_max: number;
	port: number;
}

export interface SparkMoeManagerStatus {
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

async function managerFetch<T>(path: string, init?: RequestInit): Promise<T> {
	const response = await fetch(MANAGER_URL + path, init);
	const payload = (await response.json()) as T & { error?: string };

	if (!response.ok) {
		throw new Error(payload.error || 'SparkMoE manager returned HTTP ' + response.status);
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
	static readonly managerUrl = MANAGER_URL;

	static async chooseModelsDirectory(): Promise<{ cancelled: boolean; directory?: string }> {
		return managerFetch('/api/browse-model-directory', { method: 'POST' });
	}

	static diagnosticsUrl(): string {
		return MANAGER_URL + '/api/diagnostics';
	}

	static async logChunk(session: string, offset?: number): Promise<SparkMoeLogChunk> {
		const params = new URLSearchParams({ limit: '262144', session });

		if (offset !== undefined) params.set('offset', String(offset));

		return managerFetch('/api/logs?' + params.toString());
	}

	static logDownloadUrl(session: string): string {
		return MANAGER_URL + '/api/logs/download?session=' + encodeURIComponent(session);
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
		model: string
	): Promise<{ ok: boolean; router_reloaded: boolean }> {
		return managerFetch('/api/model-profile?model=' + encodeURIComponent(model), {
			method: 'DELETE'
		});
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
		profile: SparkMoeModelProfile
	): Promise<{ ok: boolean; router_reloaded: boolean }> {
		return managerFetch('/api/model-profile', {
			body: JSON.stringify({ model, profile }),
			headers: { 'content-type': 'application/json' },
			method: 'POST'
		});
	}

	static async saveRouterConfig(
		config: SparkMoeRouterConfig
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
