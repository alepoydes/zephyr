/*
 * Copyright (c) 2022, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/sys/mem_stats.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/ztest.h>

#define TIMEOUT_MS 500

#define POOL_SIZE 28672

#ifdef CONFIG_USERSPACE
#define STACK_OBJ_SIZE K_THREAD_STACK_LEN(CONFIG_DYNAMIC_THREAD_STACK_SIZE)
#else
#define STACK_OBJ_SIZE K_KERNEL_STACK_LEN(CONFIG_DYNAMIC_THREAD_STACK_SIZE)
#endif

#define MAX_HEAP_STACKS (POOL_SIZE / STACK_OBJ_SIZE)

K_HEAP_DEFINE(stack_heap, POOL_SIZE);

#if defined(CONFIG_ARM_MPU) && defined(CONFIG_HW_STACK_PROTECTION) && \
	defined(CONFIG_MPU_STACK_GUARD) && defined(CONFIG_USERSPACE)
#define GUARDED_SMALL_SIZE 2048
#define GUARDED_LARGE_SIZE 12288
K_HEAP_DEFINE(guarded_stack_heap, 65536);
K_SEM_DEFINE(guarded_ready, 0, 2);
K_SEM_DEFINE(guarded_gate, 0, 2);

ZTEST_DMEM static struct guarded_case {
	char *priv;
	char *metadata;
	bool checked;
} guarded_cases[2];

static void guarded_entry(void *arg1, void *arg2, void *arg3)
{
	struct guarded_case *test_case = arg1;
	char own_buffer[CONFIG_THREAD_MAX_NAME_LEN];

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/* These calls also exercise the privileged SVC stack before the MPU checks. */
	k_sem_give(&guarded_ready);
	zassert_ok(k_sem_take(&guarded_gate, K_FOREVER));
	zassert_ok(k_thread_name_copy(k_current_get(), own_buffer, sizeof(own_buffer)));
	zassert_equal(k_thread_name_copy(k_current_get(), test_case->priv, 1), -EFAULT);
	zassert_equal(k_thread_name_copy(k_current_get(), test_case->metadata, 1), -EFAULT);
	test_case->checked = true;
}

static void oversized_child_entry(void *arg1, void *arg2, void *arg3)
{
	k_thread_create(arg1, arg2, K_THREAD_STACK_LEN(GUARDED_LARGE_SIZE) + 1,
			guarded_entry, NULL, NULL, NULL, 0, K_USER, K_NO_WAIT);
	zassert_unreachable("oversized child stack was accepted");
}

static void metadata_probe_entry(void *arg1, void *arg2, void *arg3)
{
	volatile uint8_t byte = *(volatile uint8_t *)arg1;

	ARG_UNUSED(byte);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);
	zassert_unreachable("user read privileged stack metadata");
}
#endif

ZTEST_DMEM bool tflag[MAX(CONFIG_DYNAMIC_THREAD_POOL_SIZE, MAX_HEAP_STACKS)];

static void func(void *arg1, void *arg2, void *arg3)
{
	bool *flag = (bool *)arg1;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	printk("Hello, dynamic world!\n");

	*flag = true;
}

/** @brief Check we can create a thread from userspace, using dynamic objects */
ZTEST_USER(dynamic_thread_stack, test_dynamic_thread_stack_userspace_dyn_obj)
{
	k_tid_t tid;
	struct k_thread *th;
	k_thread_stack_t *stack;

	if (!IS_ENABLED(CONFIG_USERSPACE)) {
		ztest_test_skip();
	}

	if (!IS_ENABLED(CONFIG_DYNAMIC_THREAD_PREFER_ALLOC)) {
		ztest_test_skip();
	}

	if (!IS_ENABLED(CONFIG_DYNAMIC_THREAD_ALLOC)) {
		ztest_test_skip();
	}

	stack = k_thread_stack_alloc(CONFIG_DYNAMIC_THREAD_STACK_SIZE, K_USER);
	zassert_not_null(stack);

	th = k_object_alloc(K_OBJ_THREAD);
	zassert_not_null(th);

	tid = k_thread_create(th, stack, CONFIG_DYNAMIC_THREAD_STACK_SIZE, func,
			      &tflag[0], NULL, NULL, 0,
			      K_USER | K_INHERIT_PERMS, K_NO_WAIT);

	zassert_not_null(tid);

	zassert_ok(k_thread_join(tid, K_MSEC(TIMEOUT_MS)));
	zassert_true(tflag[0]);
	zassert_ok(k_thread_stack_free(stack));
}

