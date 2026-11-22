<script lang="ts">
	import { t } from '$lib/i18n/locale.svelte';
	import ChatMessageActionCard from './ChatMessageActionCard.svelte';
	import { RotateCw } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';

	interface Props {
		onDecision: (shouldContinue: boolean) => void;
	}

	let { onDecision }: Props = $props();
</script>

<ChatMessageActionCard icon={RotateCw}>
	{#snippet message()}
		Agentic turn limit reached. Continue?
	{/snippet}

	{#snippet actions()}
		<Button size="sm" onclick={() => onDecision(true)}>Continue</Button>

		<Button
			variant="destructive"
			size="sm"
			class="text-destructive hover:text-destructive"
			onclick={() => onDecision(false)}
		>
			{t("Stop")}
		</Button>
	{/snippet}
</ChatMessageActionCard>
