/*
 * Copyright (c) 2026 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_LLEXT_KHEAP_H_
#define ZEPHYR_SUBSYS_LLEXT_KHEAP_H_

#include <zephyr/kernel.h>
#include <zephyr/llext/llext.h>

#ifdef CONFIG_HARVARD
extern struct k_heap llext_instr_heap;
extern struct k_heap llext_data_heap;
#define llext_metadata_heap llext_data_heap
#else
extern struct k_heap llext_heap;
#define llext_instr_heap    llext_heap
#define llext_data_heap     llext_heap
#define llext_metadata_heap llext_heap
#endif /* CONFIG_HARVARD */

static inline bool llext_heap_is_inited(void)
{
#ifdef CONFIG_LLEXT_HEAP_DYNAMIC
	extern bool llext_heap_inited;

	return llext_heap_inited;
#else
	return true;
#endif
}

static inline void *llext_alloc_metadata(struct k_heap *heap, size_t bytes)
{
	if (bytes != 0 && (heap || llext_heap_is_inited())) {
		return k_heap_alloc(heap ? heap : &llext_metadata_heap, bytes, K_NO_WAIT);
	}

	return NULL;
}

static inline void *llext_aligned_alloc_data(struct llext *ext, size_t align, size_t bytes)
{
	if (bytes != 0 && (ext->heap || llext_heap_is_inited())) {
		return k_heap_aligned_alloc(ext->heap ? ext->heap : &llext_data_heap,
					    align, bytes, K_NO_WAIT);
	}

	return NULL;
}

static inline void *llext_aligned_alloc_instr(struct llext *ext, size_t align, size_t bytes)
{
	if (bytes != 0 && (ext->heap || llext_heap_is_inited())) {
		return k_heap_aligned_alloc(ext->heap ? ext->heap : &llext_instr_heap,
					    align, bytes, K_NO_WAIT);
	}

	return NULL;
}

static inline void llext_free_metadata(struct k_heap *heap, void *ptr)
{
	if (heap || llext_heap_is_inited()) {
		k_heap_free(heap ? heap : &llext_metadata_heap, ptr);
	}
}

static inline void llext_free_data(struct llext *ext, void *ptr)
{
	if (ext->heap || llext_heap_is_inited()) {
		k_heap_free(ext->heap ? ext->heap : &llext_data_heap, ptr);
	}
}

static inline void llext_free_instr(struct llext *ext, void *ptr)
{
	if (ext->heap || llext_heap_is_inited()) {
		k_heap_free(ext->heap ? ext->heap : &llext_instr_heap, ptr);
	}
}

static inline void llext_heap_reset(struct llext *ext)
{
	ARG_UNUSED(ext);
}

#endif /* ZEPHYR_SUBSYS_LLEXT_KHEAP_H_ */
