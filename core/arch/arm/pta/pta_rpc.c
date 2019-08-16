/*
 * Copyright (C) Microsoft. All rights reserved
 */

#include <kernel/msg_param.h>
#include <kernel/pseudo_ta.h>
#include <optee_msg_supplicant.h>
#include <pta_rpc.h>
#include <string.h>

/* Send RPC to the rich OS */
static TEE_Result pta_rpc_execute(void *sess_ctx, uint32_t param_types,
                                  TEE_Param params[TEE_NUM_PARAMS]) {
  TEE_Result result = TEE_SUCCESS;
  bool mm_result;

  uint64_t memory_object_cookie = 0;
  struct mobj *memory_object = NULL;
  uint32_t memory_object_size = 0;
  uint32_t memory_object_offset = 0;
  uint8_t *memory_object_va = NULL;

  struct optee_msg_param rpc_msg_params[4] = {0};

  uint32_t pt_os;
  uint32_t pt_inout;
  uint32_t pt_in;
  uint32_t pt_out;

  /* Retrieve the type of the call parameters */
  pt_os = TEE_PARAM_TYPE_GET(param_types, 0);
  pt_inout = TEE_PARAM_TYPE_GET(param_types, 1);
  pt_in = TEE_PARAM_TYPE_GET(param_types, 2);
  pt_out = TEE_PARAM_TYPE_GET(param_types, 3);

  /* Assert the parameter types are what we expect */
  if (pt_os != TEE_PARAM_TYPE_VALUE_INPUT) {
    EMSG("Incorrect type for the first parameter: was %#x", pt_os);
    result = TEE_ERROR_BAD_PARAMETERS;
    goto done;
  }

  if (pt_inout != TEE_PARAM_TYPE_NONE &&
      pt_inout != TEE_PARAM_TYPE_MEMREF_INOUT) {
    EMSG("Incorrect type for the second parameter: %#x", pt_inout);
    result = TEE_ERROR_BAD_PARAMETERS;
    goto done;
  }

  if (pt_in != TEE_PARAM_TYPE_NONE && pt_in != TEE_PARAM_TYPE_MEMREF_INPUT) {
    EMSG("Incorrect type for the third parameter: %#x", pt_in);
    result = TEE_ERROR_BAD_PARAMETERS;
    goto done;
  }

  if (pt_out != TEE_PARAM_TYPE_NONE && pt_out != TEE_PARAM_TYPE_MEMREF_OUTPUT) {
    EMSG("Incorrect type for the fourth parameter: %#x", pt_out);
    result = TEE_ERROR_BAD_PARAMETERS;
    goto done;
  }

  /* Compute the size of memory to marshal back and forth */
  if (pt_inout)
    memory_object_size += params[1].memref.size;
  if (pt_in == TEE_PARAM_TYPE_MEMREF_INPUT)
    memory_object_size += params[2].memref.size;
  if (pt_out == TEE_PARAM_TYPE_MEMREF_OUTPUT)
    memory_object_size += params[3].memref.size;

  /* The RPC interface does not currently handle parameter-less calls */
  if (memory_object_size == 0) {
    EMSG("No parameters were provided for RPC Type: %#x", params[0].value.a);
    result = TEE_ERROR_BAD_PARAMETERS;
    goto done;
  }

  /* Allocate a single memory object for both in/out, input and output data */
  memory_object = thread_rpc_alloc_host_payload(
      memory_object_size, &memory_object_cookie, (vaddr_t)sess_ctx);
  if (memory_object == NULL) {
    EMSG("Failed to allocate memory object");
    result = TEE_ERROR_OUT_OF_MEMORY;
    goto done;
  }

  /* Get the VA for the memory object's buffer */
  memory_object_va = mobj_get_va(memory_object, 0);
  if (memory_object_va == NULL) {
    EMSG("Failed to get VA for memory object");
    result = TEE_ERROR_OUT_OF_MEMORY;
    goto done;
  }

  /* RPC parameter 0: contains three values */
  rpc_msg_params[0].attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;

  /* RPC parameter 0: A -> RPC Type */
  rpc_msg_params[0].u.value.a = params[0].value.a;

  /* RPC parameter 0: B -> TA Session ID
   * See the comment from pta_rpc_open_session
   */
  rpc_msg_params[0].u.value.b = (vaddr_t)sess_ctx;

  /* RPC parameter 0: C -> RPC Key */
  rpc_msg_params[0].u.value.c = params[0].value.b;

  /* Output the values of RPC parameter 0 to the console */
  FMSG("RPC context = %#x, type = %#x, TA session %#x",
       (uint32_t)rpc_msg_params[0].u.value.c,
       (uint32_t)rpc_msg_params[0].u.value.a,
       (uint32_t)rpc_msg_params[0].u.value.b);

  /* RPC parameter 1: in/out data buffer */
  if (pt_inout == TEE_PARAM_TYPE_MEMREF_INOUT) {
    mm_result = msg_param_init_memparam(
        &rpc_msg_params[1], memory_object, memory_object_offset,
        params[1].memref.size, memory_object_cookie, MSG_PARAM_MEM_DIR_INOUT);

    if (mm_result) {
      memcpy(memory_object_va, params[1].memref.buffer, params[1].memref.size);
      memory_object_offset += params[1].memref.size;
    } else {
      EMSG("msg_param_init_memparam failed for in/out data");
      result = TEE_ERROR_GENERIC;
      goto done;
    }
  }

  /* RPC parameter 2 - input data buffer */
  if (pt_in == TEE_PARAM_TYPE_MEMREF_INPUT) {
    mm_result = msg_param_init_memparam(
        &rpc_msg_params[2], memory_object, memory_object_offset,
        params[2].memref.size, memory_object_cookie, MSG_PARAM_MEM_DIR_IN);

    if (mm_result) {
      memcpy(memory_object_va + memory_object_offset, params[2].memref.buffer,
             params[2].memref.size);
      memory_object_offset += params[2].memref.size;
    } else {
      EMSG("msg_param_init_memparam failed for input data");
      result = TEE_ERROR_GENERIC;
      goto done;
    }
  }

  /* RPC parameter 3 - output data buffer */
  if (pt_out == TEE_PARAM_TYPE_MEMREF_OUTPUT) {
    mm_result = msg_param_init_memparam(
        &rpc_msg_params[3], memory_object, memory_object_offset,
        params[3].memref.size, memory_object_cookie, MSG_PARAM_MEM_DIR_OUT);

    if (!mm_result) {
      EMSG("msg_param_init_memparam failed for output data");
      result = TEE_ERROR_GENERIC;
      goto done;
    }
  }

  /* Send RPC message to the rich OS */
  result = thread_rpc_cmd(OPTEE_MSG_RPC_CMD_GENERIC, ARRAY_SIZE(rpc_msg_params),
                          rpc_msg_params);
  FMSG("Returned from thread_rpc_cmd with result = %#x", result);

  if (result == TEE_SUCCESS) {
    /* Copy the in/out data */
    if (pt_inout == TEE_PARAM_TYPE_MEMREF_INOUT) {
      size_t inout_buffer_size = msg_param_get_buf_size(&rpc_msg_params[1]);
      assert(inout_buffer_size <= params[1].memref.size);

      if (inout_buffer_size != 0)
        memcpy(params[1].memref.buffer, memory_object_va,
               params[1].memref.size);

      params[1].memref.size = inout_buffer_size;
    }

    /* Copy the output data, if necessary */
    if (pt_out == TEE_PARAM_TYPE_MEMREF_OUTPUT) {
      size_t out_buffer_size = msg_param_get_buf_size(&rpc_msg_params[3]);
      assert(out_buffer_size <= params[3].memref.size);

      if (params[3].memref.size != 0)
        memcpy(
            params[3].memref.buffer,
            memory_object_va +
                (pt_inout == TEE_PARAM_TYPE_NONE ? 0 : params[1].memref.size) +
                (pt_in == TEE_PARAM_TYPE_NONE ? 0 : params[2].memref.size),
            params[3].memref.size);

      params[3].memref.size = out_buffer_size;
    }
  }

done:
  if (memory_object != NULL) {
    thread_rpc_free_host_payload(memory_object_cookie, memory_object,
                                 (vaddr_t)sess_ctx);
  }

  return result;
}

