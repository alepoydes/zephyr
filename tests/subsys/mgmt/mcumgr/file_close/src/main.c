/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/mgmt/mcumgr/grp/fs_mgmt/fs_mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

/* A synthetic backend injects a close error through the public filesystem API. */
static unsigned int opened, closed, replies;
static int close_error;
static int mount_file(struct fs_mount_t *mount) { return 0; }
static int open_file(struct fs_file_t *file, const char *path, fs_mode_t flags)
{
	opened++;
	return 0;
}
static int close_file(struct fs_file_t *file)
{
	closed++;
	return close_error;
}
static ssize_t write_file(struct fs_file_t *file, const void *data, size_t length)
{
	return length;
}
static off_t tell_file(struct fs_file_t *file) { return 0; }
static int stat_file(struct fs_mount_t *mount, const char *path, struct fs_dirent *entry)
{
	entry->type = FS_DIR_ENTRY_FILE;
	entry->size = 0;
	return 0;
}
static const struct fs_file_system_t backend = {
	.mount = mount_file, .open = open_file, .close = close_file,
	.write = write_file, .tell = tell_file, .stat = stat_file,
};
static struct fs_mount_t mount = { .type = FS_TYPE_EXTERNAL_BASE, .mnt_point = "/test" };
static int output(struct net_buf *reply)
{
	replies++;
	smp_packet_free(reply);
	return 0;
}
static struct smp_transport transport = { .functions = { .output = output } };

/* SMP v2 group 8: upload one of two bytes to /test/a, leaving the file open. */
static const uint8_t upload[] =
	"\x0a\0\0\x1f\0\x08\0\0"
	"\xa4\x64name\x67/test/a\x63off\0\x63len\x02\x64" "data\x41\x01";
static const uint8_t close_request[] = {0x0a, 0, 0, 1, 0, 8, 0, 4, 0xa0};

static int process(const uint8_t *data, size_t length)
{
	struct cbor_nb_reader reader;
	struct cbor_nb_writer writer;
	struct smp_streamer stream = {
		.smpt = &transport, .reader = &reader, .writer = &writer,
	};
	struct net_buf *request = smp_packet_alloc();

	zassert_not_null(request);
	net_buf_add_mem(request, data, length);
	return smp_process_request_packet(&stream, request);
}

ZTEST(file_close, test_close_and_error_recovery)
{
	zassert_ok(fs_register(FS_TYPE_EXTERNAL_BASE, &backend));
	zassert_ok(fs_mount(&mount));
	zassert_ok(fs_mgmt_file_close());
	zassert_equal(closed, 0);
	zassert_ok(process(upload, sizeof(upload) - 1));
	zassert_equal(opened, 1);
	zassert_equal(closed, 0);
	zassert_ok(fs_mgmt_file_close());
	zassert_equal(closed, 1);
	zassert_ok(fs_mgmt_file_close());
	zassert_equal(closed, 1);

	zassert_ok(process(upload, sizeof(upload) - 1));
	close_error = -EIO;
	zassert_equal(fs_mgmt_file_close(), -EIO);
	zassert_equal(closed, 2);
	zassert_ok(process(upload, sizeof(upload) - 1));
	/* A close failure is a group error response, not an encoding error. */
	zassert_ok(process(close_request, sizeof(close_request)));
	zassert_equal(closed, 3);
	close_error = 0;
	zassert_ok(process(upload, sizeof(upload) - 1));
	zassert_ok(process(close_request, sizeof(close_request)));
	zassert_equal(opened, 4);
	zassert_equal(closed, 4);
	zassert_equal(replies, 6);
}
ZTEST_SUITE(file_close, NULL, NULL, NULL, NULL, NULL);
