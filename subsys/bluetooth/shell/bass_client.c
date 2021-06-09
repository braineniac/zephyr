/**
 * @file
 * @brief Shell APIs for Bluetooth BASS
 *
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>

#include <zephyr.h>
#include <zephyr/types.h>
#include <shell/shell.h>
#include <stdlib.h>
#include <bluetooth/gatt.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include "../host/audio/bass.h"
#include "../host/hci_core.h"
#include "bt.h"

static const char *phy2str(uint8_t phy)
{
	switch (phy) {
	case 0: return "No packets";
	case BT_GAP_LE_PHY_1M: return "LE 1M";
	case BT_GAP_LE_PHY_2M: return "LE 2M";
	case BT_GAP_LE_PHY_CODED: return "LE Coded";
	default: return "Unknown";
	}
}

static void bass_client_discover_cb(struct bt_conn *conn, int err,
				    uint8_t recv_state_count)
{
	if (err) {
		shell_error(ctx_shell, "BASS discover failed (%d)", err);
	} else {
		shell_print(ctx_shell, "BASS discover done with %u recv states",
			    recv_state_count);
	}

}

static void bass_client_scan_cb(const struct bt_le_scan_recv_info *info)
{
	char le_addr[BT_ADDR_LE_STR_LEN];
	char name[30];

	(void)memset(name, 0, sizeof(name));

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));
	shell_print(ctx_shell, "[DEVICE]: %s, AD evt type %u, RSSI %i %s "
		    "C:%u S:%u D:%d SR:%u E:%u Prim: %s, Secn: %s, "
		    "Interval: 0x%04x (%u ms), SID: 0x%x",
		    le_addr, info->adv_type, info->rssi, name,
		    (info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE) != 0,
		    (info->adv_props & BT_GAP_ADV_PROP_SCANNABLE) != 0,
		    (info->adv_props & BT_GAP_ADV_PROP_DIRECTED) != 0,
		    (info->adv_props & BT_GAP_ADV_PROP_SCAN_RESPONSE) != 0,
		    (info->adv_props & BT_GAP_ADV_PROP_EXT_ADV) != 0,
		    phy2str(info->primary_phy), phy2str(info->secondary_phy),
		    info->interval, info->interval * 5 / 4, info->sid);

}

static bool metadata_entry(struct bt_data *data, void *user_data)
{
	char metadata[512];

	bin2hex(data->data, data->data_len, metadata, sizeof(metadata));

	shell_print(ctx_shell, "\t\tMetadata length %u, type %u, data: %s",
		    data->data_len, data->type, metadata);

	return true;
}

static void bass_client_recv_state_cb(struct bt_conn *conn, int err,
				      const struct bt_bass_recv_state *state)
{
	char le_addr[BT_ADDR_LE_STR_LEN];
	char bad_code[33];

	if (err) {
		shell_error(ctx_shell, "BASS recv state read failed (%d)", err);
		return;
	}


	bt_addr_le_to_str(&state->addr, le_addr, sizeof(le_addr));
	bin2hex(state->bad_code, BASS_BROADCAST_CODE_SIZE, bad_code, sizeof(bad_code));
	shell_print(ctx_shell, "BASS recv state: src_id %u, addr %s, "
		    "sid %u, sync_state %u, encrypt_state %u%s%s",
		    state->src_id, le_addr, state->adv_sid,
		    state->pa_sync_state, state->encrypt_state,
		    state->encrypt_state == BASS_BIG_ENC_STATE_BAD_CODE ? ", bad code" : "",
		    bad_code);

	for (int i = 0; i < state->num_subgroups; i++) {
		const struct bt_bass_subgroup *subgroup = &state->subgroups[i];
		struct net_buf_simple buf;

		shell_print(ctx_shell, "\t[%d]: BIS sync %u, metadata_len %u",
			    i, subgroup->bis_sync, subgroup->metadata_len);

		net_buf_simple_init_with_data(&buf, (void *)subgroup->metadata,
					      subgroup->metadata_len);
		bt_data_parse(&buf, metadata_entry, NULL);
	}


	if (state->pa_sync_state == BASS_PA_STATE_INFO_REQ) {
		struct bt_le_per_adv_sync *per_adv_sync = NULL;
		struct bt_le_ext_adv *ext_adv = NULL;

		/* Lookup matching PA sync */
		for (int i = 0; i < ARRAY_SIZE(per_adv_syncs); i++) {
			if (per_adv_syncs[i] &&
			    !bt_addr_le_cmp(&per_adv_syncs[i]->addr,
					    &state->addr)) {
				per_adv_sync = per_adv_syncs[i];
				break;
			}
		}

		if (per_adv_sync) {
			shell_print(ctx_shell, "Sending PAST");

			err = bt_le_per_adv_sync_transfer(per_adv_sync,
							  conn,
							  BT_UUID_BASS_VAL);

			if (err) {
				shell_error(ctx_shell, "Could not transfer periodic adv sync: %d",
					    err);
			}

			return;
		}

		/* If no PA sync was found, check for local advertisers */
		for (int i = 0; i < ARRAY_SIZE(adv_sets); i++) {
			struct bt_le_oob oob_local;

			if (!adv_sets[i]) {
				continue;
			}

			err = bt_le_ext_adv_oob_get_local(adv_sets[i], &oob_local);

			if (err) {
				shell_error(ctx_shell, "Could not get local OOB %d", err);
				return;
			}

			if (!bt_addr_le_cmp(&oob_local.addr, &state->addr)) {
				ext_adv = adv_sets[i];
				break;
			}
		}

		if (ext_adv) {
			shell_print(ctx_shell, "Sending local PAST");

			err = bt_le_per_adv_set_info_transfer(ext_adv, conn, BT_UUID_BASS_VAL);

			if (err) {
				shell_error(ctx_shell, "Could not transfer per adv set info: %d",
					    err);
			}
		} else {
			shell_error(ctx_shell, "Could not send PA to BASS server");
		}
	}
}

