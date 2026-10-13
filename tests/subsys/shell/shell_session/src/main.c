/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A session reset must leave one shell thread reusable without carrying input
 * or configuration from the previous transport owner into the next session.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

SHELL_DUMMY_DEFINE(session_transport);
SHELL_DEFINE(session_shell, "session> ", &session_transport, 0, 0,
	     SHELL_FLAG_OLF_CRLF);

static shell_transport_handler_t transport_handler;
static void *transport_context;
static atomic_t bytes_read;
static atomic_t hold_completed;
static atomic_t secret_count;
static atomic_t ok_count;
static atomic_t bypass_count;
K_SEM_DEFINE(read_ready, 0, 128);
K_SEM_DEFINE(hold_started, 0, 1);
K_SEM_DEFINE(hold_release, 0, 1);
K_SEM_DEFINE(reset_entered, 0, 1);
K_SEM_DEFINE(reset_finished, 0, 1);
K_SEM_DEFINE(processed, 0, 1);
K_SEM_DEFINE(secret_done, 0, 1);
K_SEM_DEFINE(ok_done, 0, 1);
K_SEM_DEFINE(bypass_started, 0, 1);
K_SEM_DEFINE(bypass_release, 0, 1);
static const struct shell_backend_config_flags defaults =
	SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;

static int transport_init(const struct shell_transport *transport, const void *config,
			  shell_transport_handler_t handler, void *context)
{
	int err = shell_dummy_transport_api.init(transport, config, handler, context);

	if (!err) {
		transport_handler = handler;
		transport_context = context;
	}
	return err;
}

static int transport_read(const struct shell_transport *transport, void *data,
			  size_t length, size_t *count)
{
	int err = shell_dummy_transport_api.read(transport, data, length, count);

	if (!err && *count) {
		atomic_add(&bytes_read, *count);
		k_sem_give(&read_ready);
	}
	return err;
}

static int transport_uninit(const struct shell_transport *transport)
{
	return shell_dummy_transport_api.uninit(transport);
}

static int transport_enable(const struct shell_transport *transport, bool blocking)
{
	return shell_dummy_transport_api.enable(transport, blocking);
}

static int transport_write(const struct shell_transport *transport, const void *data,
			   size_t length, size_t *count)
{
	return shell_dummy_transport_api.write(transport, data, length, count);
}

static void transport_update(const struct shell_transport *transport)
{
	k_sem_give(&processed);
}

static const struct shell_transport_api session_api = {
	.init = transport_init,
	.uninit = transport_uninit,
	.enable = transport_enable,
	.write = transport_write,
	.read = transport_read,
	.update = transport_update,
};

static void input(const char *data, size_t size)
{
	int target = atomic_get(&bytes_read) + size;

	zassert_ok(shell_backend_dummy_push_input(&session_shell, data, size));
	transport_handler(SHELL_TRANSPORT_EVT_RX_RDY, transport_context);
	while (atomic_get(&bytes_read) < target) {
		zassert_ok(k_sem_take(&read_ready, K_SECONDS(1)));
	}
}

static int hold(const struct shell *sh, size_t argc, char **argv)
{
	k_sem_give(&hold_started);
	(void)k_sem_take(&hold_release, K_FOREVER);
	atomic_set(&hold_completed, 1);
	return 0;
}
SHELL_CMD_REGISTER(session_hold, NULL, NULL, hold);

static int secret(const struct shell *sh, size_t argc, char **argv)
{
	atomic_inc(&secret_count);
	k_sem_give(&secret_done);
	return 0;
}
SHELL_CMD_REGISTER(session_secret, NULL, NULL, secret);

static int ok(const struct shell *sh, size_t argc, char **argv)
{
	atomic_inc(&ok_count);
	k_sem_give(&ok_done);
	return 0;
}
SHELL_CMD_REGISTER(session_ok, NULL, NULL, ok);

static void bypass(const struct shell *sh, uint8_t *data, size_t length, void *context)
{
	atomic_inc(&bypass_count);
	k_sem_give(&bypass_started);
	(void)k_sem_take(&bypass_release, K_FOREVER);
}

