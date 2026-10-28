// SPDX-FileCopyrightText: 2022 Riccardo Schirone <sirmy15@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-only
#include <rz_cmd.h>
#include <rz_core.h>
#include <cmd_descs.h>
#include "../core_private.h"

static bool macro_print_cb(RzCmd *cmd, const RzCmdMacro *macro, void *user) {
	RzCmdStateOutput *state = (RzCmdStateOutput *)user;
	switch (state->mode) {
	case RZ_OUTPUT_MODE_STANDARD:
		if (cmd->has_cons) {
			rz_cons_printf(cmd->core->cons, "(%s", macro->name);
			for (int i = 0; i < macro->nargs; i++) {
				rz_cons_printf(cmd->core->cons, " %s", macro->args[i]);
			}
			rz_cons_printf(cmd->core->cons, "%s)\n", macro->code);
		}
		break;
	case RZ_OUTPUT_MODE_JSON:
		pj_o(state->d.pj);
		pj_ks(state->d.pj, "name", macro->name);
		pj_ka(state->d.pj, "args");
		for (int i = 0; i < macro->nargs; i++) {
			pj_s(state->d.pj, macro->args[i]);
		}
		pj_end(state->d.pj);
		pj_ks(state->d.pj, "code", macro->code);
		pj_end(state->d.pj);
		break;
	default:
		rz_warn_if_reached();
		return false;
	}
	return true;
}

RZ_IPI RzCmdStatus rz_macros_list_handler(RzCore *core, int argc, const char **argv, RzCmdStateOutput *state) {
	rz_cmd_state_output_array_start(state);
	rz_cmd_macro_foreach(core->rcmd, macro_print_cb, state);
	rz_cmd_state_output_array_end(state);
	return RZ_CMD_STATUS_OK;
}

RZ_IPI RzCmdStatus rz_macros_remove_handler(RzCore *core, int argc, const char **argv) {
	return bool2status(rz_cmd_macro_rm(core->rcmd, argv[1]));
}

RZ_IPI RzCmdStatus rz_macros_handler(RzCore *core, const char *name, const char **args, const char *body, const char **argv) {
	rz_return_val_if_fail(core && name && args && body, RZ_CMD_STATUS_INVALID);

	const RzCmdMacro *macro = rz_cmd_macro_get(core->rcmd, name);
	if (!macro) {
		if (!rz_cmd_macro_add(core->rcmd, name, args, body)) {
			RZ_LOG_ERROR("core: Cannot add macro '%s'\n", name);
			return RZ_CMD_STATUS_ERROR;
		}
	} else {
		if (!rz_cmd_macro_update(core->rcmd, name, args, body)) {
			RZ_LOG_ERROR("core: Cannot update macro '%s'\n", name);
			return RZ_CMD_STATUS_ERROR;
		}
	}
	return argv ? rz_cmd_macro_call(core->rcmd, name, argv) : RZ_CMD_STATUS_OK;
}

/*
 * SDB Format:
 *
 *   /macros
 *     <macro name>={"args":[<str>],"code":<str>,}
 */

static bool macro_save_cb(RzCmd *cmd, const RzCmdMacro *macro, void *user) {
	Sdb *db = user;
	PJ *j = pj_new();
	if (!j) {
		return false;
	}
	pj_o(j);
	pj_ka(j, "args");
	for (size_t i = 0; i < macro->nargs; i++) {
		pj_s(j, macro->args[i]);
	}
	pj_end(j);
	pj_ks(j, "code", macro->code + 1);
	pj_end(j);
	sdb_set(db, macro->name, pj_string(j));
	pj_free(j);
	return true;
}

/**
 * \brief Save macros to a database.
 *
 * Serializes all defined macros from the given \p rcmd into the namespace
 * "macros" within the database \p db.
 *
 * \param db Database to save into.
 * \param rcmd RzCmd of which macros are to be serialized.
 */
RZ_API void rz_serialize_macro_save(RZ_NONNULL Sdb *db, RZ_NONNULL RzCmd *rcmd) {
	rz_return_if_fail(db && rcmd);
	rz_cmd_macro_foreach(rcmd, macro_save_cb, db);
}