static void bass_client_recv_state_removed_cb(
	struct bt_conn *conn, int err, const struct bt_bass_recv_state *state)
{
	if (err) {
		shell_error(ctx_shell, "BASS recv state removed failed (%d)",
			    err);
	} else {
		shell_print(ctx_shell, "BASS recv state %u removed",
			    state->src_id);
	}
}

static void bass_client_scan_start_cb(struct bt_conn *conn, int err)
{
	if (err) {
		shell_error(ctx_shell, "BASS scan start failed (%d)", err);
	} else {
		shell_print(ctx_shell, "BASS scan start successful");
	}
}

static void bass_client_scan_stop_cb(struct bt_conn *conn, int err)
{
	if (err) {
		shell_error(ctx_shell, "BASS scan stop failed (%d)", err);
	} else {
		shell_print(ctx_shell, "BASS scan stop successful");
	}
}

static void bass_client_add_src_cb(struct bt_conn *conn, int err)
{
	if (err) {
		shell_error(ctx_shell, "BASS add source failed (%d)", err);
	} else {
		shell_print(ctx_shell, "BASS add source successful");
	}
}

static void bass_client_mod_src_cb(struct bt_conn *conn, int err)
{
	if (err) {
		shell_error(ctx_shell, "BASS modify source failed (%d)", err);
	} else {
		shell_print(ctx_shell, "BASS modify source successful");
	}
}

static void bass_client_broadcast_code_cb(struct bt_conn *conn, int err)
{
	if (err) {
		shell_error(ctx_shell, "BASS broadcast code failed (%d)", err);
	} else {
		shell_print(ctx_shell, "BASS broadcast code successful");
	}
}

static void bass_client_rem_src_cb(struct bt_conn *conn, int err)
{
	if (err) {
		shell_error(ctx_shell, "BASS remove source failed (%d)", err);
	} else {
		shell_print(ctx_shell, "BASS remove source successful");
	}
}

static struct bt_bass_client_cb_t cbs = {
	.discover = bass_client_discover_cb,
	.scan = bass_client_scan_cb,
	.recv_state = bass_client_recv_state_cb,
	.recv_state_removed = bass_client_recv_state_removed_cb,
	.scan_start = bass_client_scan_start_cb,
	.scan_stop = bass_client_scan_stop_cb,
	.add_src = bass_client_add_src_cb,
	.mod_src = bass_client_mod_src_cb,
	.broadcast_code = bass_client_broadcast_code_cb,
	.rem_src = bass_client_rem_src_cb,
};

