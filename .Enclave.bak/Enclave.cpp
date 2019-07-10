/*

Copyright 2018 Intel Corporation

This software and the related documents are Intel copyrighted materials,
and your use of them is governed by the express license under which they
were provided to you (License). Unless the License provides otherwise,
you may not use, modify, copy, publish, distribute, disclose or transmit
this software or the related documents without Intel's prior written
permission.

This software and the related documents are provided as is, with no
express or implied warranties, other than those that are expressly stated
in the License.

*/

#ifndef _WIN32
#include "../config.h"
#endif
#include "Enclave_t.h"
#include <string.h>
#include <string>
#include <cstdlib>
#include <sgx_utils.h>
#include <sgx_tae_service.h>
#include <sgx_tkey_exchange.h>
#include <sgx_tcrypto.h>

#include "BISGX.h"

std::string BISGX_lex_main(std::string code);

static const sgx_ec256_public_t def_service_public_key = {
    {
        0x72, 0x12, 0x8a, 0x7a, 0x17, 0x52, 0x6e, 0xbf,
        0x85, 0xd0, 0x3a, 0x62, 0x37, 0x30, 0xae, 0xad,
        0x3e, 0x3d, 0xaa, 0xee, 0x9c, 0x60, 0x73, 0x1d,
        0xb0, 0x5b, 0xe8, 0x62, 0x1c, 0x4b, 0xeb, 0x38
    },
    {
        0xd4, 0x81, 0x40, 0xd9, 0x50, 0xe2, 0x57, 0x7b,
        0x26, 0xee, 0xb7, 0x41, 0xe7, 0xc6, 0x14, 0xe2,
        0x24, 0xb7, 0xbd, 0xc9, 0x03, 0xf2, 0x9a, 0x28,
        0xa8, 0x3c, 0xc8, 0x10, 0x11, 0x14, 0x5e, 0x06
    }

};

#define PSE_RETRIES	5	/* Arbitrary. Not too long, not too short. */

/*----------------------------------------------------------------------
 * WARNING
 *----------------------------------------------------------------------
 *
 * End developers should not normally be calling these functions
 * directly when doing remote attestation:
 *
 *    sgx_get_ps_sec_prop()
 *    sgx_get_quote()
 *    sgx_get_quote_size()
 *    sgx_get_report()
 *    sgx_init_quote()
 *
 * These functions short-circuits the RA process in order
 * to generate an enclave quote directly!
 *
 * The high-level functions provided for remote attestation take
 * care of the low-level details of quote generation for you:
 *
 *   sgx_ra_init()
 *   sgx_ra_get_msg1
 *   sgx_ra_proc_msg2
 *
 *----------------------------------------------------------------------
 */

/*
 * This doesn't really need to be a C++ source file, but a bug in 
 * 2.1.3 and earlier implementations of the SGX SDK left a stray
 * C++ symbol in libsgx_tkey_exchange.so so it won't link without
 * a C++ compiler. Just making the source C++ was the easiest way
 * to deal with that.
 */

sgx_status_t get_report(sgx_report_t *report, sgx_target_info_t *target_info)
{
#ifdef SGX_HW_SIM
	return sgx_create_report(NULL, NULL, report);
#else
	return sgx_create_report(target_info, NULL, report);
#endif
}

size_t get_pse_manifest_size ()
{
	return sizeof(sgx_ps_sec_prop_desc_t);
}

sgx_status_t get_pse_manifest(char *buf, size_t sz)
{
	sgx_ps_sec_prop_desc_t ps_sec_prop_desc;
	sgx_status_t status= SGX_ERROR_SERVICE_UNAVAILABLE;
	int retries= PSE_RETRIES;

	do {
		status= sgx_create_pse_session();
		if ( status != SGX_SUCCESS ) return status;
	} while (status == SGX_ERROR_BUSY && retries--);
	if ( status != SGX_SUCCESS ) return status;

	status= sgx_get_ps_sec_prop(&ps_sec_prop_desc);
	if ( status != SGX_SUCCESS ) return status;

	memcpy(buf, &ps_sec_prop_desc, sizeof(ps_sec_prop_desc));

	sgx_close_pse_session();

	return status;
}

sgx_status_t enclave_ra_init(sgx_ec256_public_t key, int b_pse,
	sgx_ra_context_t *ctx, sgx_status_t *pse_status)
{
	sgx_status_t ra_status;

	/*
	 * If we want platform services, we must create a PSE session 
	 * before calling sgx_ra_init()
	 */

	if ( b_pse ) {
		int retries= PSE_RETRIES;
		do {
			*pse_status= sgx_create_pse_session();
			if ( *pse_status != SGX_SUCCESS ) return SGX_ERROR_UNEXPECTED;
		} while (*pse_status == SGX_ERROR_BUSY && retries--);
		if ( *pse_status != SGX_SUCCESS ) return SGX_ERROR_UNEXPECTED;
	}

	ra_status= sgx_ra_init(&key, b_pse, ctx);

	if ( b_pse ) {
		int retries= PSE_RETRIES;
		do {
			*pse_status= sgx_create_pse_session();
			if ( *pse_status != SGX_SUCCESS ) return SGX_ERROR_UNEXPECTED;
		} while (*pse_status == SGX_ERROR_BUSY && retries--);
		if ( *pse_status != SGX_SUCCESS ) return SGX_ERROR_UNEXPECTED;
	}

	return ra_status;
}