/** @brief Exercise the pool-based thread stack allocator */
ZTEST(dynamic_thread_stack, test_dynamic_thread_stack_pool)
{
	static k_tid_t tid[CONFIG_DYNAMIC_THREAD_POOL_SIZE];
	static struct k_thread th[CONFIG_DYNAMIC_THREAD_POOL_SIZE];
	static k_thread_stack_t *stack[CONFIG_DYNAMIC_THREAD_POOL_SIZE];

	if (!IS_ENABLED(CONFIG_DYNAMIC_THREAD_PREFER_POOL)) {
		ztest_test_skip();
	}

	/* allocate all thread stacks from the pool */
	for (size_t i = 0; i < CONFIG_DYNAMIC_THREAD_POOL_SIZE; ++i) {
		stack[i] = k_thread_stack_alloc(CONFIG_DYNAMIC_THREAD_STACK_SIZE,
						IS_ENABLED(CONFIG_USERSPACE) ? K_USER : 0);

		zassert_not_null(stack[i]);
	}

	if (IS_ENABLED(CONFIG_DYNAMIC_THREAD_ALLOC)) {
		/* ensure 1 thread can be allocated from the heap when the pool is depleted */
		zassert_ok(k_thread_stack_free(
			k_thread_stack_alloc(CONFIG_DYNAMIC_THREAD_STACK_SIZE,
					     IS_ENABLED(CONFIG_USERSPACE) ? K_USER : 0)));
	} else {
		/* ensure that no more thread stacks can be allocated from the pool */
		zassert_is_null(k_thread_stack_alloc(CONFIG_DYNAMIC_THREAD_STACK_SIZE,
						     IS_ENABLED(CONFIG_USERSPACE) ? K_USER : 0));
	}

	/* spawn our threads */
	for (size_t i = 0; i < CONFIG_DYNAMIC_THREAD_POOL_SIZE; ++i) {
		tflag[i] = false;
		tid[i] = k_thread_create(&th[i], stack[i],
				CONFIG_DYNAMIC_THREAD_STACK_SIZE, func,
				&tflag[i], NULL, NULL, 0,
				K_USER | K_INHERIT_PERMS, K_NO_WAIT);
	}

	/* join all threads and check that flags have been set */
	for (size_t i = 0; i < CONFIG_DYNAMIC_THREAD_POOL_SIZE; ++i) {
		zassert_ok(k_thread_join(tid[i], K_MSEC(TIMEOUT_MS)));
		zassert_true(tflag[i]);
	}

	/* clean up stacks allocated from the pool */
	for (size_t i = 0; i < CONFIG_DYNAMIC_THREAD_POOL_SIZE; ++i) {
		zassert_ok(k_thread_stack_free(stack[i]));
	}
}

/** @brief Exercise the heap-based thread stack allocator */
ZTEST(dynamic_thread_stack, test_dynamic_thread_stack_alloc)
{
	size_t N;
	static k_tid_t tid[MAX_HEAP_STACKS];
	static struct k_thread th[MAX_HEAP_STACKS];
	static k_thread_stack_t *stack[MAX_HEAP_STACKS];

	if (!IS_ENABLED(CONFIG_DYNAMIC_THREAD_PREFER_ALLOC)) {
		ztest_test_skip();
	}

	if (!IS_ENABLED(CONFIG_DYNAMIC_THREAD_ALLOC)) {
		ztest_test_skip();
	}

	/* allocate all thread stacks from the heap */
	for (N = 0; N < MAX_HEAP_STACKS; ++N) {
		stack[N] = k_thread_stack_alloc(CONFIG_DYNAMIC_THREAD_STACK_SIZE,
						IS_ENABLED(CONFIG_USERSPACE) ? K_USER : 0);
		if (stack[N] == NULL) {
			break;
		}
	}

	/* spwan our threads */
	for (size_t i = 0; i < N; ++i) {
		tflag[i] = false;
		tid[i] = k_thread_create(&th[i], stack[i],
					 CONFIG_DYNAMIC_THREAD_STACK_SIZE, func,
					 &tflag[i], NULL, NULL, 0,
					 K_USER | K_INHERIT_PERMS, K_NO_WAIT);
	}

	/* join all threads and check that flags have been set */
	for (size_t i = 0; i < N; ++i) {
		zassert_ok(k_thread_join(tid[i], K_MSEC(TIMEOUT_MS)));
		zassert_true(tflag[i]);
	}

	/* clean up stacks allocated from the heap */
	for (size_t i = 0; i < N; ++i) {
		zassert_ok(k_thread_stack_free(stack[i]));
	}
}

