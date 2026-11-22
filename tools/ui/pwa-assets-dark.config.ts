import { PWA_ASSET_GENERATOR } from './src/lib/constants/pwa.constants';
import { defineConfig } from '@vite-pwa/assets-generator/config';


export default defineConfig({
	headLinkOptions: {
		preset: '2023'
	},
	images: ['static/spark-edge.png'],
	preset: {
		apple: {
			sizes: []
		},
		maskable: {
			sizes: []
		},
		transparent: {
			favicons: [[48, 'favicon-dark.ico']],
			padding: PWA_ASSET_GENERATOR.FAVICON_PADDING,
			sizes: []
		}
	}
});