static int cmd_bass_client_discover(const struct shell *shell, size_t argc,
				    char **argv)
{
	int result;

	bt_bass_client_register_cb(&cbs);

	result = bt_bass_client_discover(default_conn);
	if (result) {
		shell_print(shell, "Fail: %d", result);
	}
	return result;
}

static int cmd_bass_client_scan_start(const struct shell *shell, size_t argc,
				      char **argv)
{
	int result;
	int start_scan = false;

	if (argc > 1) {
		start_scan = strtol(argv[1], NULL, 0);

		if (start_scan != 0 && start_scan != 1) {
			shell_error(shell, "Value shall be boolean");
			return -ENOEXEC;
		}
	}

	result = bt_bass_client_scan_start(default_conn, (bool)start_scan);
	if (result) {
		shell_print(shell, "Fail: %d", result);
	}
	return result;
}

static int cmd_bass_client_scan_stop(const struct shell *shell, size_t argc,
				     char **argv)
{
	int result;

	result = bt_bass_client_scan_stop(default_conn);
	if (result) {
		shell_print(shell, "Fail: %d", result);
	}
	return result;
}

static int cmd_bass_client_add_src(const struct shell *shell, size_t argc,
				   char **argv)
{
	int result;
	struct bt_bass_add_src_param param = { 0 };
	struct bt_bass_subgroup subgroup = { 0 };

	result = bt_addr_le_from_str(argv[1], argv[2], &param.addr);
	if (result) {
		shell_error(shell, "Invalid peer address (err %d)", result);
		return -ENOEXEC;
	}

	param.adv_sid = strtol(argv[3], NULL, 0);
	if (param.adv_sid < 0 || param.adv_sid > 0x0F) {
		shell_error(shell, "adv_sid shall be 0x00-0x0f");
		return -ENOEXEC;
	}

	param.pa_sync = strtol(argv[4], NULL, 0);
	if (param.pa_sync < 0 || param.pa_sync > 1) {
		shell_error(shell, "pa_sync shall be boolean");
		return -ENOEXEC;
	}

	/* TODO: Support multiple subgroups */
	if (argc > 5) {
		subgroup.bis_sync = strtoul(argv[5], NULL, 0);
		if (subgroup.bis_sync > UINT32_MAX) {
			shell_error(shell, "bis_sync shall be 0x00000000 "
				    "to 0xFFFFFFFF");
			return -ENOEXEC;
		}
	}

	if (argc > 6) {
		subgroup.metadata_len = hex2bin(argv[6], strlen(argv[6]),
						subgroup.metadata, sizeof(subgroup.metadata));

		if (!subgroup.metadata_len) {
			shell_error(shell, "Could not parse metadata");
			return -ENOEXEC;
		}
	}

	param.num_subgroups = 1;
	param.subgroups = &subgroup;

	result = bt_bass_client_add_src(default_conn, &param);
	if (result) {
		shell_print(shell, "Fail: %d", result);
	}
	return result;
}

static int cmd_bass_client_mod_src(const struct shell *shell, size_t argc,
				   char **argv)
{
	int result;
	struct bt_bass_mod_src_param param = { 0 };
	struct bt_bass_subgroup subgroup = { 0 };

	param.src_id = strtol(argv[1], NULL, 0);
	if (param.src_id < 0 || param.src_id > UINT8_MAX) {
		shell_error(shell, "adv_sid shall be 0x00-0xff");
		return -ENOEXEC;
	}

	result = bt_addr_from_str(argv[2], &param.addr);
	if (result) {
		shell_error(shell, "Invalid peer address (err %d)", result);
		return -ENOEXEC;
	}

	param.pa_sync = strtol(argv[3], NULL, 0);
	if (param.pa_sync < 0 || param.pa_sync > 1) {
		shell_error(shell, "pa_sync shall be boolean");
		return -ENOEXEC;
	}

	/* TODO: Support multiple subgroups */
	if (argc > 4) {
		subgroup.bis_sync = strtoul(argv[4], NULL, 0);
		if (subgroup.bis_sync > UINT32_MAX) {
			shell_error(shell, "bis_sync shall be 0x00000000 "
				    "to 0xFFFFFFFF");
			return -ENOEXEC;
		}
	}

	if (argc > 5) {
		subgroup.metadata_len = hex2bin(argv[6], strlen(argv[6]),
						subgroup.metadata, sizeof(subgroup.metadata));

		if (!subgroup.metadata_len) {
			shell_error(shell, "Could not parse metadata");
			return -ENOEXEC;
		}
	}

	param.num_subgroups = 1;
	param.subgroups = &subgroup;

	result = bt_bass_client_mod_src(default_conn, &param);
	if (result) {
		shell_print(shell, "Fail: %d", result);
	}
	return result;
}