/* PTA command handler */
static TEE_Result pta_rpc_invoke_command(void *sess_ctx, uint32_t cmd_id,
                                         uint32_t param_types,
                                         TEE_Param params[TEE_NUM_PARAMS]) {
  TEE_Result res;

  switch (cmd_id) {
  case PTA_RPC_EXECUTE:
    res = pta_rpc_execute(sess_ctx, param_types, params);
    break;

  default:
    EMSG("Command %#x is not supported\n", cmd_id);
    res = TEE_ERROR_NOT_IMPLEMENTED;
    break;
  }

  return res;
}

static TEE_Result
pta_rpc_open_session(uint32_t param_types __unused,
                     TEE_Param params[TEE_NUM_PARAMS] __unused,
                     void **sess_ctx) {
  /*
   * When the rich OS opened a session to the caller TA, it received
   * this value, that can be used as a "TA session ID".
   */
  *sess_ctx = tee_ta_get_calling_session();

  /* Just TAs are allowed to use this PTA */
  if (*sess_ctx == NULL) {
    EMSG("Rejecting session opened from rich OS");
    return TEE_ERROR_ACCESS_DENIED;
  }

  DMSG("RPC PTA open session succeeded");
  return TEE_SUCCESS;
}

static void pta_rpc_close_session(void *sess_ctx __unused) {
  DMSG("RPC PTA close session succeeded");
}

/* The TA manager uses a mutex to synchronize calls to any of these routines */
pseudo_ta_register(.uuid = PTA_RPC_UUID, .name = "RPC_PTA",
                   .flags = PTA_DEFAULT_FLAGS,
                   .open_session_entry_point = pta_rpc_open_session,
                   .close_session_entry_point = pta_rpc_close_session,
                   .invoke_command_entry_point = pta_rpc_invoke_command);