typedef enum {
	BOOKMARK_FIELD_ARGS,
	BOOKMARK_FIELD_CODE,
} MacroField;

typedef struct {
	RzCmd *rcmd;
	RzKeyParser *parser;
} MacroLoadCtx;

static bool macros_load_cb(void *user, const SdbKv *kv) {
	MacroLoadCtx *ctx = user;

	char *json_str = sdbkv_dup_value(kv);
	if (!json_str) {
		return true;
	}
	RzJson *json = rz_json_parse(json_str);
	if (!json || json->type != RZ_JSON_OBJECT) {
		free(json_str);
		return false;
	}

	RzCmdMacro proto = { 0 };
	proto.name = (char *)sdbkv_key(kv);
	bool res = true;

	RZ_KEY_PARSER_JSON(ctx->parser, json, child, {
		case BOOKMARK_FIELD_ARGS:
			if (child->type != RZ_JSON_ARRAY) {
				res = false;
				goto beach;
			}
			proto.args = calloc(child->children.count, sizeof(char *));
			proto.nargs = 0;
			RzJson *baby;
			for (baby = child->children.first; baby; baby = baby->next) {
				if (baby->type != RZ_JSON_STRING) {
					res = false;
					goto beach;
				}
				proto.args[proto.nargs++] = (char *)baby->str_value;
			}
			break;
		case BOOKMARK_FIELD_CODE:
			if (child->type != RZ_JSON_STRING) {
				res = false;
				goto beach;
			}
			proto.code = (char *)child->str_value;
			break;
		default:
			break;
	});

	const RzCmdMacro *macro = rz_cmd_macro_get(ctx->rcmd, proto.name);
	if (!macro) {
		if (!rz_cmd_macro_add(ctx->rcmd, proto.name, (const char **)proto.args, proto.code)) {
			RZ_LOG_ERROR("core: Cannot add macro '%s'\n", proto.name);
			res = false;
		}
	} else {
		if (!rz_cmd_macro_update(ctx->rcmd, proto.name, (const char **)proto.args, proto.code)) {
			RZ_LOG_ERROR("core: Cannot update macro '%s'\n", proto.name);
			res = false;
		}
	}
beach:
	if (proto.args) {
		free(proto.args);
	}
	rz_json_free(json);
	free(json_str);
	return res;
}

static bool load_macros(RZ_NONNULL Sdb *macros_db, RZ_NONNULL RzCmd *rcmd) {
	MacroLoadCtx ctx = { rcmd, rz_key_parser_new() };
	if (!ctx.parser) {
		return false;
	}
	rz_key_parser_add(ctx.parser, "args", BOOKMARK_FIELD_ARGS);
	rz_key_parser_add(ctx.parser, "code", BOOKMARK_FIELD_CODE);
	bool r = sdb_foreach(macros_db, macros_load_cb, &ctx);
	rz_key_parser_free(ctx.parser);
	return r;
}

/**
 * \brief Load macros from a database.
 *
 * Clears all existing macros in \p rcmd and loads macros from the namespace
 * "macros" within the database \p db.
 *
 * \param db Database to load from.
 * \param rcmd RzCmd to populate.
 * \param res Optional result info structure for reporting errors, may be NULL.
 * \return True if loading succeeded. False if an error occurred.
 *
 * \note If no "macros" namespace exists, this is treated as an empty set
 *       of macros (not an error).
 */
RZ_API bool rz_serialize_macro_load(RZ_NONNULL Sdb *macros_db, RZ_NONNULL RzCmd *rcmd, RZ_NULLABLE RzSerializeResultInfo *res) {
	rz_return_val_if_fail(macros_db && rcmd, false);
	rz_cmd_macro_rm_all(rcmd);

	if (!macros_db) {
		// Old project, no macros saved -> treat as empty, not an error
		return true;
	}
	if (!load_macros(macros_db, rcmd)) {
		RZ_SERIALIZE_ERR(res, "failed to parse mark json");
		return false;
	}
	return true;
}

RZ_API void rz_cmd_macro_rm_all(RZ_NONNULL RzCmd *cmd) {
	rz_return_if_fail(cmd);
	ht_sp_clear(cmd->macros);
}