static int cmd_bass_client_broadcast_code(const struct shell *shell,
					  size_t argc, char **argv)
{
	int result;
	int src_id;
	uint8_t broadcast_code[BASS_BROADCAST_CODE_SIZE] = { 0 };

	src_id = strtol(argv[1], NULL, 0);
	if (src_id < 0 || src_id > UINT8_MAX) {
		shell_error(shell, "adv_sid shall be 0x00-0xff");
		return -ENOEXEC;
	}

	for (int i = 0; i < argc - 2; i++) {
		broadcast_code[i] = strtol(argv[i + 2], NULL, 0);
	}

	result = bt_bass_client_broadcast_code(default_conn, src_id,
					       broadcast_code);
	if (result) {
		shell_print(shell, "Fail: %d", result);
	}
	return result;
}

static int cmd_bass_client_rem_src(const struct shell *shell, size_t argc,
				   char **argv)
{
	int result;
	int src_id;

	src_id = strtol(argv[1], NULL, 0);
	if (src_id < 0 || src_id > UINT8_MAX) {
		shell_error(shell, "adv_sid shall be 0x00-0xff");
		return -ENOEXEC;
	}

	result = bt_bass_client_rem_src(default_conn, src_id);
	if (result) {
		shell_print(shell, "Fail: %d", result);
	}
	return result;
}



static int cmd_bass_client_read_recv_state(const struct shell *shell,
					   size_t argc, char **argv)
{
	int result;
	int idx;

	idx = strtol(argv[1], NULL, 0);
	if (idx < 0 || idx > UINT8_MAX) {
		shell_error(shell, "adv_sid shall be 0x00-0xff");
		return -ENOEXEC;
	}

	result = bt_bass_client_read_recv_state(default_conn, idx);
	if (result) {
		shell_print(shell, "Fail: %d", result);
	}
	return result;
}


static int cmd_bass_client(const struct shell *shell, size_t argc, char **argv)
{
	if (argc > 1) {
		shell_error(shell, "%s unknown parameter: %s",
			    argv[0], argv[1]);
	} else {
		shell_error(shell, "%s Missing subcommand", argv[0]);
	}

	return -ENOEXEC;
}

SHELL_STATIC_SUBCMD_SET_CREATE(bass_client_cmds,
	SHELL_CMD_ARG(discover, NULL,
		      "Discover BASS on the server",
		      cmd_bass_client_discover, 1, 0),
	SHELL_CMD_ARG(scan_start, NULL,
		      "Start scanning for broadcasters",
		      cmd_bass_client_scan_start, 1, 0),
	SHELL_CMD_ARG(scan_stop, NULL,
		      "Stop scanning for BISs",
		      cmd_bass_client_scan_stop, 1, 0),
	SHELL_CMD_ARG(add_src, NULL,
		      "Add a source <address: XX:XX:XX:XX:XX:XX> "
		      "<type: public/random> <adv_sid> <sync_pa> [<sync_bis>] "
		      "[<metadata>]",
		      cmd_bass_client_add_src, 5, 2),
	SHELL_CMD_ARG(mod_src, NULL,
		      "Set sync <src_id> <address: XX:XX:XX:XX:XX:XX> <sync_pa> [<sync_bis>] [<metadata>]",
		      cmd_bass_client_mod_src, 4, 2),
	SHELL_CMD_ARG(broadcast_code, NULL,
		      "Send a space separated broadcast code of up to 16 bytes "
		      "<src_id> [broadcast code]",
		      cmd_bass_client_broadcast_code, 2, 16),
	SHELL_CMD_ARG(rem_src, NULL,
		      "Remove a source <src_id>",
		      cmd_bass_client_rem_src, 2, 0),
	SHELL_CMD_ARG(read_state, NULL,
		      "Remove a source <index>",
		      cmd_bass_client_read_recv_state, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_ARG_REGISTER(bass_client, &bass_client_cmds,
		       "Bluetooth BASS client shell commands",
		       cmd_bass_client, 1, 1);
