/* SPDX-License-Identifier: BSD-2-Clause */
/*
* Copyright (c) 2014, STMicroelectronics International N.V.
* Copyright (c) 2016-2017, Renesas Electronics Corporation
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
* this list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice,
* this list of conditions and the following disclaimer in the documentation
* and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*/
#ifndef TEE_SVC_CRYP_H
#define TEE_SVC_CRYP_H

#include <tee_api_types.h>
#include <utee_types.h>
#include <tee/tee_obj.h>

struct user_ta_ctx;

TEE_Result syscall_cryp_obj_get_info(unsigned long obj, TEE_ObjectInfo *info);
TEE_Result syscall_cryp_obj_restrict_usage(unsigned long obj,
			unsigned long usage);
TEE_Result syscall_cryp_obj_get_attr(unsigned long obj, unsigned long attr_id,
			void *buffer, uint64_t *size);

TEE_Result syscall_cryp_obj_alloc(unsigned long obj_type,
			unsigned long max_key_size, uint32_t *obj);
TEE_Result syscall_cryp_obj_close(unsigned long obj);
TEE_Result syscall_cryp_obj_reset(unsigned long obj);
TEE_Result syscall_cryp_obj_populate(unsigned long obj,
			struct utee_attribute *attrs, unsigned long attr_count);
TEE_Result syscall_cryp_obj_copy(unsigned long dst_obj,
			unsigned long src_obj);
TEE_Result syscall_obj_generate_key(unsigned long obj, unsigned long key_size,
			const struct utee_attribute *params,
			unsigned long param_count);

TEE_Result syscall_cryp_state_alloc(unsigned long algo, unsigned long op_mode,
			unsigned long key1, unsigned long key2,
			uint32_t *state);
TEE_Result syscall_cryp_state_copy(unsigned long dst, unsigned long src);
TEE_Result syscall_cryp_state_free(unsigned long state);
void tee_svc_cryp_free_states(struct user_ta_ctx *utc);

/* iv and iv_len are ignored for hash algorithms */
TEE_Result syscall_hash_init(unsigned long state, const void *iv,
			size_t iv_len);
TEE_Result syscall_hash_update(unsigned long state, const void *chunk,
			size_t chunk_size);
TEE_Result syscall_hash_final(unsigned long state, const void *chunk,
			size_t chunk_size, void *hash, uint64_t *hash_len);

TEE_Result syscall_cipher_init(unsigned long state, const void *iv,
			size_t iv_len);
TEE_Result syscall_cipher_update(unsigned long state, const void *src,
			size_t src_len, void *dest, uint64_t *dest_len);
TEE_Result syscall_cipher_final(unsigned long state, const void *src,
			size_t src_len, void *dest, uint64_t *dest_len);

TEE_Result syscall_cryp_derive_key(unsigned long state,
			const struct utee_attribute *params,
			unsigned long param_count, unsigned long derived_key);

TEE_Result syscall_cryp_random_number_generate(void *buf, size_t blen);

TEE_Result syscall_authenc_init(unsigned long state, const void *nonce,
			size_t nonce_len, size_t tag_len,
			size_t aad_len, size_t payload_len);
TEE_Result syscall_authenc_update_aad(unsigned long state,
			const void *aad_data, size_t aad_data_len);
TEE_Result syscall_authenc_update_payload(unsigned long state,
			const void *src_data, size_t src_len, void *dest_data,
			uint64_t *dest_len);
TEE_Result syscall_authenc_enc_final(unsigned long state,
			const void *src_data, size_t src_len, void *dest_data,
			uint64_t *dest_len, void *tag, uint64_t *tag_len);
TEE_Result syscall_authenc_dec_final(unsigned long state,
			const void *src_data, size_t src_len, void *dest_data,
			uint64_t *dest_len, const void *tag, size_t tag_len);

TEE_Result syscall_asymm_operate(unsigned long state,
			const struct utee_attribute *usr_params,
			size_t num_params, const void *src_data,
			size_t src_len, void *dest_data, uint64_t *dest_len);
TEE_Result syscall_asymm_verify(unsigned long state,
			const struct utee_attribute *usr_params,
			size_t num_params, const void *data, size_t data_len,
			const void *sig, size_t sig_len);

TEE_Result tee_obj_set_type(struct tee_obj *o, uint32_t obj_type,
			    size_t max_key_size);

void tee_obj_attr_free(struct tee_obj *o);
void tee_obj_attr_clear(struct tee_obj *o);
TEE_Result tee_obj_attr_to_binary(struct tee_obj *o, void *data,
				  size_t *data_len);
TEE_Result tee_obj_attr_from_binary(struct tee_obj *o, const void *data,
				    size_t data_len);
TEE_Result tee_obj_attr_copy_from(struct tee_obj *o, const struct tee_obj *src);

TEE_Result syscall_rcar_aes_unwrap(void *srcData, uint32_t srcLen,
		const void *keyData, uint32_t keySize, uint32_t isSecretKey, void *destData,
		uint32_t *dstLen);
TEE_Result syscall_rcar_gen_skey_package(RCAR_SkeyParams_t *skeyParams,
		uint8_t *skeyPackageBuf, uint32_t skeyPackageSize);
TEE_Result syscall_rcar_asset_unpack(uint32_t assetId, uint8_t *pAssetPackage,
		uint32_t assetPackagLen, uint8_t *pAssetData,
		uint32_t *pAssetDataLen, uint32_t *pUserData);
#endif /* TEE_SVC_CRYP_H */
