/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 * Copyright (c) 2015 Runtime Inc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "settings_test.h"
#include "settings/settings_file.h"
#include <zephyr/fs/fs.h>

ZTEST(settings_config_fs, test_config_empty_file)
{
	int rc;
	struct settings_file cf_mfg;
	struct settings_file cf_running;
	const char cf_mfg_test[] = "";
	const char cf_running_test[] = "\n\n";

	config_wipe_srcs();

	cf_mfg.cf_name = TEST_CONFIG_DIR"/mfg";
	cf_running.cf_name = TEST_CONFIG_DIR"/running";

	rc = settings_file_src(&cf_mfg);
	zassert_true(rc == 0, "can't register FS as configuration source");
	rc = settings_file_src(&cf_running);
	zassert_true(rc == 0, "can't register FS as configuration source");

	settings_mount_file_backend(&cf_mfg);
	/*
	 * No files
	 */
	settings_load();

	rc = fs_mkdir(TEST_CONFIG_DIR);
	zassert_true(rc == 0 || rc == -EEXIST, "can't create directory");

	rc = fsutil_write_file(TEST_CONFIG_DIR"/mfg", cf_mfg_test,
			       0);
	zassert_true(rc == 0, "can't write to file");

	rc = fsutil_write_file(TEST_CONFIG_DIR"/running", cf_running_test,
			       sizeof(cf_running_test));
	zassert_true(rc == 0, "can't write to file");

	settings_load();
	config_wipe_srcs();
	ctest_clear_call_state();
}

ZTEST(settings_config_fs, test_truncated_file_errors)
{
	struct settings_file source = {.cf_name = TEST_CONFIG_DIR "/truncated"};
	const uint8_t partial_header = 7;
	uint8_t value[4];

	config_wipe_srcs();
	zassert_ok(settings_file_src(&source));
	zassert_ok(settings_file_dst(&source));
	settings_mount_file_backend(&source);
	int rc = fs_mkdir(TEST_CONFIG_DIR);

	zassert_true(rc == 0 || rc == -EEXIST);
	zassert_ok(fsutil_write_file(source.cf_name, &partial_header, 1));
	zassert_equal(settings_load_one("missing", value, sizeof(value)), -ESPIPE);
	zassert_ok(fs_unlink(source.cf_name));
	zassert_ok(settings_save_one("value", "1234", 4));
	struct fs_dirent entry;
	struct fs_file_t file;

	zassert_ok(fs_stat(source.cf_name, &entry));
	fs_file_t_init(&file);
	zassert_ok(fs_open(&file, source.cf_name, FS_O_WRITE));
	zassert_ok(fs_truncate(&file, entry.size - 2));
	zassert_ok(fs_close(&file));
	zassert_equal(settings_load_one("value", value, sizeof(value)), -ESPIPE);
	config_wipe_srcs();
	ctest_clear_call_state();
}