K_SEM_DEFINE(perm_sem, 0, 1);
ZTEST_BMEM static volatile bool expect_fault;
ZTEST_BMEM static volatile unsigned int expected_reason;

static void set_fault(unsigned int reason)
{
	expect_fault = true;
	expected_reason = reason;
	compiler_barrier();
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *pEsf)
{
	if (expect_fault) {
		if (expected_reason == reason) {
			printk("System error was expected\n");
			expect_fault = false;
		} else {
			printk("Wrong fault reason, expecting %d\n",
			       expected_reason);
			TC_END_REPORT(TC_FAIL);
			k_fatal_halt(reason);
		}
	} else {
		printk("Unexpected fault during test\n");
		TC_END_REPORT(TC_FAIL);
		k_fatal_halt(reason);
	}
}

static void perm_func(void *arg1, void *arg2, void *arg3)
{
	k_sem_take((struct k_sem *)arg1, K_FOREVER);
}

static void perm_func_violator(void *arg1, void *arg2, void *arg3)
{
	(void)k_thread_stack_free((k_thread_stack_t *)arg2);

	zassert_unreachable("should not reach here");
}

/** @brief Exercise stack permissions */
ZTEST(dynamic_thread_stack, test_dynamic_thread_stack_permission)
{
	static k_tid_t tid[2];
	static struct k_thread th[2];
	static k_thread_stack_t *stack[2];

	if (!IS_ENABLED(CONFIG_DYNAMIC_THREAD_PREFER_ALLOC)) {
		ztest_test_skip();
	}

	if (!IS_ENABLED(CONFIG_DYNAMIC_THREAD_ALLOC)) {
		ztest_test_skip();
	}

	if (!IS_ENABLED(CONFIG_USERSPACE)) {
		ztest_test_skip();
	}

	stack[0] = k_thread_stack_alloc(CONFIG_DYNAMIC_THREAD_STACK_SIZE, K_USER);
	zassert_not_null(stack[0]);

	stack[1] = k_thread_stack_alloc(CONFIG_DYNAMIC_THREAD_STACK_SIZE, K_USER);
	zassert_not_null(stack[1]);

	k_thread_access_grant(k_current_get(), &perm_sem);

	/* First thread inherit permissions */
	tid[0] = k_thread_create(&th[0], stack[0], CONFIG_DYNAMIC_THREAD_STACK_SIZE, perm_func,
				 &perm_sem, NULL, NULL, 0, K_USER | K_INHERIT_PERMS, K_NO_WAIT);
	zassert_not_null(tid[0]);

	/* Second thread will have access to specific kobjects only */
	tid[1] = k_thread_create(&th[1], stack[1], CONFIG_DYNAMIC_THREAD_STACK_SIZE,
				 perm_func_violator, &perm_sem, stack[0], NULL, 0, K_USER,
				 K_FOREVER);
	zassert_not_null(tid[1]);
	k_thread_access_grant(tid[1], &perm_sem);
	k_thread_access_grant(tid[1], &stack[1]);

	set_fault(K_ERR_KERNEL_OOPS);

	k_thread_start(tid[1]);

	/* join all threads and check that flags have been set */
	zassert_ok(k_thread_join(tid[1], K_MSEC(TIMEOUT_MS)));

	k_sem_give(&perm_sem);
	zassert_ok(k_thread_join(tid[0], K_MSEC(TIMEOUT_MS)));

	/* clean up stacks allocated from the heap */
	zassert_ok(k_thread_stack_free(stack[0]));
	zassert_ok(k_thread_stack_free(stack[1]));
}

#if defined(CONFIG_ARM_MPU) && defined(CONFIG_HW_STACK_PROTECTION) && \
	defined(CONFIG_MPU_STACK_GUARD) && defined(CONFIG_USERSPACE)
