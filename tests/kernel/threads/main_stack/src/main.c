/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/sys/printk.h>

#define USER_STACK_SIZE 1024

static K_SEM_DEFINE(probe_sem, 0, 1);
static K_THREAD_STACK_DEFINE(user_stack, USER_STACK_SIZE);
static struct k_thread user_thread;

static void user_probe(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	if (!k_is_user_context()) {
		printk("main_stack: FAIL: user thread stayed privileged\n");
		return;
	}

	k_sem_give(&probe_sem);
}

static void main_user_probe(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	if (!k_is_user_context()) {
		printk("main_stack: FAIL: main stayed privileged\n");
		return;
	}

	k_sem_give(&probe_sem);
	if (k_sem_take(&probe_sem, K_NO_WAIT) != 0) {
		printk("main_stack: FAIL: user syscall failed\n");
		return;
	}

	printk("main_stack: PASS\n");
}

int main(void)
{
	k_tid_t tid;

	if (IS_ENABLED(CONFIG_MAIN_THREAD_USERSPACE) ==
	    IS_ENABLED(TEST_MAIN_THREAD_USERSPACE_DISABLED)) {
		printk("main_stack: FAIL: unexpected configuration\n");
		return 1;
	}
	if ((k_object_find(k_current_get()->stack_obj) != NULL) !=
	    IS_ENABLED(CONFIG_MAIN_THREAD_USERSPACE)) {
		printk("main_stack: FAIL: incorrect main stack capability\n");
		return 1;
	}

	if (IS_ENABLED(CONFIG_MAIN_THREAD_USERSPACE)) {
		k_object_access_grant(&probe_sem, k_current_get());
		k_thread_user_mode_enter(main_user_probe, NULL, NULL, NULL);
	}

	if (k_is_user_context()) {
		printk("main_stack: FAIL: main entered user mode\n");
		return 1;
	}

	tid = k_thread_create(&user_thread, user_stack, USER_STACK_SIZE,
			      user_probe, NULL, NULL, NULL,
			      K_PRIO_PREEMPT(0), K_USER, K_FOREVER);
	k_object_access_grant(&probe_sem, tid);
	k_thread_start(tid);
	if (k_thread_join(tid, K_SECONDS(1)) != 0 ||
	    k_sem_take(&probe_sem, K_NO_WAIT) != 0) {
		printk("main_stack: FAIL: user thread syscall failed\n");
		return 1;
	}

	printk("main_stack: PASS\n");
	return 0;
}
