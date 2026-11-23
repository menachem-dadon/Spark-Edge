<script lang="ts">
	import { X } from '@lucide/svelte';
	import { browser } from '$app/environment';
	import { goto } from '$app/navigation';
	import { page } from '$app/state';
	import { ActionIcon } from '$lib/components/app';
	import { ROUTES } from '$lib/constants';

	let { children } = $props();

	let previousRouteId = $state<string | null>(null);

	$effect(() => {
		const currentId = page.route.id;

		return () => {
			previousRouteId = currentId;
		};
	});

	function handleClose() {
		const prevIsSettings = previousRouteId?.startsWith('/settings');

		if (browser && window.history.length > 1 && !prevIsSettings) {
			history.back();
		} else {
			goto(ROUTES.SETTINGS_EXIT);
		}
	}
</script>

<div class="fixed top-[calc(1.125rem+var(--desktop-caption-height,0px))] start-4 z-50 md:hidden">
	<ActionIcon icon={X} tooltip="Close" onclick={handleClose} />
</div>

<div class="min-h-full">
	{@render children?.()}
</div>
