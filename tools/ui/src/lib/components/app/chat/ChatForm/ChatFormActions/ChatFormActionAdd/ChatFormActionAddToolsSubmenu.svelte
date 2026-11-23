<script lang="ts">
	import { t } from '$lib/i18n/locale.svelte';
	import { Check, ChevronDown, ChevronRight, Info, Loader2, PencilRuler } from '@lucide/svelte';
	import { Checkbox } from '$lib/components/ui/checkbox';
	import * as Collapsible from '$lib/components/ui/collapsible';
	import * as DropdownMenu from '$lib/components/ui/dropdown-menu';
	import * as Tooltip from '$lib/components/ui/tooltip';
	import { CLI_FLAGS, ICON_CLASS_DEFAULT } from '$lib/constants';
	import { useToolsPanel } from '$lib/hooks/use-tools-panel.svelte';
	import { toolsStore } from '$lib/stores';

	const toolsPanel = useToolsPanel();
</script>

<DropdownMenu.Sub onOpenChange={(open) => open && toolsPanel.handleOpen()}>
	<DropdownMenu.SubTrigger class="flex cursor-pointer items-center gap-2">
		<PencilRuler class={ICON_CLASS_DEFAULT} />

		<span>{t("Tools")}</span>
	</DropdownMenu.SubTrigger>

	<DropdownMenu.SubContent class="w-72 p-0">
		{#if toolsPanel.totalToolCount === 0}
			{#if toolsStore.loading}
				<div class="px-3 py-4 text-center text-sm text-muted-foreground">
					<Loader2 class="mx-auto mb-1 {ICON_CLASS_DEFAULT} animate-spin" />

					{t("Loading tools...")}
				</div>
			{:else if toolsStore.isToolsEndpointUnreachable}
				<div class="grid gap-2.5 px-3 py-4 text-sm text-muted-foreground">
					<span class="flex gap-2">
						<Info class="mt-0.5 {ICON_CLASS_DEFAULT} shrink-0" />

						<span>
							{t('Run llama-server with this flag to enable server tools')} <code>{CLI_FLAGS.TOOLS}</code>.
						</span>
					</span>

					<span class="flex gap-2">
						<Info class="mt-0.5 {ICON_CLASS_DEFAULT} shrink-0" />

						<span>
							{t('Enable tools on an MCP server to use them here.')}
						</span>
					</span>
				</div>
			{:else if toolsStore.error}
				<div class="px-3 py-4 text-center text-sm text-muted-foreground">{t("Failed to load tools")}</div>
			{:else if toolsPanel.noToolsInfoMessage}
				<div class="flex gap-2 px-3 py-4 text-sm text-muted-foreground">
					<Info class="mt-0.5 {ICON_CLASS_DEFAULT} shrink-0" />

					<span>{t(toolsPanel.noToolsInfoMessage)}</span>
				</div>
			{:else}
				<div class="px-3 py-4 text-center text-sm text-muted-foreground">{t("No tools available")}</div>
			{/if}
		{:else}
			<div class="max-h-80 overflow-y-auto p-2 pe-1">
				{#each toolsPanel.activeGroups as group (group.key)}
					{@const isExpanded = toolsPanel.expandedGroups.has(group.key)}
					{@const checked = toolsPanel.isGroupChecked(group)}
					{@const favicon = toolsPanel.getFavicon(group)}

					<Collapsible.Root
						open={isExpanded}
						onOpenChange={() => toolsPanel.toggleGroupExpanded(group.key)}
					>
						<div class="flex items-center gap-1">
							<Collapsible.Trigger
								class="flex min-w-0 flex-1 items-center gap-2 rounded px-2 py-1.5 text-sm hover:bg-muted/50"
							>
								{#if isExpanded}
									<ChevronDown class="h-3.5 w-3.5 shrink-0" />
								{:else}
									<ChevronRight class="rtl:rotate-180 h-3.5 w-3.5 shrink-0" />
								{/if}

								<span class="inline-flex min-w-0 items-center gap-1.5 font-medium">
									{#if favicon}
										<img
											src={favicon}
											alt=""
											class="{ICON_CLASS_DEFAULT} shrink-0 rounded-sm"
											onerror={(e) => {
												(e.currentTarget as HTMLImageElement).style.display = 'none';
											}}
										/>
									{/if}

									<span class="truncate">{group.label}</span>
								</span>

								<span class="ms-auto shrink-0 text-xs text-muted-foreground">
									{toolsPanel.getEnabledToolCount(group)}/{group.tools.length}
								</span>
							</Collapsible.Trigger>

							<Tooltip.Root>
								<Tooltip.Trigger>
									{#snippet child({ props })}
										<Checkbox
											{...props}
											{checked}
											onCheckedChange={() => toolsPanel.toggleGroupByKey(group.key)}
											class="me-2 {ICON_CLASS_DEFAULT} shrink-0"
										/>
									{/snippet}
								</Tooltip.Trigger>

								<Tooltip.Content side="right">
									<p>
										{t(checked ? 'Disable' : 'Enable')}
										{group.tools.length} {t('tools')}
									</p>
								</Tooltip.Content>
							</Tooltip.Root>
						</div>

						<Collapsible.Content>
							<div class="ms-4 flex flex-col gap-0.5 border-s border-border/50 ps-2">
								{#each group.tools as entry (entry.key)}
									{@const enabled = toolsStore.isToolEnabled(entry.key)}
									<button
										type="button"
										class="flex w-full items-center gap-2 rounded px-2 py-1.5 text-start text-sm transition-colors hover:bg-muted/50"
										onclick={() => toolsStore.toggleTool(entry.key)}
									>
										<span
											data-slot="checkbox"
											data-state={enabled ? 'checked' : 'unchecked'}
											class="flex size-4 shrink-0 items-center justify-center rounded-[4px] border border-input data-[state=checked]:border-primary data-[state=checked]:bg-primary data-[state=checked]:text-primary-foreground"
										>
											{#if enabled}
												<Check class="size-3.5" />
											{/if}
										</span>

										<span class="min-w-0 flex-1 truncate font-mono text-[12px]">
											{entry.definition.function.name}
										</span>
									</button>
								{/each}
							</div>
						</Collapsible.Content>
					</Collapsible.Root>
				{/each}
			</div>
		{/if}
	</DropdownMenu.SubContent>
</DropdownMenu.Sub>
