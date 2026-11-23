<script lang="ts">
	import { ModelBadge } from '$lib/components/app';
	import { copyToClipboard } from '$lib/utils';

	let { displayedModel }: { displayedModel: string | null } = $props();
</script>

<ModelBadge model={displayedModel || undefined} onclick={() => void copyToClipboard(displayedModel ?? '')} />
