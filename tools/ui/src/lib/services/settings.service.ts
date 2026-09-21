import { browser } from '$app/environment';
import { CONFIG_LOCALSTORAGE_KEY, USER_OVERRIDES_LOCALSTORAGE_KEY } from '$lib/constants';

/**
 * SettingsService - localStorage persistence layer for settings
 *
 * Stateless read/write of the settings config and user-override keys. Business
 * logic (default merging, mobile defaults, theme migration) stays in the store.
 *
 * **Architecture & Relationships:**
 * - **settingsStore**: Primary consumer - loads config on init and persists on change
 *
 * @see settingsStore in stores/settings/index.svelte.ts - reactive state + business logic
 */
export class SettingsService {
	/**
	 * Read the raw config and user overrides from localStorage.
	 * @returns Parsed values, or empty defaults when nothing is stored or parsing fails.
	 */
	static loadConfig(): {
		config: Record<string, unknown>;
		userOverrides: string[];
		isFirstVisit: boolean;
	} {
		if (!browser) {
			return { config: {}, isFirstVisit: false, userOverrides: [] };
		}

		try {
			const storedConfigRaw = localStorage.getItem(CONFIG_LOCALSTORAGE_KEY);
			const isFirstVisit = storedConfigRaw === null;
			const config = JSON.parse(storedConfigRaw || '{}') as Record<string, unknown>;
			const userOverrides = JSON.parse(
				localStorage.getItem(USER_OVERRIDES_LOCALSTORAGE_KEY) || '[]'
			) as string[];

			return { config, isFirstVisit, userOverrides };
		} catch (error) {
			console.warn('Failed to parse config from localStorage, using defaults:', error);

			return { config: {}, isFirstVisit: false, userOverrides: [] };
		}
	}

	/**
	 * Migrate the legacy un-namespaced "theme" localStorage key.
	 * Returns the legacy theme value (and removes the key) when present, else null.
	 */
	static migrateLegacyTheme(): string | null {
		if (!browser) return null;

		const legacyTheme = localStorage.getItem('theme');

		if (legacyTheme) {
			localStorage.removeItem('theme');

			return legacyTheme;
		}

		return null;
	}

	/**
	 * Persist the config and user overrides to localStorage.
	 */
	static saveConfig(config: Record<string, unknown>, userOverrides: string[]): boolean {
		if (!browser) return true;
		let previous: [string | null, string | null] | undefined;

		try {
			previous = [
				localStorage.getItem(CONFIG_LOCALSTORAGE_KEY),
				localStorage.getItem(USER_OVERRIDES_LOCALSTORAGE_KEY)
			];
			localStorage.setItem(CONFIG_LOCALSTORAGE_KEY, JSON.stringify(config));
			localStorage.setItem(USER_OVERRIDES_LOCALSTORAGE_KEY, JSON.stringify(userOverrides));
			return true;
		} catch (error) {
			if (previous) {
				try {
					for (const [index, key] of [
						CONFIG_LOCALSTORAGE_KEY,
						USER_OVERRIDES_LOCALSTORAGE_KEY
					].entries()) {
						const value = previous[index];
						if (value === null) localStorage.removeItem(key);
						else localStorage.setItem(key, value);
					}
				} catch {
					/* The caller keeps the draft if storage remains unavailable. */
				}
			}
			console.error('Failed to save config to localStorage:', error);
			return false;
		}
	}
}
