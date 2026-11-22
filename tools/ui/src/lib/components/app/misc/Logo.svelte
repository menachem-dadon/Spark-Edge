<script>
	let { class: className = '', style = '' } = $props();
</script>

<div class={className} {style}>
	<img src="spark-edge-static.svg" alt="Spark Edge" />
</div>

<style>
	img {
		width: var(--size, 1rem);
		height: var(--size, 1rem);
		border-radius: 24%;
		object-fit: cover;
	}
</style>
