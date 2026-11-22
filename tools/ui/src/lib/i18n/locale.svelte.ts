import hebrew from './he.json';

export type AppLocale = 'he' | 'en';

const STORAGE_KEY = 'spark-edge.locale';
const messages: Record<string, string> = hebrew;

class LocaleStore {
	language = $state<AppLocale>('he');

	init() {
		const locale = localStorage.getItem(STORAGE_KEY);
		this.select(locale === 'en' ? 'en' : 'he');
	}

	select(language: AppLocale) {
		this.language = language;
		if (typeof document !== 'undefined') {
			document.documentElement.lang = language;
			document.documentElement.dir = language === 'he' ? 'rtl' : 'ltr';
			localStorage.setItem(STORAGE_KEY, language);
		}
	}
}

export const appLocale = new LocaleStore();

export function t(message: string): string {
	return appLocale.language === 'he' ? (messages[message] ?? message) : message;
}
