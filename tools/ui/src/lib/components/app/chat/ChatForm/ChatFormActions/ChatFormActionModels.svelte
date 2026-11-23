<script lang="ts">
	import { ModelsSelectorDropdown, ModelsSelectorSheet } from '$lib/components/app';
	import { deviceStore, modelsStore, serverStore } from '$lib/stores';
	import { t } from '$lib/i18n/locale.svelte';

	interface Props {
		disabled?: boolean;
		forceForegroundText?: boolean;
		hasAudioModality?: boolean;
		hasVideoModality?: boolean;
		hasVisionModality?: boolean;
		hasModelSelected?: boolean;
		isSelectedModelInCache?: boolean;
		submitTooltip?: string;
		useGlobalSelection?: boolean;
	}

	let {
		disabled = false,
		forceForegroundText = false,
		hasAudioModality = $bindable(false),
		hasModelSelected = $bindable(false),
		hasVideoModality = $bindable(false),
		hasVisionModality = $bindable(false),
		isSelectedModelInCache = $bindable(true),
		submitTooltip = $bindable(''),
		useGlobalSelection = false
	}: Props = $props();

	let isRouter = $derived(serverStore.isRouterMode);
	let isOffline = $derived(!!serverStore.error);

	let selectorModel = $derived(modelsStore.selectedModelName);

	let activeModelId = $derived(modelsStore.activeModelId);

	let modelPropsVersion = $state(0); // Used to trigger reactivity after fetch

	$effect(() => {
		if (activeModelId) {
			const cached = modelsStore.props.getModelProps(activeModelId);

			if (!cached) {
				modelsStore.props.fetchModelProps(activeModelId).then(() => {
					modelPropsVersion++;
				});
			}
		}
	});

	$effect(() => {
		void modelPropsVersion;

		hasAudioModality = activeModelId ? modelsStore.props.modelSupportsAudio(activeModelId) : false;
	});

	$effect(() => {
		void modelPropsVersion;

		hasVideoModality = activeModelId ? modelsStore.props.modelSupportsVideo(activeModelId) : false;
	});

	$effect(() => {
		void modelPropsVersion;

		hasVisionModality = activeModelId
			? modelsStore.props.modelSupportsVision(activeModelId)
			: false;
	});

	$effect(() => {
		hasModelSelected = !isRouter || !!modelsStore.selectedModelId;
		isSelectedModelInCache = !isRouter || !!modelsStore.selectedModel;
		submitTooltip = !hasModelSelected
			? t('Please select a model first')
			: !isSelectedModelInCache
				? t('Selected model is not available, please select another')
				: '';
	});

	let selectorModelRef: ModelsSelectorDropdown | ModelsSelectorSheet | undefined =
		$state(undefined);

	export function open() {
		selectorModelRef?.open();
	}
</script>

{#if deviceStore.isMobile}
	<ModelsSelectorSheet
		disabled={disabled || isOffline}
		bind:this={selectorModelRef}
		currentModel={selectorModel}
		{forceForegroundText}
		{useGlobalSelection}
	/>
{:else}
	<ModelsSelectorDropdown
		disabled={disabled || isOffline}
		bind:this={selectorModelRef}
		currentModel={selectorModel}
		{forceForegroundText}
		{useGlobalSelection}
	/>
{/if}
