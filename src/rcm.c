/*
 * Copyright (c) 2011, NVIDIA CORPORATION
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "rcm.h"
#include "aes-cmac.h"

static int rcm_sign_msg(uint8_t *buf);
static void rcm_init_msg(
	uint8_t *buf,
	uint32_t msg_len,
	uint32_t opcode,
	void *args,
	uint32_t args_len,
	uint32_t payload_len);
static uint8_t *rcm_get_msg_payload(uint8_t *buf);
static void rcm_msg_pad(uint8_t *data, uint32_t len);
static uint32_t rcm_get_pad_len(uint32_t payload_len);
static uint32_t rcm_get_msg_buf_len(uint32_t payload_len);

uint32_t rcm_get_msg_len(uint8_t *msg)
{
	return ((rcm_msg_t*)msg)->len_insecure;
}

int rcm_create_msg(
	uint32_t opcode,
	uint8_t *args,
	uint32_t args_len,
	uint8_t *payload,
	uint32_t payload_len,
	uint8_t **buf)
{
	int ret = 0;
	uint32_t msg_len;
	uint8_t *msg = NULL;
	uint8_t *msg_payload;

	// create message buffer
	msg_len = rcm_get_msg_buf_len(payload_len);
	msg = malloc(msg_len);
	if (!msg) {
		ret = -ENOMEM;
		goto done;
	}

	// initialize message
	rcm_init_msg(msg, msg_len, opcode, args, args_len, payload_len);

	// fill message payload
	msg_payload = rcm_get_msg_payload(msg);
	if (payload_len)
		memcpy(msg_payload, payload, payload_len);

	// sign message
	rcm_sign_msg(msg);

done:
	if (ret) {
		free(msg);
		msg = NULL;
	}

	*buf = msg;

	return ret;
}

static int rcm_sign_msg(uint8_t *buf)
{
	rcm_msg_t *msg;
	uint32_t crypto_len;

	msg = (rcm_msg_t*)buf;

	// signing does not include the len_insecure and
	// cmac_hash fields at the beginning of the message.
	crypto_len = msg->len_insecure - sizeof(msg->len_insecure) -
		sizeof(msg->cmac_hash);
	if (crypto_len % RCM_AES_BLOCK_SIZE) {
		return -EMSGSIZE;
	}

	cmac_hash(msg->reserved, crypto_len, msg->cmac_hash);
	return 0;
}

static uint32_t rcm_get_msg_buf_len(uint32_t payload_len)
{
	return sizeof(rcm_msg_t) + payload_len +
		rcm_get_pad_len(payload_len);
}

static uint8_t *rcm_get_msg_payload(uint8_t *buf)
{
    return buf + sizeof(rcm_msg_t);
}

static void rcm_msg_pad(uint8_t *data, uint32_t len)
{
	if (!len)
		return;

	*data = 0x80;
	memset(data+1, 0, len-1);
}

static void rcm_init_msg(
	uint8_t *buf,
	uint32_t msg_len,
	uint32_t opcode,
	void *args,
	uint32_t args_len,
	uint32_t payload_len)
{
	uint32_t padding_len;
	rcm_msg_t *msg;

	msg = (rcm_msg_t *)buf;

	padding_len = rcm_get_pad_len(payload_len);

	msg->len_insecure = sizeof(rcm_msg_t) + payload_len +
		padding_len;

	memset(&msg->cmac_hash, 0x0, sizeof(msg->cmac_hash));
	memset(&msg->reserved, 0x0, sizeof(msg->reserved));

	msg->opcode         = opcode;
	msg->len_secure     = msg->len_insecure;
	msg->payload_len    = payload_len;
	msg->rcm_version    = RCM_VERSION;

	if (args_len)
		memcpy(msg->args, args, args_len);
	memset(msg->args + args_len, 0x0, sizeof(msg->args) - args_len);

	rcm_msg_pad(msg->padding, sizeof(msg->padding));
	rcm_msg_pad(buf + sizeof(rcm_msg_t) + payload_len, padding_len);
}

static uint32_t rcm_get_pad_len(uint32_t payload_len)
{
	uint32_t pad_len = 0;
	uint32_t msg_len = sizeof(rcm_msg_t) + payload_len;

	// First, use padding to bump the message size up to the minimum.
	if (msg_len < RCM_MIN_MSG_LENGTH) {
		pad_len = RCM_MIN_MSG_LENGTH - msg_len;
		msg_len += pad_len;
	}

	/*
	 * Next, add any extra padding needed to bump the relevant subset
	 * of the data up to a multiple of 16 bytes.  Subtracting off the
	 * rcm_msg_t size handles the initial data that is not part of
	 * the hashing and encryption.
	 */
	pad_len += 16 - ((msg_len - sizeof(rcm_msg_t)) & 0xf);

	return pad_len;
}