K_THREAD_STACK_DEFINE(reset_stack, 1024);
static struct k_thread reset_thread;
static int reset_error;

static void reset_while_command_runs(void *a, void *b, void *c)
{
	k_sem_give(&reset_entered);
	reset_error = shell_reset(&session_shell, defaults);
	k_sem_give(&reset_finished);
}

ZTEST(shell_session, test_reset_keeps_thread_and_isolates_sessions)
{
	const struct shell *sh = &session_shell;
	session_transport.api = &session_api;
	zassert_ok(shell_init(sh, NULL, defaults, false, 0));
	k_tid_t original_thread = sh->ctx->tid;
	zassert_not_null(original_thread);
	zassert_ok(shell_start(sh));

	input("session_hold\n", sizeof("session_hold\n") - 1);
	zassert_ok(k_sem_take(&hold_started, K_SECONDS(1)));
	k_thread_create(&reset_thread, reset_stack, K_THREAD_STACK_SIZEOF(reset_stack),
			reset_while_command_runs, NULL, NULL, NULL,
			K_PRIO_COOP(0), 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&reset_entered, K_SECONDS(1)));
	zassert_equal(k_sem_take(&reset_finished, K_NO_WAIT), -EBUSY,
		      "reset completed before the active command");
	k_sem_give(&hold_release);
	zassert_ok(k_sem_take(&reset_finished, K_SECONDS(1)));
	zassert_ok(k_thread_join(&reset_thread, K_SECONDS(1)));
	zassert_ok(reset_error);
	zassert_equal(atomic_get(&hold_completed), 1);
	zassert_equal_ptr(sh->ctx->tid, original_thread);
	zassert_false(shell_ready(sh));
	zassert_ok(shell_start(sh));
	k_sem_reset(&processed);
	input("session_secret\n", sizeof("session_secret\n") - 1);
	zassert_ok(k_sem_take(&secret_done, K_SECONDS(1)));
	zassert_ok(k_sem_take(&processed, K_SECONDS(1)));
	input("session_\x1b[", sizeof("session_\x1b[") - 1);
	zassert_ok(k_sem_take(&processed, K_SECONDS(1)));
	shell_set_bypass(sh, bypass, NULL);
	zassert_equal(shell_echo_set(sh, false), 1);
	zassert_equal(shell_obscure_set(sh, true), 0);
	input("x", 1);
	zassert_ok(k_sem_take(&bypass_started, K_SECONDS(1)));
	k_thread_create(&reset_thread, reset_stack, K_THREAD_STACK_SIZEOF(reset_stack),
			reset_while_command_runs, NULL, NULL, NULL,
			K_PRIO_COOP(0), 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&reset_entered, K_SECONDS(1)));
	zassert_equal(k_sem_take(&reset_finished, K_NO_WAIT), -EBUSY,
		      "reset completed before the active bypass callback");
	k_sem_give(&bypass_release);
	zassert_ok(k_sem_take(&reset_finished, K_SECONDS(1)));
	zassert_ok(k_thread_join(&reset_thread, K_SECONDS(1)));
	zassert_ok(reset_error);
	zassert_equal_ptr(sh->ctx->tid, original_thread);
	zassert_false(shell_ready(sh));
	zassert_equal(shell_echo_set(sh, false), 1);
	zassert_equal(shell_obscure_set(sh, true), 0);
	zassert_ok(shell_echo_set(sh, true));
	zassert_equal(shell_obscure_set(sh, false), 1);

	zassert_ok(shell_start(sh));
	k_sem_reset(&processed);
	input("\x1b[A\n", sizeof("\x1b[A\n") - 1);
	zassert_ok(k_sem_take(&processed, K_SECONDS(1)));
	zassert_equal(atomic_get(&secret_count), 1,
		      "previous session's command history was replayed");
	input("session_ok\n", sizeof("session_ok\n") - 1);
	zassert_ok(k_sem_take(&ok_done, K_SECONDS(1)));
	zassert_equal(atomic_get(&bypass_count), 1);
	zassert_equal_ptr(sh->ctx->tid, original_thread);
}

ZTEST_SUITE(shell_session, NULL, NULL, NULL, NULL, NULL);