ZTEST(dynamic_thread_stack, test_arm_guarded_heap_user_stacks)
{
	static struct k_thread threads[3];
	static struct k_thread oversized_child;
	k_thread_stack_t *stacks[2];
	struct sys_memory_stats before, after;
	const size_t sizes[2] = {GUARDED_SMALL_SIZE, GUARDED_LARGE_SIZE};

	k_thread_heap_assign(k_current_get(), &guarded_stack_heap);
	zassert_ok(sys_heap_runtime_stats_get(&guarded_stack_heap.heap, &before));
	zassert_is_null(k_thread_stack_alloc(0, K_USER));
	zassert_is_null(k_thread_stack_alloc(SIZE_MAX, K_USER));
	k_thread_access_grant(k_current_get(), &guarded_ready, &guarded_gate);

	for (size_t i = 0; i < 2; i++) {
		const struct k_object *obj;

		stacks[i] = k_thread_stack_alloc(sizes[i], K_USER);
		zassert_not_null(stacks[i]);
		zassert_equal((uintptr_t)stacks[i] % K_THREAD_STACK_LEN(sizes[i]), 0);
		obj = k_object_find(stacks[i]);
		zassert_not_null(obj);
		guarded_cases[i].priv = (char *)obj->data.stack_data->priv;
		guarded_cases[i].metadata = (char *)obj->data.stack_data;
		guarded_cases[i].checked = false;
		k_thread_create(&threads[i], stacks[i], sizes[i], guarded_entry,
				&guarded_cases[i], NULL, NULL, 0,
				K_USER | K_INHERIT_PERMS, K_FOREVER);
	}

	/* A suspended thread owns its stack even before its first instruction. */
	zassert_equal(k_thread_stack_free(stacks[0]), -EBUSY);
	k_thread_start(&threads[0]);
	k_thread_start(&threads[1]);
	zassert_ok(k_sem_take(&guarded_ready, K_MSEC(TIMEOUT_MS)));
	zassert_ok(k_sem_take(&guarded_ready, K_MSEC(TIMEOUT_MS)));
	k_sem_give(&guarded_gate);
	k_sem_give(&guarded_gate);
	for (size_t i = 0; i < 2; i++) {
		zassert_ok(k_thread_join(&threads[i], K_MSEC(TIMEOUT_MS)));
		zassert_true(guarded_cases[i].checked);
	}

	/* The user MPU must fault on a real load, not just deny a syscall buffer. */
	k_thread_create(&threads[0], stacks[0], GUARDED_SMALL_SIZE, metadata_probe_entry,
			guarded_cases[0].metadata, NULL, NULL, 0, K_USER, K_FOREVER);
	set_fault(K_ERR_CPU_EXCEPTION);
	k_thread_start(&threads[0]);
	zassert_ok(k_thread_join(&threads[0], K_MSEC(TIMEOUT_MS)));
	zassert_false(expect_fault);
	zassert_ok(k_thread_stack_free(stacks[0]));
	zassert_ok(k_thread_stack_free(stacks[1]));

	/* A user-created child must not consume the adjacent privileged storage. */
	stacks[0] = k_thread_stack_alloc(GUARDED_SMALL_SIZE, K_USER);
	stacks[1] = k_thread_stack_alloc(GUARDED_LARGE_SIZE, K_USER);
	zassert_not_null(stacks[0]);
	zassert_not_null(stacks[1]);
	k_thread_create(&threads[2], stacks[0], GUARDED_SMALL_SIZE,
			oversized_child_entry, &oversized_child, stacks[1], NULL, 0,
			K_USER, K_FOREVER);
	k_thread_access_grant(&threads[2], &oversized_child, stacks[1]);
	set_fault(K_ERR_KERNEL_OOPS);
	k_thread_start(&threads[2]);
	zassert_ok(k_thread_join(&threads[2], K_MSEC(TIMEOUT_MS)));
	zassert_false(expect_fault);
	zassert_ok(k_thread_stack_free(stacks[0]));
	zassert_ok(k_thread_stack_free(stacks[1]));
	zassert_ok(sys_heap_runtime_stats_get(&guarded_stack_heap.heap, &after));
	zassert_equal(after.free_bytes, before.free_bytes);
	k_thread_heap_assign(k_current_get(), &stack_heap);
}
#endif

static void *dynamic_thread_stack_setup(void)
{
	k_thread_heap_assign(k_current_get(), &stack_heap);
	return NULL;
}

ZTEST_SUITE(dynamic_thread_stack, NULL, dynamic_thread_stack_setup, NULL, NULL, NULL);
