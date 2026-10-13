/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/bluetooth/addr.h>
#include <zephyr/ztest.h>

#include <host/keys.h>

ZTEST(bt_keys_br_eviction, test_recently_used_bond_survives_ram_only_eviction)
{
	static const bt_addr_t a = { .val = { 1, 0, 0, 0, 0, 0 } };
	static const bt_addr_t b = { .val = { 2, 0, 0, 0, 0, 0 } };
	static const bt_addr_t c = { .val = { 3, 0, 0, 0, 0, 0 } };

	zassert_not_null(bt_keys_get_link_key(&a));
	zassert_not_null(bt_keys_get_link_key(&b));

	bt_keys_link_key_update_usage(&a);

	zassert_not_null(bt_keys_get_link_key(&c));
	zassert_not_null(bt_keys_find_link_key(&a));
	zassert_is_null(bt_keys_find_link_key(&b));
	zassert_not_null(bt_keys_find_link_key(&c));
}

ZTEST_SUITE(bt_keys_br_eviction, NULL, NULL, NULL, NULL, NULL);
