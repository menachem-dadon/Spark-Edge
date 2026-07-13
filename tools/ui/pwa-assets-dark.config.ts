import { defineConfig } from '@vite-pwa/assets-generator/config';
import { PWA_ASSET_GENERATOR } from './src/lib/constants/pwa';

export default defineConfig({
	headLinkOptions: {
		preset: '2023'
	},
	preset: {
		transparent: {
			sizes: [],
			favicons: [[48, 'favicon-dark.ico']],
			padding: PWA_ASSET_GENERATOR.FAVICON_PADDING
		},
		maskable: {
			sizes: []
		},
		apple: {
			sizes: []
		}
	},
	images: ['static/SparkMoE.png']
});
