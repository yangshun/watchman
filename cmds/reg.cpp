/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static w_ht_t *command_funcs = NULL;
static w_ht_t *capabilities = NULL;
/* Some error conditions will put us into a non-recoverable state where we
 * can't guarantee that we will be operating correctly.  Rather than suffering
 * in silence and misleading our clients, we'll poison ourselves and advertise
 * that we have done so and provide some advice on how the user can cure us. */
char *poisoned_reason = NULL;

static int compare_def(const void *A, const void *B)
{
  struct watchman_command_handler_def *a =
    *(struct watchman_command_handler_def**)A;
  struct watchman_command_handler_def *b =
    *(struct watchman_command_handler_def**)B;

  return strcmp(a->name, b->name);
}

void print_command_list_for_help(FILE *where)
{
  uint32_t i = 0, n = w_ht_size(command_funcs);
  struct watchman_command_handler_def **defs;
  w_ht_iter_t iter;

  defs = (watchman_command_handler_def**)calloc(n, sizeof(*defs));
  if (!defs) {
    abort();
  }
  if (w_ht_first(command_funcs, &iter)) do {
    defs[i++] = (watchman_command_handler_def*)w_ht_val_ptr(iter.value);
  } while (w_ht_next(command_funcs, &iter));

  if (n > 0) {
    qsort(defs, n, sizeof(*defs), compare_def);
  }

  fprintf(where, "\n\nAvailable commands:\n\n");
  for (i = 0; i < n; i++) {
    fprintf(where, "      %s\n", defs[i]->name);
  }
  free(defs);
}

void w_register_command(struct watchman_command_handler_def *defs)
{
  char capname[128];

  if (!command_funcs) {
    command_funcs = w_ht_new(16, &w_ht_string_funcs);
  }
  w_ht_set(command_funcs,
      w_ht_ptr_val(w_string_new_typed(defs->name, W_STRING_UNICODE)),
      w_ht_ptr_val(defs));

  snprintf(capname, sizeof(capname), "cmd-%s", defs->name);
  w_capability_register(capname);
}

static struct watchman_command_handler_def *lookup(
    json_t *args, char **errmsg, int mode)
{
  const char *cmd_name;
  const json_t *jstr;
  w_string_t *cmd;

  if (!json_array_size(args)) {
    ignore_result(asprintf(errmsg,
        "invalid command (expected an array with some elements!)"));
    return nullptr;
  }

  jstr = json_array_get(args, 0);
  cmd_name = json_string_value(jstr);
  if (!cmd_name) {
    ignore_result(asprintf(errmsg,
        "invalid command: expected element 0 to be the command name"));
    return nullptr;
  }
  cmd = json_to_w_string(jstr);
  auto def = (watchman_command_handler_def*)w_ht_val_ptr(
      w_ht_get(command_funcs, w_ht_ptr_val(cmd)));
  // Not added ref so not decrementing it.

  if (def) {
    if (mode && ((def->flags & mode) == 0)) {
      ignore_result(asprintf(errmsg,
          "command %s not available in this mode", cmd_name));
      return nullptr;
    }
    return def;
  }

  if (mode) {
    ignore_result(asprintf(errmsg, "unknown command %s", cmd_name));
  }

  return nullptr;
}

void preprocess_command(json_t *args, enum w_pdu_type output_pdu)
{
  char *errmsg = NULL;
  struct watchman_command_handler_def *def;

  def = lookup(args, &errmsg, 0);

  if (!def && !errmsg) {
    // Nothing known about it, pass the command on anyway for forwards
    // compatibility
    return;
  }

  if (!errmsg && def->cli_validate) {
    def->cli_validate(args, &errmsg);
  }

  if (errmsg) {
    w_jbuffer_t jr;

    json_t *err = json_pack(
      "{s:m, s:u, s:b}",
      "error", errmsg,
      "version", PACKAGE_VERSION,
      "cli_validated", true
    );

    w_json_buffer_init(&jr);
    w_ser_write_pdu(output_pdu, &jr, w_stm_stdout(), err);
    json_decref(err);
    w_json_buffer_free(&jr);

    free(errmsg);
    exit(1);
  }
}

bool dispatch_command(struct watchman_client *client, json_t *args, int mode)
{
  struct watchman_command_handler_def *def;
  char *errmsg = NULL;
  bool result = false;
  char sample_name[128];

  // Stash a reference to the current command to make it easier to log
  // the command context in some of the error paths
  client->current_command = args;
  json_incref(client->current_command);

  def = lookup(args, &errmsg, mode);

  if (!def) {
    send_error_response(client, "%s", errmsg);
    goto done;
  }

  if (poisoned_reason && (def->flags & CMD_POISON_IMMUNE) == 0) {
    send_error_response(client, "%s", poisoned_reason);
    goto done;
  }

  if (!client->client_is_owner && (def->flags & CMD_ALLOW_ANY_USER) == 0) {
    send_error_response(client, "you must be the process owner to execute '%s'",
                        def->name);
    return false;
  }

  // Scope for the perf sample
  {
    w_log(W_LOG_DBG, "dispatch_command: %s\n", def->name);
    snprintf(
        sample_name, sizeof(sample_name), "dispatch_command:%s", def->name);
    w_perf_t sample(sample_name);
    client->perf_sample = &sample;

    sample.set_wall_time_thresh(
        cfg_get_double(nullptr, "slow_command_log_threshold_seconds", 1.0));

    result = true;
    def->func(client, args);

    if (sample.finish()) {
      json_incref(args);
      sample.add_meta("args", args);
      sample.log();
    } else {
      w_log(W_LOG_DBG, "dispatch_command: %s (completed)\n", def->name);
    }
  }

done:
  free(errmsg);
  json_decref(client->current_command);
  client->current_command = NULL;
  client->perf_sample = nullptr;
  return result;
}

void w_capability_register(const char *name) {
  if (!capabilities) {
    capabilities = w_ht_new(128, &w_ht_string_funcs);
  }
  w_ht_set(capabilities,
      w_ht_ptr_val(w_string_new_typed(name, W_STRING_UNICODE)),
      true);
}

bool w_capability_supported(const w_string_t *name) {
  bool res;
  res = w_ht_get(capabilities, w_ht_ptr_val(name));
  return res;
}

json_t *w_capability_get_list(void) {
  json_t *arr = json_array_of_size(w_ht_size(capabilities));
  w_ht_iter_t iter;

  w_ht_first(capabilities, &iter);
  do {
    auto name = (w_string_t*)w_ht_val_ptr(iter.key);
    json_array_append(arr, w_string_to_json(name));
  } while (w_ht_next(capabilities, &iter));

  return arr;
}

/* vim:ts=2:sw=2:et:
 */