sgx_status_t enclave_ra_init_def(int b_pse, sgx_ra_context_t *ctx,
	sgx_status_t *pse_status)
{
	return enclave_ra_init(def_service_public_key, b_pse, ctx, pse_status);
}

/*
 * Return a SHA256 hash of the requested key. KEYS SHOULD NEVER BE
 * SENT OUTSIDE THE ENCLAVE IN PLAIN TEXT. This function let's us
 * get proof of possession of the key without exposing it to untrusted
 * memory.
 */

sgx_status_t enclave_ra_get_key_hash(sgx_status_t *get_keys_ret,
	sgx_ra_context_t ctx, sgx_ra_key_type_t type, sgx_sha256_hash_t *hash)
{
	sgx_status_t sha_ret;
	sgx_ra_key_128_t k;

	// First get the requested key which is one of:
	//  * SGX_RA_KEY_MK 
	//  * SGX_RA_KEY_SK
	// per sgx_ra_get_keys().

	*get_keys_ret= sgx_ra_get_keys(ctx, type, &k);
	if ( *get_keys_ret != SGX_SUCCESS ) return *get_keys_ret;

	/* Now generate a SHA hash */

	sha_ret= sgx_sha256_msg((const uint8_t *) &k, sizeof(k), 
		(sgx_sha256_hash_t *) hash); // Sigh.

	/* Let's be thorough */

	memset(k, 0, sizeof(k));

	return sha_ret;
}

sgx_status_t enclave_ra_close(sgx_ra_context_t ctx)
{
        sgx_status_t ret;
        ret = sgx_ra_close(ctx);
        return ret;
}

sgx_status_t run_interpreter(sgx_ra_context_t context, unsigned char *code_cipher,
	size_t cipherlen, unsigned char *p_iv, unsigned char *tag, 
	unsigned char *res_cipher, size_t *res_len)
{
	sgx_status_t status = SGX_SUCCESS;
	sgx_ec_key_128bit_t sk_key, mk_key;
	
	/*Get session key SK to decrypt secret*/
	status = sgx_ra_get_keys(context, SGX_RA_KEY_SK, &sk_key);

	if(status != SGX_SUCCESS)
	{
		const char* message = "Error while obtaining session key.";
		OCALL_print(message);
		OCALL_print_status(status);
		return status;
	}
	
	uint32_t p_iv_len = 12;
	uint8_t intp_code[10000] = {'\0'};

	
	sgx_aes_gcm_128bit_tag_t tag_t;

	for(int i = 0; i < 16; i++)
	{
		tag_t[i] = tag[i];
	}
	
	//OCALL_dump(code_cipher_t, cipherlen);

	status = sgx_rijndael128GCM_decrypt(&sk_key, (uint8_t *)code_cipher, cipherlen,
		intp_code, p_iv, p_iv_len, NULL, 0, &tag_t);


	if(status != SGX_SUCCESS)
	{
		const char* message = "Error while decrypting SP's secret.";
		OCALL_print(message);
		OCALL_print_status(status);
		return status;
	}

	{
		//OCALL_print_int((int)sizeof(intp_code));
		const char *message = (const char*)intp_code;
		OCALL_print(message);
	}

	std::string intp_str(reinterpret_cast<char*>(intp_code));

	/*Call interpreter*/
	std::string intp_result = BISGX_lex_main(intp_str);
	
	OCALL_print("\nlexical analysis result:");
	OCALL_print(intp_result.c_str());

	/*processes for encrypt result*/
	uint8_t *intp_res_char;
	//uint8_t *res_cipher;
	uint8_t res_iv[12] = {'\0'};

	intp_res_char = reinterpret_cast<uint8_t*>
		(const_cast<char*>(intp_result.c_str()));
	
	*res_len = std::strlen((const char*)intp_res_char);

	OCALL_generate_nonce(res_iv, 12);

	/*AES/GCM's cipher length is equal to the length of plain text*/
	status = sgx_rijndael128GCM_encrypt(&sk_key, intp_res_char, *res_len,
		res_cipher, res_iv, 12, NULL, 0, &tag_t);

	if(status != SGX_SUCCESS)
	{
		OCALL_print("Error while encrypting result.");
		OCALL_print_status(status);
		return status;
	}

	
	for(int i = 0; i < 16; i++)
	{
		tag[i] = tag_t[i];
	}

	for(int i = 0; i < 12; i++)
	{
		p_iv[i] = res_iv[i];
	}

	OCALL_print("\nStart context check before exit ECALL.\n");
	OCALL_print("Cipher: ");
	OCALL_dump(res_cipher, *res_len);
	OCALL_print("\nIV: ");
	OCALL_dump(p_iv, 12);
	OCALL_print("\nTag: ");
	OCALL_dump(tag, 16);
	OCALL_print("\nResult cipher length: ");
	OCALL_print_int((int)*res_len);

	return SGX_SUCCESS;
}