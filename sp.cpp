/*

Copyright 2019 Intel Corporation

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
#include "config.h"
#endif

#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#include <intrin.h>
#include <openssl/applink.c>
#include "win32/getopt.h"
#else
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#endif
#include <sgx_key_exchange.h>
#include <sgx_report.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include "json.hpp"
#include "common.h"
#include "hexutil.h"
#include "fileio.h"
#include "crypto.h"
#include "byteorder.h"
#include "msgio.h"
#include "protocol.h"
#include "base64.h"
#include "iasrequest.h"
#include "logfile.h"
#include "settings.h"
#include "enclave_verify.h"

using namespace json;
using namespace std;

#include <map>
#include <string>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>

#ifdef _WIN32
#define strdup(x) _strdup(x)
#endif

#define DIV_UNIT 20000000
#define CHUNK_SIZE 20000000
#define ROUND_UNIT 100000000

static const unsigned char def_service_private_key[32] = {
	0x90, 0xe7, 0x6c, 0xbb, 0x2d, 0x52, 0xa1, 0xce,
	0x3b, 0x66, 0xde, 0x11, 0x43, 0x9c, 0x87, 0xec,
	0x1f, 0x86, 0x6a, 0x3b, 0x65, 0xb6, 0xae, 0xea,
	0xad, 0x57, 0x34, 0x53, 0xd1, 0x03, 0x8c, 0x01
};

typedef struct ra_session_struct {
	unsigned char g_a[64];
	unsigned char g_b[64];
	unsigned char kdk[16];
	unsigned char smk[16];
	unsigned char sk[16];
	unsigned char mk[16];
	unsigned char vk[16];
} ra_session_t;

typedef struct config_struct {
	sgx_spid_t spid;
	unsigned char pri_subscription_key[IAS_SUBSCRIPTION_KEY_SIZE+1];
	unsigned char sec_subscription_key[IAS_SUBSCRIPTION_KEY_SIZE+1];
	uint16_t quote_type;
	EVP_PKEY *service_private_key;
	char *proxy_server;
	char *ca_bundle;
	char *user_agent;
	unsigned int proxy_port;
	unsigned char kdk[16];
	X509_STORE *store;
	X509 *signing_ca;
	unsigned int apiver;
	int strict_trust;
	sgx_measurement_t req_mrsigner;
	sgx_prod_id_t req_isv_product_id;
	sgx_isv_svn_t min_isvsvn;
	int allow_debug_enclave;
} config_t;

void usage();
#ifndef _WIN32
void cleanup_and_exit(int signo);
#endif

int derive_kdk(EVP_PKEY *Gb, unsigned char kdk[16], sgx_ec256_public_t g_a,
	config_t *config);

int process_msg01 (MsgIO *msg, IAS_Connection *ias, sgx_ra_msg1_t *msg1,
	sgx_ra_msg2_t *msg2, char **sigrl, config_t *config,
	ra_session_t *session);

int process_msg3 (MsgIO *msg, IAS_Connection *ias, sgx_ra_msg1_t *msg1,
	ra_msg4_t *msg4, config_t *config, ra_session_t *session);

int get_sigrl (IAS_Connection *ias, int version, sgx_epid_group_id_t gid,
	char **sigrl, uint32_t *msg2);

int get_attestation_report(IAS_Connection *ias, int version,
	const char *b64quote, sgx_ps_sec_prop_desc_t sec_prop, ra_msg4_t *msg4,
	int strict_trust);

int get_proxy(char **server, unsigned int *port, const char *url);

int encrypt_data_for_ISV(unsigned char *plaintext, int plaintext_len,
    unsigned char *key, unsigned char *iv, unsigned char *ciphertext, uint8_t *tag);

int decrypt_cipher_from_ISV(uint8_t *ciphertext, int ciphertext_len, uint8_t *key,
    uint8_t *iv, int iv_len, uint8_t *tag, uint8_t *plaintext);

int base64_encrypt(uint8_t *src, int srclen, uint8_t *dst, int dstlen);

int base64_decrypt(uint8_t *src, int srclen, uint8_t *dst, int dstlen);

uint8_t* generate_nonce(int sz);

string generate_random_filename();

int send_login_info(MsgIO *msgio, ra_session_t session, 
	string *username, string *datatype);

int process_vcf(string *tar_filename, string *access_list, uint8_t *sp_key,
	uint8_t *iv_array, uint8_t *tag_array, ifstream *vcf_ifs);

chrono::system_clock::time_point chrono_start, chrono_end;

void OCALL_chrono_start()
{
    chrono_start = chrono::system_clock::now();
}

void OCALL_chrono_end()
{
    chrono_end = chrono::system_clock::now();
    double elapsed = chrono::duration_cast<chrono::milliseconds>
        (chrono_end - chrono_start).count();

    cout << endl;
    cout << "-----------------------------------------------" << endl;
    cout << "Elapsed time is: " << elapsed << "[ms]" << endl;
    cout << "-----------------------------------------------" << endl;
    cout << endl;
}


char debug = 0;
char verbose = 0;
/* Need a global for the signal handler */
MsgIO *msgio = NULL;

/* Flag to bypass BIOS verification failure */
bool flag_BIOS = false;

int main(int argc, char *argv[])
{
	char flag_spid = 0;
	char flag_pubkey = 0;
	char flag_api_key = 0;
	char flag_ca = 0;
	char flag_usage = 0;
	char flag_noproxy= 0;
	char flag_prod= 0;
	char flag_stdio= 0;
	char flag_isv_product_id= 0;
	char flag_min_isvsvn= 0;
	char flag_mrsigner= 0;
	char *sigrl = NULL;
	config_t config;
	int oops;
	IAS_Connection *ias= NULL;
	char *port= NULL;
#ifndef _WIN32
	struct sigaction sact;
#endif

	/* Command line options */

	static struct option long_opt[] =
	{
		{"ias-signing-cafile",		required_argument,	0, 'A'},
		{"ca-bundle",				required_argument,	0, 'B'},
		{"no-debug-enclave",		no_argument,		0, 'D'},
		{"list-agents",				no_argument,		0, 'G'},
		{"ias-pri-api-key-file",	required_argument,	0, 'I'},
		{"ias-sec-api-key-file",	required_argument,	0, 'J'},
		{"service-key-file",		required_argument,	0, 'K'},
		{"mrsigner",				required_argument,  0, 'N'},
		{"production",				no_argument,		0, 'P'},
		{"isv-product-id",			required_argument,	0, 'R'},
		{"spid-file",				required_argument,	0, 'S'},
		{"min-isv-svn",				required_argument,  0, 'V'},
		{"strict-trust-mode",		no_argument,		0, 'X'},
		{"debug",					no_argument,		0, 'd'},
		{"user-agent",				required_argument,	0, 'g'},
		{"help",					no_argument, 		0, 'h'},
		{"ias-pri-api-key",			required_argument,	0, 'i'},
		{"ias-sec-api-key",			required_argument,	0, 'j'},
		{"key",						required_argument,	0, 'k'},
		{"linkable",				no_argument,		0, 'l'},
		{"proxy",					required_argument,	0, 'p'},
		{"api-version",				required_argument,	0, 'r'},
		{"spid",					required_argument,	0, 's'},
		{"verbose",					no_argument,		0, 'v'},
		{"no-proxy",				no_argument,		0, 'x'},
		{"stdio",					no_argument,		0, 'z'},
		{ 0, 0, 0, 0 }
	};

	cout << "*** INFO: SP is running as CLIENT. ***" << endl;

	/* Create a logfile to capture debug output and actual msg data */

	fplog = create_logfile("sp.log");
	fprintf(fplog, "Server log started\n");

	/* Config defaults */

	memset(&config, 0, sizeof(config));

	config.apiver= IAS_API_DEF_VERSION;

	/*
	 * For demo purposes only. A production/release enclave should
	 * never allow debug-mode enclaves to attest.
	 */
	config.allow_debug_enclave= 1;

	/* Parse our options */

	while (1) {
		int c;
		int opt_index = 0;
		off_t offset = IAS_SUBSCRIPTION_KEY_SIZE;
		int ret = 0;
		char *eptr= NULL;
		unsigned long val;

		c = getopt_long(argc, argv,
			"A:B:DGI:J:K:N:PR:S:V:X:dg:hk:lp:r:s:i:j:vxz",
			long_opt, &opt_index);
		if (c == -1) break;

		switch (c) {

		case 0:
			break;

		case 'A':
			if (!cert_load_file(&config.signing_ca, optarg)) {
				crypto_perror("cert_load_file");
				eprintf("%s: could not load IAS Signing Cert CA\n", optarg);
				return 1;
			}

			config.store = cert_init_ca(config.signing_ca);
			if (config.store == NULL) {
				eprintf("%s: could not initialize certificate store\n", optarg);
				return 1;
			}
			++flag_ca;

			break;

		case 'B':
			config.ca_bundle = strdup(optarg);
			if (config.ca_bundle == NULL) {
				perror("strdup");
				return 1;
			}

			break;

		case 'D':
			config.allow_debug_enclave= 0;
			break;
		case 'G':
			ias_list_agents(stdout);
			return 1;

		case 'I':
			// Get Size of File, should be IAS_SUBSCRIPTION_KEY_SIZE + EOF
			ret = from_file(NULL, optarg, &offset); 

			if ((offset != IAS_SUBSCRIPTION_KEY_SIZE+1) || (ret == 0)) {
				eprintf("IAS Primary Subscription Key must be %d-byte hex string.\n",
					IAS_SUBSCRIPTION_KEY_SIZE);
				return 1;
			}

			// Remove the EOF
			offset--;

			// Read the contents of the file
			if (!from_file((unsigned char *)&config.pri_subscription_key, optarg, &offset)) {
				eprintf("IAS Primary Subscription Key must be %d-byte hex string.\n",
					IAS_SUBSCRIPTION_KEY_SIZE);
					return 1;
			}
			break;

		case 'J':
			// Get Size of File, should be IAS_SUBSCRIPTION_KEY_SIZE + EOF
			ret = from_file(NULL, optarg, &offset);

			if ((offset != IAS_SUBSCRIPTION_KEY_SIZE+1) || (ret == 0)) {
				eprintf("IAS Secondary Subscription Key must be %d-byte hex string.\n",
					IAS_SUBSCRIPTION_KEY_SIZE);
				return 1;
			}

			// Remove the EOF
			offset--;

			// Read the contents of the file
			if (!from_file((unsigned char *)&config.sec_subscription_key, optarg, &offset)) {
				eprintf("IAS Secondary Subscription Key must be %d-byte hex string.\n",
					IAS_SUBSCRIPTION_KEY_SIZE);
					return 1;
			}

			break;

		case 'K':
			if (!key_load_file(&config.service_private_key, optarg, KEY_PRIVATE)) {
				crypto_perror("key_load_file");
				eprintf("%s: could not load EC private key\n", optarg);
				return 1;
			}
			break;

		case 'N':
			if (!from_hexstring((unsigned char *)&config.req_mrsigner,
				optarg, 32)) {

				eprintf("MRSIGNER must be 64-byte hex string\n");
				return 1;
			}
			++flag_mrsigner;
			break;

        case 'P':
			flag_prod = 1;
			break;

		case 'R':
			eptr= NULL;
			val= strtoul(optarg, &eptr, 10);
			if ( *eptr != '\0' || val > 0xFFFF ) {
				eprintf("Product Id must be a positive integer <= 65535\n");
				return 1;
			}
			config.req_isv_product_id= val;
			++flag_isv_product_id;
			break;

		case 'S':
			if (!from_hexstring_file((unsigned char *)&config.spid, optarg, 16)) {
				eprintf("SPID must be 32-byte hex string\n");
				return 1;
			}
			++flag_spid;

			break;

		case 'V':
			eptr= NULL;
			val= strtoul(optarg, &eptr, 10);
			if ( *eptr != '\0' || val > (unsigned long) 0xFFFF ) {
				eprintf("Minimum ISV SVN must be a positive integer <= 65535\n");
				return 1;
			}
			config.min_isvsvn= val;
			++flag_min_isvsvn;
			break;

		case 'X':
			config.strict_trust= 1;
			break;

		case 'd':
			debug = 1;
			break;

		case 'g':
			config.user_agent= strdup(optarg);
			if ( config.user_agent == NULL ) {
				perror("malloc");
				return 1;
			}
			break;

		case 'i':
			if (strlen(optarg) != IAS_SUBSCRIPTION_KEY_SIZE) {
				eprintf("IAS Subscription Key must be %d-byte hex string\n",IAS_SUBSCRIPTION_KEY_SIZE);
				return 1;
			}

			strncpy((char *) config.pri_subscription_key, optarg, IAS_SUBSCRIPTION_KEY_SIZE);

			break;

		case 'j':
			if (strlen(optarg) != IAS_SUBSCRIPTION_KEY_SIZE) {
				eprintf("IAS Secondary Subscription Key must be %d-byte hex string\n",
				IAS_SUBSCRIPTION_KEY_SIZE);
				return 1;
			}

			strncpy((char *) config.sec_subscription_key, optarg, IAS_SUBSCRIPTION_KEY_SIZE);

			break;

		case 'k':
			if (!key_load(&config.service_private_key, optarg, KEY_PRIVATE)) {
				crypto_perror("key_load");
				eprintf("%s: could not load EC private key\n", optarg);
				return 1;
			}
			break;

		case 'l':
			config.quote_type = SGX_LINKABLE_SIGNATURE;
			break;

		case 'p':
			if ( flag_noproxy ) usage();
			if (!get_proxy(&config.proxy_server, &config.proxy_port, optarg)) {
				eprintf("%s: could not extract proxy info\n", optarg);
				return 1;
			}
			// Break the URL into host and port. This is a simplistic algorithm.
			break;

		case 'r':
			config.apiver= atoi(optarg);
			if ( config.apiver < IAS_MIN_VERSION || config.apiver >
				IAS_MAX_VERSION ) {

				eprintf("version must be between %d and %d\n",
					IAS_MIN_VERSION, IAS_MAX_VERSION);
				return 1;
			}
			break;

		case 's':
			if (strlen(optarg) < 32) {
				eprintf("SPID must be 32-byte hex string\n");
				return 1;
			}
			if (!from_hexstring((unsigned char *)&config.spid, (unsigned char *)optarg, 16)) {
				eprintf("SPID must be 32-byte hex string\n");
				return 1;
			}
			++flag_spid;
			break;

		case 'v':
			verbose = 1;
			break;

		case 'x':
			if ( config.proxy_server != NULL ) usage();
			flag_noproxy=1;
			break;

		case 'z':
			flag_stdio= 1;
			break;

		case 'h':
		case '?':
		default:
			usage();
		}
	}

	/* We should have zero or one command-line argument remaining */

	argc-= optind;
	if ( argc > 1 ) usage();

	/* The remaining argument, if present, is the port number. */

	if ( flag_stdio && argc ) {
		usage();
	} else if ( argc ) {
		port= argv[optind];
	} else {
		port= strdup(DEFAULT_PORT);
		if ( port == NULL ) {
			perror("strdup");
			return 1;
		}
	}

	if ( debug ) {
		eprintf("+++ IAS Primary Subscription Key set to '%c%c%c%c........................%c%c%c%c'\n",
			config.pri_subscription_key[0],
        	config.pri_subscription_key[1],
        	config.pri_subscription_key[2],
        	config.pri_subscription_key[3],
        	config.pri_subscription_key[IAS_SUBSCRIPTION_KEY_SIZE -4 ],
        	config.pri_subscription_key[IAS_SUBSCRIPTION_KEY_SIZE -3 ],
        	config.pri_subscription_key[IAS_SUBSCRIPTION_KEY_SIZE -2 ],
        	config.pri_subscription_key[IAS_SUBSCRIPTION_KEY_SIZE -1 ]
		);

		eprintf("+++ IAS Secondary Subscription Key set to '%c%c%c%c........................%c%c%c%c'\n",
        	config.sec_subscription_key[0],
        	config.sec_subscription_key[1],
        	config.sec_subscription_key[2],
        	config.sec_subscription_key[3],
        	config.sec_subscription_key[IAS_SUBSCRIPTION_KEY_SIZE -4 ],
        	config.sec_subscription_key[IAS_SUBSCRIPTION_KEY_SIZE -3 ],
        	config.sec_subscription_key[IAS_SUBSCRIPTION_KEY_SIZE -2 ],
        	config.sec_subscription_key[IAS_SUBSCRIPTION_KEY_SIZE -1 ] 
		);
	}


	/* Use the default CA bundle unless one is provided */

	if ( config.ca_bundle == NULL ) {
		config.ca_bundle= strdup(DEFAULT_CA_BUNDLE);
		if ( config.ca_bundle == NULL ) {
			perror("strdup");
			return 1;
		}
		if ( debug ) eprintf("+++ Using default CA bundle %s\n",
			config.ca_bundle);
	}

	/*
	 * Use the hardcoded default key unless one is provided on the
	 * command line. Most real-world services would hardcode the
	 * key since the public half is also hardcoded into the enclave.
	 */

	if (config.service_private_key == NULL) {
		if (debug) {
			eprintf("Using default private key\n");
		}
		config.service_private_key = key_private_from_bytes(def_service_private_key);
		if (config.service_private_key == NULL) {
			crypto_perror("key_private_from_bytes");
			return 1;
		}

	}

	if (debug) {
		eprintf("+++ using private key:\n");
		PEM_write_PrivateKey(stderr, config.service_private_key, NULL,
			NULL, 0, 0, NULL);
		PEM_write_PrivateKey(fplog, config.service_private_key, NULL,
			NULL, 0, 0, NULL);
	}

	if (!flag_spid) {
		eprintf("--spid or --spid-file is required\n");
		flag_usage = 1;
	}

	if (!flag_ca) {
		eprintf("--ias-signing-cafile is required\n");
		flag_usage = 1;
	}

	if ( ! flag_isv_product_id ) {
		eprintf("--isv-product-id is required\n");
		flag_usage = 1;
	}
	
	if ( ! flag_min_isvsvn ) {
		eprintf("--min-isvsvn is required\n");
		flag_usage = 1;
	}
	
	if ( ! flag_mrsigner ) {
		eprintf("--mrsigner is required\n");
		flag_usage = 1;
	}

	if (flag_usage) usage();

	/* Initialize out support libraries */

	crypto_init();

	/* Initialize our IAS request object */

	try {
		ias = new IAS_Connection(
			(flag_prod) ? IAS_SERVER_PRODUCTION : IAS_SERVER_DEVELOPMENT,
			0,
			(char *)(config.pri_subscription_key),
			(char *)(config.sec_subscription_key)
		);
	}
	catch (...) {
		oops = 1;
		eprintf("exception while creating IAS request object\n");
		return 1;
	}

	if ( flag_noproxy ) ias->proxy_mode(IAS_PROXY_NONE);
	else if (config.proxy_server != NULL) {
		ias->proxy_mode(IAS_PROXY_FORCE);
		ias->proxy(config.proxy_server, config.proxy_port);
	}

	if ( config.user_agent != NULL ) {
		if ( ! ias->agent(config.user_agent) ) {
			eprintf("%s: unknown user agent\n", config.user_agent);
			return 0;
		}
	}

	/* 
	 * Set the cert store for this connection. This is used for verifying 
	 * the IAS signing certificate, not the TLS connection with IAS (the 
	 * latter is handled using config.ca_bundle).
	 */
	ias->cert_store(config.store);

	/*
	 * Set the CA bundle for verifying the IAS server certificate used
	 * for the TLS session. If this isn't set, then the user agent
	 * will fall back to it's default.
	 */
	if ( strlen(config.ca_bundle) ) ias->ca_bundle(config.ca_bundle);

	/* Get our message IO object. */
	
	if ( flag_stdio ) {
		msgio= new MsgIO();
	} else {
		try {
			msgio= new MsgIO("localhost", (port == NULL) ? DEFAULT_PORT : port);
		}
		catch(...) {
			return 1;
		}
	}

#ifndef _WIN32
	/* 
	 * Install some rudimentary signal handlers. We just want to make 
	 * sure we gracefully shutdown the listen socket before we exit
	 * to avoid "address already in use" errors on startup.
	 */

	sigemptyset(&sact.sa_mask);
	sact.sa_flags= 0;
	sact.sa_handler= &cleanup_and_exit;

	if ( sigaction(SIGHUP, &sact, NULL) == -1 ) perror("sigaction: SIGHUP");
	if ( sigaction(SIGINT, &sact, NULL) == -1 ) perror("sigaction: SIGHUP");
	if ( sigaction(SIGTERM, &sact, NULL) == -1 ) perror("sigaction: SIGHUP");
	if ( sigaction(SIGQUIT, &sact, NULL) == -1 ) perror("sigaction: SIGHUP");
#endif

 	/* If we're running in server mode, we'll block here.  */

	//while ( msgio->server_loop() ) {
		ra_session_t session;
		sgx_ra_msg1_t msg1;
		sgx_ra_msg2_t msg2;
		ra_msg4_t msg4;

		memset(&session, 0, sizeof(ra_session_t));

		/* Read message 0 and 1, then generate message 2 */

		if ( ! process_msg01(msgio, ias, &msg1, &msg2, &sigrl, &config,
			&session) ) {

			eprintf("error processing msg1\n");
			goto disconnect;
		}

		/* Send message 2 */

		/*
	 	* sgx_ra_msg2_t is a struct with a flexible array member at the
	 	* end (defined as uint8_t sig_rl[]). We could go to all the 
	 	* trouble of building a byte array large enough to hold the
	 	* entire struct and then cast it as (sgx_ra_msg2_t) but that's
	 	* a lot of work for no gain when we can just send the fixed 
	 	* portion and the array portion by hand.
	 	*/

		dividerWithText(stderr, "Copy/Paste Msg2 Below to Client");
		dividerWithText(fplog, "Msg2 (send to Client)");

		msgio->send_partial((void *) &msg2, sizeof(sgx_ra_msg2_t));
		fsend_msg_partial(fplog, (void *) &msg2, sizeof(sgx_ra_msg2_t));

		msgio->send(&msg2.sig_rl, msg2.sig_rl_size);
		fsend_msg(fplog, &msg2.sig_rl, msg2.sig_rl_size);

		edivider();

		/* Read message 3, and generate message 4 */

		if ( ! process_msg3(msgio, ias, &msg1, &msg4, &config, &session) ) {
			eprintf("error processing msg3\n");
			goto disconnect;
		}

		//sending msg4 complete; starting data encryption with session key "SK"
		if(flag_BIOS)
		{
			cerr << "============ Remote Attestation Information ============" << endl;
			cerr << "IAS says that target enclave needs BIOS or CPU microcode update," << endl;
			cerr << "But that error message always states that error regardless of BIOS/microcode version," << endl;
			cerr << "So just ignore it and complete RA, then starting secret data." << endl;
			cerr << "========================================================" << endl;
		}

		//Load data to process
		cout << endl << "Continue processing data. Enter any key to continue: " << endl;
		getchar();
		
		//to avoid compile error caused by goto sentence
		{
			ifstream fin_intp;
			string intp_filename, intp_str, username, datatype;
			stringstream ss_intp;
		
			int login_ret = send_login_info(msgio, session, 
				&username, &datatype);
			
			intp_str = "";

			if(login_ret == 0) /* Data owner */
			{
				intp_str += username;
				intp_str += "\n";
			}

			/* variables for vcf transmission */
            string tar_filename = "";
            string access_list = "";

            uint8_t *sp_key = session.sk;

			if(datatype == "inquiry")
			{
				cout << "Waiting for response from ISV...\n" << endl;
			}
			else if(datatype == "vcf")
            {
                /* process VCF file to send */
                int div_total = 0;
				uint8_t *iv_array, *tag_array;

				string vcf_filename;

				cout << "\nInput filename of vcf to send: ";
				cin >> vcf_filename;
				cout << endl;

				ifstream vcf_ifs(vcf_filename, ios::in);


				while(!vcf_ifs)
				{
					cerr << "Failed to open " << vcf_filename << "." << endl;
					cerr << "Input appropriate filename: ";

					cin >> vcf_filename;
					cout << endl;

					vcf_ifs.open(vcf_filename, ios::in);
				}

				/* DIV_UNIT is defined at upper side of this src. */
				size_t est_divnum = 0;
				vcf_ifs.seekg(0, ios::end);
				est_divnum = vcf_ifs.tellg();
				vcf_ifs.seekg(0, ios::beg);

				if(est_divnum % DIV_UNIT != 0)
				{
					est_divnum /= DIV_UNIT;
					est_divnum += 1;
				}
				else
				{
					est_divnum /= DIV_UNIT;
				}


				iv_array = new uint8_t[12 * est_divnum]();
				tag_array = new uint8_t[16 * est_divnum]();

				size_t divnum = 0;

                div_total = process_vcf(&tar_filename, &access_list, 
					sp_key, iv_array, tag_array, &vcf_ifs);

				divnum = div_total;


				/* remove temporary directory */
				string rm_cmd = "rm -rf encrypted_vcf/" + tar_filename;

				int sys_ret = system(rm_cmd.c_str());

				if(!WIFEXITED(sys_ret))
				{
					cerr << "Failed to create temporary directory." << endl;
					return -1;
				}

				
                if(div_total == -1)
                {
                    return -1;
                }

                string wht_filename, whitelist, wht_tmp, chrom, nation, disease_type;

                cout << "\nInput filename of whitelist: ";
                cin >> wht_filename;
                cout << "\n" << endl;

                ifstream wht_ifs(wht_filename);

                while(!wht_ifs)
                {
                    cout << "Failed to open whitelist file." << endl;
                    cout << "Input filename of whitelist appropriately: ";
                    cin >> wht_filename;
                    cout << "\n" << endl;
					 wht_ifs.open(wht_filename);
                }


                while(getline(wht_ifs, wht_tmp))
                {
                    whitelist += wht_tmp;
                    whitelist += ';';
                }

                whitelist.pop_back();


                cout << "Input chromosome number of this VCF file: " << endl;
                cin >> chrom;

				cout << "\nInput nation info of this VCF file: " << endl;
				cin >> nation;

				cout << "\nInput disease type info of this VCF file: " << endl;
				cin >> disease_type;



				string vcf_context = "";

                vcf_context += whitelist + '\n';
                vcf_context += chrom + '\n';
				vcf_context += nation + '\n';
				vcf_context += disease_type + '\n';
                vcf_context += tar_filename + '\n';
                vcf_context += username + '\n';
                vcf_context += to_string(div_total);


                uint8_t *vcf_uctx = (uint8_t*)vcf_context.c_str();

                uint8_t *iv_vctx = new uint8_t[12]();
                uint8_t *tag_vctx = new uint8_t[16]();
                uint8_t *vctx_cipher = new uint8_t[vcf_context.length()]();
				uint8_t *vctxb64 = new uint8_t[vcf_context.length() * 2]();
                int vctx_cipher_length = 0;
				int vctxb64_len = 0;

				/* encrypt VCF contexts */
				iv_vctx = generate_nonce(12);

                vctx_cipher_length = encrypt_data_for_ISV(vcf_uctx,
                    strlen((char*)vcf_uctx), sp_key, iv_vctx,
                    vctx_cipher, tag_vctx);

				cout << vctx_cipher_length << endl;
				BIO_dump_fp(stdout, (char*)vctx_cipher, vcf_context.length());
				BIO_dump_fp(stdout, (char*)iv_vctx, 12);
				BIO_dump_fp(stdout, (char*)tag_vctx, 16);
				

				/* encode VCF contexts to base64 */
                vctxb64_len = base64_encrypt(vctx_cipher, vctx_cipher_length,
                    vctxb64, vctx_cipher_length * 2);


				uint8_t *vctxlen_char = 
					(uint8_t*)to_string(vctx_cipher_length).c_str();


				uint8_t *iv_vctxb64 = new uint8_t[24]();
				uint8_t *tag_vctxb64 = new uint8_t[32]();
				uint8_t *deflen_vctxb64 = new uint8_t[64]();
				int iv_vctxb64_len, tag_vctxb64_len, deflen_vctxb64_len;


				iv_vctxb64_len = base64_encrypt(iv_vctx, 12, iv_vctxb64, 24);
				tag_vctxb64_len = base64_encrypt(tag_vctx, 
					16, tag_vctxb64, 32);
				deflen_vctxb64_len = base64_encrypt(vctxlen_char, 
					strlen((char*)vctxlen_char), deflen_vctxb64, 64);
				
				
				cout << "IVb64:" << endl;
				cout << iv_vctxb64 << endl;

				cout << "\nTagb64:" << endl;
				cout << tag_vctxb64 << endl;

				cout << "\ndeflenb64:" << endl;
				cout << deflen_vctxb64 << endl;

				cout << "\nvctxb64:" << endl;
				cout << vctxb64 << endl;

				/* send VCF contexts */
				cout << "\nSend VCF contexts to ISV..." << endl;

				msgio->send_nd(vctxb64, vctxb64_len);
				usleep(250000);
				msgio->send_nd(iv_vctxb64, iv_vctxb64_len);
				usleep(250000);
				msgio->send_nd(tag_vctxb64, tag_vctxb64_len);
				usleep(250000);
				msgio->send_nd(deflen_vctxb64, deflen_vctxb64_len);


				/* destruct heaps for VCF */
				delete(iv_vctx);
				delete(tag_vctx);
				delete(vctx_cipher);
				delete(vctxb64);
				delete(iv_vctxb64);
				delete(tag_vctxb64);
				delete(deflen_vctxb64);

                /* Open tarball */
                string tarball_name = "/tmp/" + tar_filename + ".tar";
                ifstream tar_ifs(tarball_name, ios::in | ios::binary);


                if(!tar_ifs)
                {
                    cerr << "Failed to open tarball." << endl;
                    return -1;
                }


                tar_ifs.seekg(0, ios::end);
                uint64_t tarball_size = tar_ifs.tellg();
                tar_ifs.seekg(0, ios::beg);

                int round_num = tarball_size / ROUND_UNIT;

                if(tarball_size % ROUND_UNIT != 0)
                {
                    round_num++;
                }



				/* Send size of tarball */
                uint8_t *tbsize_char =
                    (uint8_t*)to_string(tarball_size).c_str();

                cout << "\nSend size of tarball." << endl;
                msgio->send_nd(tbsize_char, strlen((char*)tbsize_char));


                cout << "\nSending encrypted VCF to ISV..." << endl;


                for(int i = 0; i < round_num; i++)
                {
                    int process_length = 0;

                    if(i == round_num - 1)
                    {
                        process_length = tarball_size % ROUND_UNIT;
                    }
                    else
                    {
                        process_length = ROUND_UNIT;
                    }


                    uint8_t *tar_uint8t = new uint8_t[process_length]();
                    tar_ifs.read((char*)tar_uint8t, process_length);


                    /* encode tarball to base64 */
                    int tar_b64len = 0;
                    uint8_t *tar_b64 = new uint8_t[process_length * 2]();
                    tar_b64len = base64_encrypt(tar_uint8t, process_length,
                        tar_b64, process_length * 2);


                    /* send partial base64-ed encrypted VCF */
                    msgio->send_nd(tar_b64, tar_b64len);

					delete(tar_uint8t);
					delete(tar_b64);

					usleep(10000);
                }


				/* encode iv_array and tag_array to base64 */
				uint8_t *iv_array_b64 = new uint8_t[12 * divnum * 2]();
				uint8_t *tag_array_b64 = new uint8_t[16 * divnum * 2]();
				int iv_array_b64len, tag_array_b64len;

				iv_array_b64len = base64_encrypt(iv_array, 12 * divnum,
					iv_array_b64, 12 * divnum * 2);

				tag_array_b64len = base64_encrypt(tag_array, 16 * divnum,
					tag_array_b64, 16 * divnum * 2);
				
				
				/* send IVs and tags */
				cout << "\nSending IV array for VCF..." << endl;
				msgio->send_nd(iv_array_b64, iv_array_b64len);
				usleep(250000);
				
				cout << "\nSending tag array for VCF..." << endl;
				msgio->send_nd(tag_array_b64, tag_array_b64len);

				delete(iv_array_b64);
				delete(tag_array_b64);



				/* remove tarball which has been already sent */
				rm_cmd = "rm -rf /tmp/" + tar_filename + ".tar";

				sys_ret = system(rm_cmd.c_str());

				if(!WIFEXITED(sys_ret))
				{
					cerr << "Failed to create temporary directory." << endl;
					return -1;
				}

			}
			else
			{
				while(1)
				{
					cout << "Input filename to send to ISV. " << endl;
					cout << "Filename: ";
					cin >> intp_filename;

					cout << endl;

					fin_intp.open(intp_filename, ios::in);

					if(!fin_intp)
					{
						cerr << "FileOpenError: Failed to open designated file.\n" << endl;
					}
					else
					{
						cout << "Open designated file \"" << intp_filename << "\" successfully." << endl;
						break;
					}
				}

				ss_intp << fin_intp.rdbuf();
				intp_str += ss_intp.str();
				
				unsigned char *intp_plain;
				unsigned const char *dummy_plain;

				dummy_plain = reinterpret_cast<unsigned const char*>(intp_str.c_str());
				intp_plain = const_cast<unsigned char*>(dummy_plain);

				/*
				cout << "Display the content of loaded file for check: \n" << endl;
				cout << "===============================================" << endl;
				cout << intp_plain << endl;
				cout << "===============================================" << endl;
				*/

				//Start encryption
				unsigned char* sp_key = session.sk;
				unsigned char* sp_iv;
				unsigned char *intp_cipher = new uint8_t[10000000]();
				int ciphertext_len, tag_len = 16;
				uint8_t tag[16] = {'\0'};

				cout << "Generate initialization vector." << endl;
				sp_iv = generate_nonce(12);

				/*AES/GCM's cipher length is equal to the length of plain text*/
				ciphertext_len = encrypt_data_for_ISV(intp_plain, strlen((char*)intp_plain), 
					sp_key, sp_iv, intp_cipher, tag);

				if(ciphertext_len == -1)
				{
					cerr << "Failed to operate encryption." << endl;
					cerr << "Abort program..." << endl;

					return -1;
				}
				
				cout << "Encrypted data successfully." << endl;
				//BIO_dump_fp(stdout, (const char*)intp_cipher, ciphertext_len);

				/*convert data to base64 for sending with msgio correctly*/
				//uint8_t cipherb64[300000] = {'\0'};
				uint8_t *cipherb64 = new uint8_t[ciphertext_len * 2]();
				int b64dst_len = ciphertext_len * 2, b64_len;

				b64_len = base64_encrypt(intp_cipher, ciphertext_len, cipherb64, b64dst_len);
				
				/*
				cout << "==========================================================" << endl;
				cout << "Base64 format of cipher is: " << endl;
				cout << cipherb64 << endl << endl;
				cout << "Length of cipher text is: " << ciphertext_len << endl;
				cout << "Length of base64-ed cipher is: " << b64_len << endl << endl;
				cout << "==========================================================" << endl;
				*/

				/*Next, convert IV to base64 format*/
				uint8_t ivb64[64] = {'\0'}; //maybe sufficient with 32-size
				int ivb64dst_len = 64, ivb64_len;

				ivb64_len = base64_encrypt(sp_iv, 12, ivb64, ivb64dst_len);

				cout << "Base64 format of initialization vector is: " << endl;
				cout << ivb64 << endl << endl;
				cout << "Length of base64-ed IV is: " << ivb64_len << endl << endl;
				cout << "==========================================================" << endl;

				/*Also convert tag to base64 format*/
				uint8_t tagb64[64] = {'\0'}; //maybe sufficient with 32-size
				int tagb64dst_len = 64, tagb64_len;

				tagb64_len = base64_encrypt(tag, 16, tagb64, tagb64dst_len);

				cout << "Base64 format of tag is: " << endl;
				cout << tagb64 << endl << endl;
				cout << "Length of base64-ed tag is: " << tagb64_len << endl << endl;
				cout << "==========================================================" << endl;
				
				/*In addition to that, need to convert cipher's length to base64*/
				uint8_t deflenb64[128] = {'\0'}; 
				uint8_t *uint8tdeflen;
				int deflenb64dst_len = 32, deflenb64_len;

				uint8tdeflen = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(to_string(ciphertext_len).c_str()));

				cout << "uint8_t-ed default cipher length is: " << uint8tdeflen << endl << endl;

				deflenb64_len = base64_encrypt(uint8tdeflen, strlen((char*)uint8tdeflen), deflenb64, 32);
				cout << "Base64 format of default cipher length is: " << endl;
				cout << deflenb64 << endl << endl;
				cout << "Length of base64-ed default cipher length is: " << deflenb64_len << endl;
				cout << "==========================================================" << endl;


				/*Send base64-ed secret, its length, MAC tag and its length to ISV*/

				
				/*IV*/
				cout << "Initialization vector to be sent is: " << endl;
				msgio->send(ivb64, strlen((char*)ivb64));

				cout << "Complete sending IV." << endl;
				cout << "Please wait for 0.25 sec." << endl;
				cout << "==========================================================" << endl;

				usleep(250000);
				
				/*MAC tag*/
				cout << "Tag to be sent is: (display again)" << endl;
				msgio->send(tagb64, strlen((char*)tagb64));

				cout << "Complete sending MAC tag." << endl;
				cout << "Please wait for 0.25 sec." << endl;
				cout << "==========================================================" << endl;

				usleep(250000);

				/*default cipher length*/
				cout << "Default cipher's length to be sent is: " << endl;
				msgio->send(deflenb64, deflenb64_len);

				cout << "Complete sending default cipher length." << endl;
				cout << "Please wait for 0.25 sec." << endl;
				cout << "==========================================================" << endl;
				
				usleep(250000);

				/*cipher*/
				cout << "==========================================================" << endl;
				cout << "Send encrypted file to ISV." << endl;

				cout << "Encrypted secret to be sent in base64 is (display again): " << endl;
				msgio->send_nd(cipherb64, strlen((char*)cipherb64));

				cout << "Complete sending message." << endl;
				cout << "==========================================================" << endl;
			}

			/*Receive result contexts from ISV*/
			int rv;
			size_t sz;
			void **received_cipher;
			void **received_iv;
			void **received_tag;
			void **received_deflen;
			size_t rcipherb64_len, rivb64_len, rtagb64_len, rdeflenb64_len; 


			cout << "\nWaiting for results from ISV..." << endl;

			rv = msgio->read((void **) &received_cipher, &sz);

			if ( rv == -1 ) {
				eprintf("system error reading secret from ISV\n");
				return 0;
			} else if ( rv == 0 ) {
				eprintf("protocol error reading secret from ISV\n");
				return 0;
			}

			rcipherb64_len = sz / 2;

			
			rv = msgio->read((void **) &received_iv, &sz);

			if(rv == -1) {
				eprintf("system error reading IV from ISV\n");
				return 0;
			} else if ( rv == 0 ) {
				eprintf("protocol error reading IV from ISV\n");
				return 0;
			}

			rivb64_len = sz / 2;

			
			rv = msgio->read((void **) &received_tag, &sz);

			if ( rv == -1 ) {
				eprintf("system error reading MAC tag from ISV\n");
				return 0;
			} else if ( rv == 0 ) {
				eprintf("protocol error reading MAC tag from ISV\n");
				return 0;
			}

			rtagb64_len = sz / 2;

			
			rv = msgio->read((void **) &received_deflen, &sz);

			if ( rv == -1 ) {
				eprintf("system error reading default cipher length from ISV\n");
				return 0;
			} else if ( rv == 0 ) {
				eprintf("protocol error reading default cipher length from ISV\n");
				return 0;
			}

			rdeflenb64_len = sz / 2;

			/*obtain result contexts from received void* buffers*/
			uint8_t *rcipherb64 = (uint8_t *) received_cipher;
			//uint8_t *rcipherb64 = new uint8_t[rcipherb64_len]();
			uint8_t *rivb64 	= (uint8_t *) received_iv;
			uint8_t *rtagb64 	= (uint8_t *) received_tag;
			uint8_t *rdeflenb64 = (uint8_t *) received_deflen;
			uint8_t *void_to_uchar;

			/*value check*/
			cout << "Received base64-ed result cipher is: " << endl;
			cout << rcipherb64 << endl << endl;

			cout << "Received base64-ed result IV is: " << endl;
			cout << rivb64 << endl << endl;

			cout << "Received base64-ed result MAC tag is: " << endl;
			cout << rtagb64 << endl << endl;

			cout << "Received base64-ed result cipher length is: " << endl;
			cout << rdeflenb64 << endl << endl;

			/*decode result contexts from base64*/
			int result_deflen, rettmp;
			uint8_t *result_cipher;
			uint8_t result_iv_tmp[32] = {'\0'};
			uint8_t result_tag_tmp[32] = {'\0'};
			uint8_t result_deflen_tmp[128] = {'\0'};

			/*Result cipher length*/
			rettmp = base64_decrypt(rdeflenb64, rdeflenb64_len, result_deflen_tmp, 128);
			result_deflen = strtol((char*)result_deflen_tmp, NULL, 10);

			/*Result cipher*/
			result_cipher = new uint8_t[rcipherb64_len];
			int rcipher_len;

			rcipher_len = base64_decrypt(rcipherb64, rcipherb64_len, result_cipher, rcipherb64_len);

			/*Result IV*/
			int riv_len;
			riv_len = base64_decrypt(rivb64, rivb64_len, result_iv_tmp, 32);

			/*Result tag*/
			int rtag_len;
			rtag_len = base64_decrypt(rtagb64, rtagb64_len, result_tag_tmp, 32);


			/*Value check*/
			cout << "\nResult cipher length is: " << result_deflen << endl << endl;

			cout << "Result cipher is: " << endl;
			BIO_dump_fp(stdout, (const char*)result_cipher, result_deflen);
			cout << endl;

			cout << "Result IV is: " << endl;
			BIO_dump_fp(stdout, (const char*)result_iv_tmp, 12);
			cout << endl;

			cout << "Result MAC tag is: " << endl;
			BIO_dump_fp(stdout, (const char*)result_tag_tmp, 16);
			cout << endl;

			uint8_t result_iv[12];
			uint8_t result_tag[16];

			for(int i = 0; i < 12; i++)
			{
				result_iv[i] = result_iv_tmp[i];
			}

			for(int i = 0; i < 16; i++)
			{
				result_tag[i] = result_tag_tmp[i];
			}
			
			/*Decrypt result*/
			uint8_t result[200000];
			int result_len;
			
			result_len = decrypt_cipher_from_ISV(result_cipher, result_deflen, sp_key, 
				result_iv, 12, result_tag, result);

			if(result_len == -1)
			{
				cerr << "Verification error while decryption." << endl << endl;
			}
			else
			{
				cout << "Decrypted result successfully." << endl;
				cout << "Received result is: " << endl;
				
				cout << "==============================================" << endl;
				cout << result << endl;
				cout << "==============================================" << endl;
				cout << endl;

				ofstream result_output("result.dat", ios::trunc);
				result_output << result << endl;
				result_output.close();
			}
		}
		//end block to avoid goto error

disconnect:
		msgio->disconnect();
	//}

	crypto_destroy();

	return 0;
}

int process_msg3 (MsgIO *msgio, IAS_Connection *ias, sgx_ra_msg1_t *msg1,
	ra_msg4_t *msg4, config_t *config, ra_session_t *session)
{
	sgx_ra_msg3_t *msg3;
	size_t blen= 0;
	size_t sz;
	int rv;
	uint32_t quote_sz;
	char *buffer= NULL;
	char *b64quote;
	sgx_mac_t vrfymac;
	sgx_quote_t *q;

	/*
	 * Read our incoming message. We're using base16 encoding/hex strings
	 * so we should end up with sizeof(msg)*2 bytes.
	 */

	fprintf(stderr, "Waiting for msg3\n");

	/*
	 * Read message 3
	 *
	 * CMACsmk(M) || M
	 *
	 * where
	 *
	 * M = ga || PS_SECURITY_PROPERTY || QUOTE
	 *
	 */

	rv= msgio->read((void **) &msg3, &sz);
	if ( rv == -1 ) {
		eprintf("system error reading msg3\n");
		return 0;
	} else if ( rv == 0 ) {
		eprintf("protocol error reading msg3\n");
		return 0;
	}
	if ( debug ) {
		eprintf("+++ read %lu bytes\n", sz);
	}

	/*
	 * The quote size will be the total msg3 size - sizeof(sgx_ra_msg3_t)
	 * since msg3.quote is a flexible array member.
	 *
	 * Total message size is sz/2 since the income message is in base16.
	 */
	quote_sz = (uint32_t)((sz / 2) - sizeof(sgx_ra_msg3_t));
	if ( debug ) {
		eprintf("+++ quote_sz= %lu bytes\n", quote_sz);
	}

	/* Make sure Ga matches msg1 */

	if ( debug ) {
		eprintf("+++ Verifying msg3.g_a matches msg1.g_a\n");
		eprintf("msg1.g_a.gx = %s\n",
			hexstring(msg3->g_a.gx, sizeof(msg1->g_a.gx)));
		eprintf("msg1.g_a.gy = %s\n",
			hexstring(&msg3->g_a.gy, sizeof(msg1->g_a.gy)));
		eprintf("msg3.g_a.gx = %s\n",
			hexstring(msg3->g_a.gx, sizeof(msg3->g_a.gx)));
		eprintf("msg3.g_a.gy = %s\n",
			hexstring(&msg3->g_a.gy, sizeof(msg3->g_a.gy)));
	}
	if ( CRYPTO_memcmp(&msg3->g_a, &msg1->g_a, sizeof(sgx_ec256_public_t)) ) {
		eprintf("msg1.g_a and mgs3.g_a keys don't match\n");
		free(msg3);
		return 0;
	}

	/* Validate the MAC of M */

	cmac128(session->smk, (unsigned char *) &msg3->g_a,
		sizeof(sgx_ra_msg3_t)-sizeof(sgx_mac_t)+quote_sz,
		(unsigned char *) vrfymac);
	if ( debug ) {
		eprintf("+++ Validating MACsmk(M)\n");
		eprintf("msg3.mac   = %s\n", hexstring(msg3->mac, sizeof(sgx_mac_t)));
		eprintf("calculated = %s\n", hexstring(vrfymac, sizeof(sgx_mac_t)));
	}
	if ( CRYPTO_memcmp(msg3->mac, vrfymac, sizeof(sgx_mac_t)) ) {
		eprintf("Failed to verify msg3 MAC\n");
		free(msg3);
		return 0;
	}

	/* Encode the report body as base64 */

	b64quote= base64_encode((char *) &msg3->quote, quote_sz);
	if ( b64quote == NULL ) {
		eprintf("Could not base64 encode the quote\n");
		free(msg3);
		return 0;
	}
	q= (sgx_quote_t *) msg3->quote;

	if ( verbose ) {

		edividerWithText("Msg3 Details (from Client)");
		eprintf("msg3.mac                 = %s\n",
			hexstring(&msg3->mac, sizeof(msg3->mac)));
		eprintf("msg3.g_a.gx              = %s\n",
			hexstring(msg3->g_a.gx, sizeof(msg3->g_a.gx)));
		eprintf("msg3.g_a.gy              = %s\n",
			hexstring(&msg3->g_a.gy, sizeof(msg3->g_a.gy)));
		eprintf("msg3.ps_sec_prop         = %s\n",
			hexstring(&msg3->ps_sec_prop, sizeof(msg3->ps_sec_prop)));
		eprintf("msg3.quote.version       = %s\n",
			hexstring(&q->version, sizeof(uint16_t)));
		eprintf("msg3.quote.sign_type     = %s\n",
			hexstring(&q->sign_type, sizeof(uint16_t)));
		eprintf("msg3.quote.epid_group_id = %s\n",
			hexstring(&q->epid_group_id, sizeof(sgx_epid_group_id_t)));
		eprintf("msg3.quote.qe_svn        = %s\n",
			hexstring(&q->qe_svn, sizeof(sgx_isv_svn_t)));
		eprintf("msg3.quote.pce_svn       = %s\n",
			hexstring(&q->pce_svn, sizeof(sgx_isv_svn_t)));
		eprintf("msg3.quote.xeid          = %s\n",
			hexstring(&q->xeid, sizeof(uint32_t)));
		eprintf("msg3.quote.basename      = %s\n",
			hexstring(&q->basename, sizeof(sgx_basename_t)));
		eprintf("msg3.quote.report_body   = %s\n",
			hexstring(&q->report_body, sizeof(sgx_report_body_t)));
		eprintf("msg3.quote.signature_len = %s\n",
			hexstring(&q->signature_len, sizeof(uint32_t)));
		eprintf("msg3.quote.signature     = %s\n",
			hexstring(&q->signature, q->signature_len));

		edividerWithText("Enclave Quote (base64) ==> Send to IAS");

		eputs(b64quote);

		eprintf("\n");
		edivider();
	}

	/* Verify that the EPID group ID in the quote matches the one from msg1 */

	if ( debug ) {
		eprintf("+++ Validating quote's epid_group_id against msg1\n");
		eprintf("msg1.egid = %s\n", 
			hexstring(msg1->gid, sizeof(sgx_epid_group_id_t)));
		eprintf("msg3.quote.epid_group_id = %s\n",
			hexstring(&q->epid_group_id, sizeof(sgx_epid_group_id_t)));
	}

	if ( memcmp(msg1->gid, &q->epid_group_id, sizeof(sgx_epid_group_id_t)) ) {
		eprintf("EPID GID mismatch. Attestation failed.\n");
		free(b64quote);
		free(msg3);
		return 0;
	}


	if ( get_attestation_report(ias, config->apiver, b64quote,
		msg3->ps_sec_prop, msg4, config->strict_trust) ) {

		unsigned char vfy_rdata[64];
		unsigned char msg_rdata[144]; /* for Ga || Gb || VK */

		sgx_report_body_t *r= (sgx_report_body_t *) &q->report_body;

		memset(vfy_rdata, 0, 64);

		/*
		 * Verify that the first 64 bytes of the report data (inside
		 * the quote) are SHA256(Ga||Gb||VK) || 0x00[32]
		 *
		 * VK = CMACkdk( 0x01 || "VK" || 0x00 || 0x80 || 0x00 )
		 *
		 * where || denotes concatenation.
		 */

		/* Derive VK */

		cmac128(session->kdk, (unsigned char *)("\x01VK\x00\x80\x00"),
				6, session->vk);

		/* Build our plaintext */

		memcpy(msg_rdata, session->g_a, 64);
		memcpy(&msg_rdata[64], session->g_b, 64);
		memcpy(&msg_rdata[128], session->vk, 16);

		/* SHA-256 hash */

		sha256_digest(msg_rdata, 144, vfy_rdata);

		if ( verbose ) {
			edividerWithText("Enclave Report Verification");
			if ( debug ) {
				eprintf("VK                 = %s\n", 
					hexstring(session->vk, 16));
			}
			eprintf("SHA256(Ga||Gb||VK) = %s\n",
				hexstring(vfy_rdata, 32));
			eprintf("report_data[64]    = %s\n",
				hexstring(&r->report_data, 64));
		}

		if ( CRYPTO_memcmp((void *) vfy_rdata, (void *) &r->report_data,
			64) ) {

			eprintf("Report verification failed.\n");
			free(b64quote);
			free(msg3);
			return 0;
		}

		/*
		 * The service provider must validate that the enclave
		 * report is from an enclave that they recognize. Namely,
		 * that the MRSIGNER matches our signing key, and the MRENCLAVE
		 * hash matches an enclave that we compiled.
		 *
		 * Other policy decisions might include examining ISV_SVN to 
		 * prevent outdated/deprecated software from successfully
		 * attesting, and ensuring the TCB is not out of date.
		 *
		 * A real-world service provider might allow multiple ISV_SVN
		 * values, but for this sample we only allow the enclave that
		 * is compiled.
		 */

#ifndef _WIN32
/* Windows implementation is not available yet */

		if ( ! verify_enclave_identity(config->req_mrsigner, 
			config->req_isv_product_id, config->min_isvsvn, 
			config->allow_debug_enclave, r) ) {

			eprintf("Invalid enclave.\n");
			msg4->status= NotTrusted;
		}
#endif

		if ( verbose ) {
			edivider();

			// The enclave report is valid so we can trust the report
			// data.

			edividerWithText("Enclave Report Details");

			eprintf("cpu_svn     = %s\n",
				hexstring(&r->cpu_svn, sizeof(sgx_cpu_svn_t)));
			eprintf("misc_select = %s\n",
				hexstring(&r->misc_select, sizeof(sgx_misc_select_t)));
			eprintf("attributes  = %s\n",
				hexstring(&r->attributes, sizeof(sgx_attributes_t)));
			eprintf("mr_enclave  = %s\n",
				hexstring(&r->mr_enclave, sizeof(sgx_measurement_t)));
			eprintf("mr_signer   = %s\n",
				hexstring(&r->mr_signer, sizeof(sgx_measurement_t)));
			eprintf("isv_prod_id = %04hX\n", r->isv_prod_id);
			eprintf("isv_svn     = %04hX\n", r->isv_svn);
			eprintf("report_data = %s\n",
				hexstring(&r->report_data, sizeof(sgx_report_data_t)));
		}


		edividerWithText("Copy/Paste Msg4 Below to Client"); 

		/* Serialize the members of the Msg4 structure independently */
		/* vs. the entire structure as one send_msg() */

		msgio->send_partial(&msg4->status, sizeof(msg4->status));
		msgio->send(&msg4->platformInfoBlob, sizeof(msg4->platformInfoBlob));

		fsend_msg_partial(fplog, &msg4->status, sizeof(msg4->status));
		fsend_msg(fplog, &msg4->platformInfoBlob,
			sizeof(msg4->platformInfoBlob));
		edivider();

		/*
		 * If the enclave is trusted, derive the MK and SK. Also get
		 * SHA256 hashes of these so we can verify there's a shared
		 * secret between us and the client.
		 */

		//if ( msg4->status == Trusted ) {
		if(true){
			unsigned char hashmk[32], hashsk[32];

			if ( debug ) eprintf("+++ Deriving the MK and SK\n");
			cmac128(session->kdk, (unsigned char *)("\x01MK\x00\x80\x00"),
				6, session->mk);
			cmac128(session->kdk, (unsigned char *)("\x01SK\x00\x80\x00"),
				6, session->sk);

			sha256_digest(session->mk, 16, hashmk);
			sha256_digest(session->sk, 16, hashsk);

			if ( verbose ) {
				if ( debug ) {
					eprintf("MK         = %s\n", hexstring(session->mk, 16));
					eprintf("SK         = %s\n", hexstring(session->sk, 16));
				}
				eprintf("SHA256(MK) = %s\n", hexstring(hashmk, 32));
				eprintf("SHA256(SK) = %s\n", hexstring(hashsk, 32));
			}
		}

	} else {
		eprintf("Attestation failed\n");
		free(msg3);
		free(b64quote);
		return 0;
	}

	free(b64quote);
	free(msg3);

	return 1;
}

/*
 * Read and process message 0 and message 1. These messages are sent by
 * the client concatenated together for efficiency (msg0||msg1).
 */

int process_msg01 (MsgIO *msgio, IAS_Connection *ias, sgx_ra_msg1_t *msg1,
	sgx_ra_msg2_t *msg2, char **sigrl, config_t *config, ra_session_t *session)
{
	struct msg01_struct {
		uint32_t msg0_extended_epid_group_id;
		sgx_ra_msg1_t msg1;
	} *msg01;
	size_t blen= 0;
	char *buffer= NULL;
	unsigned char digest[32], r[32], s[32], gb_ga[128];
	EVP_PKEY *Gb;
	int rv;

	memset(msg2, 0, sizeof(sgx_ra_msg2_t));

	/*
	 * Read our incoming message. We're using base16 encoding/hex strings
	 * so we should end up with sizeof(msg)*2 bytes.
	 */

	fprintf(stderr, "Waiting for msg0||msg1\n");

	rv= msgio->read((void **) &msg01, NULL);
	if ( rv == -1 ) {
		eprintf("system error reading msg0||msg1\n");
		return 0;
	} else if ( rv == 0 ) {
		eprintf("protocol error reading msg0||msg1\n");
		return 0;
	}

	if ( verbose ) {
		edividerWithText("Msg0 Details (from Client)");
		eprintf("msg0.extended_epid_group_id = %u\n",
			 msg01->msg0_extended_epid_group_id);
		edivider();
	}

	/* According to the Intel SGX Developer Reference
	 * "Currently, the only valid extended Intel(R) EPID group ID is zero. The
	 * server should verify this value is zero. If the Intel(R) EPID group ID 
	 * is not zero, the server aborts remote attestation"
	 */

	if ( msg01->msg0_extended_epid_group_id != 0 ) {
		eprintf("msg0 Extended Epid Group ID is not zero.  Exiting.\n");
		free(msg01);
		return 0;
	}

	// Pass msg1 back to the pointer in the caller func
	memcpy(msg1, &msg01->msg1, sizeof(sgx_ra_msg1_t));

	if ( verbose ) {
		edividerWithText("Msg1 Details (from Client)");
		eprintf("msg1.g_a.gx = %s\n",
			hexstring(&msg1->g_a.gx, sizeof(msg1->g_a.gx)));
		eprintf("msg1.g_a.gy = %s\n",
			hexstring(&msg1->g_a.gy, sizeof(msg1->g_a.gy)));
		eprintf("msg1.gid    = %s\n",
			hexstring( &msg1->gid, sizeof(msg1->gid)));
		edivider();
	}

	/* Generate our session key */

	if ( debug ) eprintf("+++ generating session key Gb\n");

	Gb= key_generate();
	if ( Gb == NULL ) {
		eprintf("Could not create a session key\n");
		free(msg01);
		return 0;
	}

	/*
	 * Derive the KDK from the key (Ga) in msg1 and our session key.
	 * An application would normally protect the KDK in memory to 
	 * prevent trivial inspection.
	 */

	if ( debug ) eprintf("+++ deriving KDK\n");

	if ( ! derive_kdk(Gb, session->kdk, msg1->g_a, config) ) {
		eprintf("Could not derive the KDK\n");
		free(msg01);
		return 0;
	}

	if ( debug ) eprintf("+++ KDK = %s\n", hexstring(session->kdk, 16));

	/*
 	 * Derive the SMK from the KDK 
	 * SMK = AES_CMAC(KDK, 0x01 || "SMK" || 0x00 || 0x80 || 0x00) 
	 */

	if ( debug ) eprintf("+++ deriving SMK\n");

	cmac128(session->kdk, (unsigned char *)("\x01SMK\x00\x80\x00"), 7,
		session->smk);

	if ( debug ) eprintf("+++ SMK = %s\n", hexstring(session->smk, 16));

	/*
	 * Build message 2
	 *
	 * A || CMACsmk(A) || SigRL
	 * (148 + 16 + SigRL_length bytes = 164 + SigRL_length bytes)
	 *
	 * where:
	 *
	 * A      = Gb || SPID || TYPE || KDF-ID || SigSP(Gb, Ga) 
	 *          (64 + 16 + 2 + 2 + 64 = 148 bytes)
	 * Ga     = Client enclave's session key
	 *          (32 bytes)
	 * Gb     = Service Provider's session key
	 *          (32 bytes)
	 * SPID   = The Service Provider ID, issued by Intel to the vendor
	 *          (16 bytes)
	 * TYPE   = Quote type (0= linkable, 1= linkable)
	 *          (2 bytes)
	 * KDF-ID = (0x0001= CMAC entropy extraction and key derivation)
	 *          (2 bytes)
	 * SigSP  = ECDSA signature of (Gb.x || Gb.y || Ga.x || Ga.y) as r || s
	 *          (signed with the Service Provider's private key)
	 *          (64 bytes)
	 *
	 * CMACsmk= AES-128-CMAC(A)
	 *          (16 bytes)
	 * 
	 * || denotes concatenation
	 *
	 * Note that all key components (Ga.x, etc.) are in little endian 
	 * format, meaning the byte streams need to be reversed.
	 *
	 * For SigRL, send:
	 *
	 *  SigRL_size || SigRL_contents
	 *
	 * where sigRL_size is a 32-bit uint (4 bytes). This matches the
	 * structure definition in sgx_ra_msg2_t
	 */

	key_to_sgx_ec256(&msg2->g_b, Gb);
	memcpy(&msg2->spid, &config->spid, sizeof(sgx_spid_t));
	msg2->quote_type= config->quote_type;
	msg2->kdf_id= 1;

	/* Get the sigrl */

	if ( ! get_sigrl(ias, config->apiver, msg1->gid, sigrl,
		&msg2->sig_rl_size) ) {

		eprintf("could not retrieve the sigrl\n");
		free(msg01);
		return 0;
	}

	memcpy(gb_ga, &msg2->g_b, 64);
	memcpy(session->g_b, &msg2->g_b, 64);

	memcpy(&gb_ga[64], &msg1->g_a, 64);
	memcpy(session->g_a, &msg1->g_a, 64);

	if ( debug ) eprintf("+++ GbGa = %s\n", hexstring(gb_ga, 128));

	ecdsa_sign(gb_ga, 128, config->service_private_key, r, s, digest);
	reverse_bytes(&msg2->sign_gb_ga.x, r, 32);
	reverse_bytes(&msg2->sign_gb_ga.y, s, 32);

	if ( debug ) {
		eprintf("+++ sha256(GbGa) = %s\n", hexstring(digest, 32));
		eprintf("+++ r = %s\n", hexstring(r, 32));
		eprintf("+++ s = %s\n", hexstring(s, 32));
	}

	/* The "A" component is conveniently at the start of sgx_ra_msg2_t */

	cmac128(session->smk, (unsigned char *) msg2, 148,
		(unsigned char *) &msg2->mac);

	if ( verbose ) {
		edividerWithText("Msg2 Details");
		eprintf("msg2.g_b.gx      = %s\n",
			hexstring(&msg2->g_b.gx, sizeof(msg2->g_b.gx)));
		eprintf("msg2.g_b.gy      = %s\n",
			hexstring(&msg2->g_b.gy, sizeof(msg2->g_b.gy)));
		eprintf("msg2.spid        = %s\n",
			hexstring(&msg2->spid, sizeof(msg2->spid)));
		eprintf("msg2.quote_type  = %s\n",
			hexstring(&msg2->quote_type, sizeof(msg2->quote_type)));
		eprintf("msg2.kdf_id      = %s\n",
			hexstring(&msg2->kdf_id, sizeof(msg2->kdf_id)));
		eprintf("msg2.sign_ga_gb  = %s\n",
			hexstring(&msg2->sign_gb_ga, sizeof(msg2->sign_gb_ga)));
		eprintf("msg2.mac         = %s\n",
			hexstring(&msg2->mac, sizeof(msg2->mac)));
		eprintf("msg2.sig_rl_size = %s\n",
			hexstring(&msg2->sig_rl_size, sizeof(msg2->sig_rl_size)));
		edivider();
	}

	free(msg01);

	return 1;
}

int derive_kdk(EVP_PKEY *Gb, unsigned char kdk[16], sgx_ec256_public_t g_a,
	config_t *config)
{
	unsigned char *Gab_x;
	size_t slen;
	EVP_PKEY *Ga;
	unsigned char cmackey[16];

	memset(cmackey, 0, 16);

	/*
	 * Compute the shared secret using the peer's public key and a generated
	 * public/private key.
	 */

	Ga= key_from_sgx_ec256(&g_a);
	if ( Ga == NULL ) {
		crypto_perror("key_from_sgx_ec256");
		return 0;
	}

	/* The shared secret in a DH exchange is the x-coordinate of Gab */
	Gab_x= key_shared_secret(Gb, Ga, &slen);
	if ( Gab_x == NULL ) {
		crypto_perror("key_shared_secret");
		return 0;
	}

	/* We need it in little endian order, so reverse the bytes. */
	/* We'll do this in-place. */

	if ( debug ) eprintf("+++ shared secret= %s\n", hexstring(Gab_x, slen));

	reverse_bytes(Gab_x, Gab_x, slen);

	if ( debug ) eprintf("+++ reversed     = %s\n", hexstring(Gab_x, slen));

	/* Now hash that to get our KDK (Key Definition Key) */

	/*
	 * KDK = AES_CMAC(0x00000000000000000000000000000000, secret)
	 */

	cmac128(cmackey, Gab_x, slen, kdk);

	return 1;
}

int get_sigrl (IAS_Connection *ias, int version, sgx_epid_group_id_t gid,
	char **sig_rl, uint32_t *sig_rl_size)
{
	IAS_Request *req= NULL;
	int oops= 1;
	string sigrlstr;

	try {
		oops= 0;
		req= new IAS_Request(ias, (uint16_t) version);
	}
	catch (...) {
		oops = 1;
	}

	if (oops) {
		eprintf("Exception while creating IAS request object\n");
		delete req;
		return 0;
	}
 
        ias_error_t ret = IAS_OK;

	while (1) {

		ret =  req->sigrl(*(uint32_t *) gid, sigrlstr);
		if ( debug ) {
			eprintf("+++ RET = %zu\n, ret");
			eprintf("+++ SubscriptionKeyID = %d\n",(int)ias->getSubscriptionKeyID());
                }
	
		if ( ret == IAS_UNAUTHORIZED && (ias->getSubscriptionKeyID() == IAS_Connection::SubscriptionKeyID::Primary))
		{

		        if ( debug ) {
				eprintf("+++ IAS Primary Subscription Key failed with IAS_UNAUTHORIZED\n");
				eprintf("+++ Retrying with IAS Secondary Subscription Key\n");
			}	

			// Retry with Secondary Subscription Key
			ias->SetSubscriptionKeyID(IAS_Connection::SubscriptionKeyID::Secondary);
			continue;
		}	
		else if (ret != IAS_OK ) {

			delete req;
			return 0;
		}

		break;
	}


	*sig_rl= strdup(sigrlstr.c_str());
	if ( *sig_rl == NULL ) {
		delete req;
		return 0;
	}

	*sig_rl_size= (uint32_t ) sigrlstr.length();

	delete req;

	return 1;
}

int get_attestation_report(IAS_Connection *ias, int version,
	const char *b64quote, sgx_ps_sec_prop_desc_t secprop, ra_msg4_t *msg4,
	int strict_trust) 
{
	IAS_Request *req = NULL;
	map<string,string> payload;
	vector<string> messages;
	ias_error_t status;
	string content;

	try {
		req= new IAS_Request(ias, (uint16_t) version);
	}
	catch (...) {
		eprintf("Exception while creating IAS request object\n");
		if ( req != NULL ) delete req;
		return 0;
	}

	payload.insert(make_pair("isvEnclaveQuote", b64quote));
	
	status= req->report(payload, content, messages);
	if ( status == IAS_OK ) {
		JSON reportObj = JSON::Load(content);

		if ( verbose ) {
			edividerWithText("Report Body");
			eprintf("%s\n", content.c_str());
			edivider();
			if ( messages.size() ) {
				edividerWithText("IAS Advisories");
				for (vector<string>::const_iterator i = messages.begin();
					i != messages.end(); ++i ) {

					eprintf("%s\n", i->c_str());
				}
				edivider();
			}
		}

		if ( verbose ) {
			edividerWithText("IAS Report - JSON - Required Fields");
			if ( version >= 3 ) {
				eprintf("version               = %d\n",
					reportObj["version"].ToInt());
			}
			eprintf("id:                   = %s\n",
				reportObj["id"].ToString().c_str());
			eprintf("timestamp             = %s\n",
				reportObj["timestamp"].ToString().c_str());
			eprintf("isvEnclaveQuoteStatus = %s\n",
				reportObj["isvEnclaveQuoteStatus"].ToString().c_str());
			eprintf("isvEnclaveQuoteBody   = %s\n",
				reportObj["isvEnclaveQuoteBody"].ToString().c_str());

			edividerWithText("IAS Report - JSON - Optional Fields");

			eprintf("platformInfoBlob  = %s\n",
				reportObj["platformInfoBlob"].ToString().c_str());
			eprintf("revocationReason  = %s\n",
				reportObj["revocationReason"].ToString().c_str());
			eprintf("pseManifestStatus = %s\n",
				reportObj["pseManifestStatus"].ToString().c_str());
			eprintf("pseManifestHash   = %s\n",
				reportObj["pseManifestHash"].ToString().c_str());
			eprintf("nonce             = %s\n",
				reportObj["nonce"].ToString().c_str());
			eprintf("epidPseudonym     = %s\n",
				reportObj["epidPseudonym"].ToString().c_str());
			edivider();
		}

    /*
     * If the report returned a version number (API v3 and above), make
     * sure it matches the API version we used to fetch the report.
	 *
	 * For API v3 and up, this field MUST be in the report.
     */

	if ( reportObj.hasKey("version") ) {
		unsigned int rversion= (unsigned int) reportObj["version"].ToInt();
		if ( verbose )
			eprintf("+++ Verifying report version against API version\n");
		if ( version != rversion ) {
			eprintf("Report version %u does not match API version %u\n",
				rversion , version);
			delete req;
			return 0;
		}
	} else if ( version >= 3 ) {
		eprintf("attestation report version required for API version >= 3\n");
		delete req;
		return 0;
	}

	/*
	 * This sample's attestion policy is based on isvEnclaveQuoteStatus:
	 * 
	 *   1) if "OK" then return "Trusted"
	 *
 	 *   2) if "CONFIGURATION_NEEDED" then return
	 *       "NotTrusted_ItsComplicated" when in --strict-trust-mode
	 *        and "Trusted_ItsComplicated" otherwise
	 *
	 *   3) return "NotTrusted" for all other responses
	 *
	 * 
	 * ItsComplicated means the client is not trusted, but can 
	 * conceivable take action that will allow it to be trusted
	 * (such as a BIOS update).
 	 */

	/*
	 * Simply check to see if status is OK, else enclave considered 
	 * not trusted
	 */

	memset(msg4, 0, sizeof(ra_msg4_t));

	if ( verbose ) edividerWithText("ISV Enclave Trust Status");

	if ( !(reportObj["isvEnclaveQuoteStatus"].ToString().compare("OK"))) {
		msg4->status = Trusted;
		if ( verbose ) eprintf("Enclave TRUSTED\n");
	} else if ( !(reportObj["isvEnclaveQuoteStatus"].ToString().compare("CONFIGURATION_NEEDED"))) {
		if ( strict_trust ) {
			msg4->status = NotTrusted_ItsComplicated;
			if ( verbose ) eprintf("Enclave NOT TRUSTED and COMPLICATED - Reason: %s\n",
				reportObj["isvEnclaveQuoteStatus"].ToString().c_str());
		} else {
			if ( verbose ) eprintf("Enclave TRUSTED and COMPLICATED - Reason: %s\n",
				reportObj["isvEnclaveQuoteStatus"].ToString().c_str());
			msg4->status = Trusted_ItsComplicated;
		}
	} else if ( !(reportObj["isvEnclaveQuoteStatus"].ToString().compare("GROUP_OUT_OF_DATE"))) {
		/* THIS ERROR MUST BE IGNORED BECAUSE CLEARLY CORRUPTED */
		msg4->status = NotTrusted_ItsComplicated;
		flag_BIOS = true;
		cerr << "*** CAUTION: THIS ERROR MUST BE IGNORED BECAUSE CLEARLY BUGGED ***" << endl;
		//if ( verbose ) eprintf("Enclave NOT TRUSTED and COMPLICATED - Reason: %s\n",
		if(verbose) eprintf("IAS returned unjust error message. Please igonore this. Message: %s\n",
			reportObj["isvEnclaveQuoteStatus"].ToString().c_str());
	} else {
		msg4->status = NotTrusted;
		if ( verbose ) eprintf("Enclave NOT TRUSTED - Reason: %s\n",
			reportObj["isvEnclaveQuoteStatus"].ToString().c_str());
	}


	/* Check to see if a platformInfoBlob was sent back as part of the
	 * response */

	if (!reportObj["platformInfoBlob"].IsNull()) {
		if ( verbose ) eprintf("A Platform Info Blob (PIB) was provided by the IAS\n");

		/* The platformInfoBlob has two parts, a TVL Header (4 bytes),
		 * and TLV Payload (variable) */

		string pibBuff = reportObj["platformInfoBlob"].ToString();

		/* remove the TLV Header (8 base16 chars, ie. 4 bytes) from
		 * the PIB Buff. */

		pibBuff.erase(pibBuff.begin(), pibBuff.begin() + (4*2)); 

		int ret = from_hexstring ((unsigned char *)&msg4->platformInfoBlob, 
			pibBuff.c_str(), pibBuff.length()/2);
	} else {
		if ( verbose ) eprintf("A Platform Info Blob (PIB) was NOT provided by the IAS\n");
	}

		delete req;
		return 1;
	}

	eprintf("attestation query returned %lu: \n", status);

	switch(status) {
		case IAS_QUERY_FAILED:
			eprintf("Could not query IAS\n");
			break;
		case IAS_BADREQUEST:
			eprintf("Invalid payload\n");
			break;
		case IAS_UNAUTHORIZED:
			eprintf("Failed to authenticate or authorize request\n");
			break;
		case IAS_SERVER_ERR:
			eprintf("An internal error occurred on the IAS server\n");
			break;
		case IAS_UNAVAILABLE:
			eprintf("Service is currently not able to process the request. Try again later.\n");
			break;
		case IAS_INTERNAL_ERROR:
			eprintf("An internal error occurred while processing the IAS response\n");
			break;
		case IAS_BAD_CERTIFICATE:
			eprintf("The signing certificate could not be validated\n");
			break;
		case IAS_BAD_SIGNATURE:
			eprintf("The report signature could not be validated\n");
			break;
		default:
			if ( status >= 100 && status < 600 ) {
				eprintf("Unexpected HTTP response code\n");
			} else {
				eprintf("An unknown error occurred.\n");
			}
	}

	delete req;

	return 0;
}

// Break a URL into server and port. NOTE: This is a simplistic algorithm.

int get_proxy(char **server, unsigned int *port, const char *url)
{
	size_t idx1, idx2;
	string lcurl, proto, srv, sport;

	if (url == NULL) return 0;

	lcurl = string(url);
	// Make lower case for sanity
	transform(lcurl.begin(), lcurl.end(), lcurl.begin(), ::tolower);

	idx1= lcurl.find_first_of(":");
	proto = lcurl.substr(0, idx1);
	if (proto == "https") *port = 443;
	else if (proto == "http") *port = 80;
	else return 0;

	idx1 = lcurl.find_first_not_of("/", idx1 + 1);
	if (idx1 == string::npos) return 0;
	
	idx2 = lcurl.find_first_of(":", idx1);
	if (idx2 == string::npos) {
		idx2 = lcurl.find_first_of("/", idx1);
		if (idx2 == string::npos) srv = lcurl.substr(idx1);
		else srv = lcurl.substr(idx1, idx2 - idx1);
	}
	else {
		srv= lcurl.substr(idx1, idx2 - idx1);
		idx1 = idx2+1;
		idx2 = lcurl.find_first_of("/", idx1);

		if (idx2 == string::npos) sport = lcurl.substr(idx1);
		else sport = lcurl.substr(idx1, idx2 - idx1);

		try {
			*port = (unsigned int) ::stoul(sport);
		}
		catch (...) {
			return 0;
		}
	}

	try {
		*server = new char[srv.length()+1];
	}
	catch (...) {
		return 0;
	}

	memcpy(*server, srv.c_str(), srv.length());
	(*server)[srv.length()] = 0;

	return 1;
}

int encrypt_data_for_ISV(unsigned char *plaintext, int plaintext_len,
	unsigned char *key, unsigned char *iv, unsigned char *ciphertext, uint8_t* tag)
{
	EVP_CIPHER_CTX *ctx;

	int len;

	int ciphertext_len;

	/* Create and initialise the context */
	if(!(ctx = EVP_CIPHER_CTX_new()))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}

	/* Initialise the encryption operation. IMPORTANT - ensure you use a key
	* and IV size appropriate for your cipher
	* In this example we are using 256 bit AES (i.e. a 256 bit key). The
	* IV size for *most* modes is the same as the block size. For AES this
	* is 128 bits */
	if(1 != EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, key, iv))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}

	/* Provide the message to be encrypted, and obtain the encrypted output.
	* EVP_EncryptUpdate can be called multiple times if necessary
	*/
	if(1 != EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}
	ciphertext_len = len;

	/* Finalise the encryption. Further ciphertext bytes may be written at
	* this stage.
	*/
	if(1 != EVP_EncryptFinal_ex(ctx, ciphertext + len, &len))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}
	ciphertext_len += len;

	/*Obtain MAC tag*/
	if(1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}

	/* Clean up */
	EVP_CIPHER_CTX_free(ctx);

	return ciphertext_len;
}

int decrypt_cipher_from_ISV(uint8_t *ciphertext, int ciphertext_len, uint8_t *key,
	uint8_t *iv, int iv_len, uint8_t *tag, uint8_t *plaintext)
{
	EVP_CIPHER_CTX *ctx;
	int len;
	int plaintext_len;
	int ret;

	/* Create and initialise the context */
	if(!(ctx = EVP_CIPHER_CTX_new()))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}

	/* Initialise the decryption operation. */
	if(!EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}

	/* Set IV length. Not necessary if this is 12 bytes (96 bits) */
	if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}

	/* Initialise key and IV */
	if(!EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}

	/* Provide the message to be decrypted, and obtain the plaintext output.
	 * EVP_DecryptUpdate can be called multiple times if necessary
	 */
	if(!EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}
	plaintext_len = len;

	/* Set expected tag value. Works in OpenSSL 1.0.1d and later */
	if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag))
	{
		ERR_print_errors_fp(stderr);
		return -1;
	}

	/* Finalise the decryption. A positive return value indicates success,
	 * anything else is a failure - the plaintext is not trustworthy.
	 */
	ret = EVP_DecryptFinal_ex(ctx, plaintext + len, &len);

	/* Clean up */
	EVP_CIPHER_CTX_free(ctx);

	if(ret > 0)
	{
		/* Success */
		plaintext_len += len;
		return plaintext_len;
	}
	else
	{
		/* Verify failed */
		return -1;
	}
}

/*referred: https://ryozi.hatenadiary.jp/entry/20101203/1291380670 in 12/30/2018*/
int base64_encrypt(uint8_t *src, int srclen, uint8_t *dst, int dstlen)
{
	const char Base64char[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i,j;
	int calclength = (srclen/3*4) + (srclen%3?4:0);
	if(calclength > dstlen) return -1;
	
	j=0;
	for(i=0; i+2<srclen; i+=3){
		dst[j++] = Base64char[ (src[i] >> 2) & 0x3F ];
		dst[j++] = Base64char[ (src[i] << 4 | src[i+1] >> 4) & 0x3F ];
		dst[j++] = Base64char[ (src[i+1] << 2 | src[i+2] >> 6) & 0x3F ];
		dst[j++] = Base64char[ (src[i+2]) & 0x3F ];
	}
	
	if(i<srclen){
		dst[j++] = Base64char[ (src[i] >> 2) & 0x3F ];
		if(i+1<srclen){
			dst[j++] = Base64char[ (src[i] << 4 | src[i+1] >> 4) & 0x3F ];
			if(i+2<srclen){
				dst[j++] = Base64char[ (src[i+1] << 2 | src[i+2] >> 6) & 0x3F ];
			}else{
				dst[j++] = Base64char[ (src[i+1] << 2) & 0x3F ];
			}
		}else{
			dst[j++] = Base64char[ (src[i] << 4) & 0x3F ];
		}
	}
	while(j%4) dst[j++] = '=';
	
	if(j<dstlen) dst[j] = '\0';
	return j;
}

int base64_decrypt(uint8_t *src, int srclen, uint8_t *dst, int dstlen)
{
	const unsigned char Base64num[] = {
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x3E,0xFF,0xFF,0xFF,0x3F,
		0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0xFF,0xFF,0xFF,0x00,0xFF,0xFF,
		0xFF,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,
		0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0xFF,0xFF,0xFF,0xFF,0xFF,
		0xFF,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
		0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,0x33,0xFF,0xFF,0xFF,0xFF,0xFF,
		
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
		0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
	};
	int calclength = (srclen/4*3);
	int i,j;
	if(calclength > dstlen || srclen % 4 != 0) return 0;
	
	j=0;
	for(i=0; i+3<srclen; i+=4){
		if((Base64num[src[i+0]]|Base64num[src[i+1]]|Base64num[src[i+2]]|Base64num[src[i+3]]) > 0x3F){
			return -1;
		}
		dst[j++] = Base64num[src[i+0]]<<2 | Base64num[src[i+1]] >> 4;
		dst[j++] = Base64num[src[i+1]]<<4 | Base64num[src[i+2]] >> 2;
		dst[j++] = Base64num[src[i+2]]<<6 | Base64num[src[i+3]];
	}
	
	if(j<dstlen) dst[j] = '\0';
	return j;
}


uint8_t* generate_nonce(int size)
{
	random_device rnd;
	mt19937 mt(rnd());
	uniform_int_distribution<> randchar(0, 255);

	uint8_t* nonce_heap = new uint8_t[size];

	for(int i = 0; i < size; i++)
	{
		nonce_heap[i] = (uint8_t)randchar(mt);
	}

	/*
	cout << "Generated nonce is: " << endl;
	BIO_dump_fp(stdout, (const char*)nonce_heap, size);
	cout << endl;
	*/

	return nonce_heap;
}


string generate_random_filename()
{
    random_device rnd;
    mt19937 mt(rnd());
    uniform_int_distribution<> randchar('0', 'z');

    string filename = "";
    char rch;

    for(int i = 0; i < 16; i++)
    {
        rch = (char)randchar(mt);

        while((rch >= ':' && rch <= '@') || (rch >= '[' && rch <= '`'))
        {
            rch = (char)randchar(mt);
        }

        filename += rch;
    }

    return filename;
}


int send_login_info(MsgIO *msgio, ra_session_t session, 
	string *username, string *datatype)
{
	ifstream fin_login;
	int mode_flag = -1;

	fin_login.open("login.ini", ios::in);

	if(!fin_login)
	{
		cerr << "FileOpenError: Failed to open \"login.ini\".\n" << endl;
		return -1;
	}
	else
	{
		cout << "Opened \"login.ini\" successfully." << endl;
	}

	//start checking whether or not violate the length limit
	string tmp, login_info;

	//username
	getline(fin_login, tmp);

	if(tmp.length() > 20)
	{
		cerr << "Username must be 20 or less characters." << endl;
		return -1;
	}

	*username = tmp;

	login_info += tmp;
	login_info += "\n";
	tmp = "";

	//password
	getline(fin_login, tmp);

	if(tmp.length() > 20)
	{
		cerr << "Password must be 20 or less characters." << endl;
		return -1;
	}

	login_info += tmp;
	login_info += "\n";
	tmp = "";

	//privilege
	getline(fin_login, tmp);

	if(tmp != "O" && tmp != "R")
	{
		cerr << "Privilege must be designated only by \"O\" (Data Owner) or \"R\" (Researcher)." << endl;
		return -1;
	}

	login_info += tmp;
	login_info += "\n";
	mode_flag = 1;

	//datatype, if Owner
	if(tmp == "O")
	{
		tmp = "";
		getline(fin_login, tmp);

		if(tmp != "integer" && tmp != "genome" && tmp != "FASTA"
			&& tmp != "vcf" && tmp != "inquiry" && tmp != "download")
		{
			cerr << "Datatype must be designated only by following: " << endl;
			cout << "integer, genome, FASTA, vcf, inquiry, download" << endl;
			cout << "tmp: " << tmp << endl;
			exit(1);
		}

		*datatype += tmp;
		login_info += tmp;

		if(*datatype == "download")
		{
			login_info += "\n";
			getline(fin_login, tmp);
			login_info += tmp;
		}

		mode_flag = 0;
	}

	cout << endl;
	cout << "Loaded login info successfully." << endl;
	cout << endl;

	//convert login info to uint8_t*
	const uint8_t* dummy_plain;
	uint8_t* login_info_plain;

	dummy_plain = reinterpret_cast<unsigned const char*>(login_info.c_str());
	login_info_plain = const_cast<unsigned char*>(dummy_plain);


	//start encrypt
	unsigned char* sp_key = session.sk;
	unsigned char* sp_iv;
	unsigned char login_info_cipher[128];
	int ciphertext_len, tag_len = 16;
	uint8_t tag[16] = {'\0'};

	cout << endl;
	cout << "Start processing login info to send." << endl;
	cout << "Generate initialization vector." << endl;
	sp_iv = generate_nonce(12);

	/*AES/GCM's cipher length is equal to the length of plain text*/
	ciphertext_len = encrypt_data_for_ISV(login_info_plain, strlen((char*)login_info_plain), 
		sp_key, sp_iv, login_info_cipher, tag);

	if(ciphertext_len == -1)
	{
		cerr << "Failed to operate encryption." << endl;
		cerr << "Abort program..." << endl;

		return -1;
	}
	
	cout << "Encrypted data successfully. Cipher text is:" << endl;
	BIO_dump_fp(stdout, (const char*)login_info_cipher, ciphertext_len);

	/*convert data to base64 for sending with msgio correctly*/
	uint8_t cipherb64[256] = {'\0'};
	int b64dst_len = 256, b64_len;

	b64_len = base64_encrypt(login_info_cipher, ciphertext_len, cipherb64, b64dst_len);

	cout << "==========================================================" << endl;
	cout << "Base64 format of cipher is: " << endl;
	cout << cipherb64 << endl << endl;
	cout << "Length of cipher text is: " << ciphertext_len << endl;
	cout << "Length of base64-ed cipher is: " << b64_len << endl << endl;
	cout << "==========================================================" << endl;

	/*Next, convert IV to base64 format*/
	uint8_t ivb64[64] = {'\0'}; //maybe sufficient with 32-size
	int ivb64dst_len = 64, ivb64_len;

	ivb64_len = base64_encrypt(sp_iv, 12, ivb64, ivb64dst_len);

	cout << "Base64 format of initialization vector is: " << endl;
	cout << ivb64 << endl << endl;
	cout << "Length of base64-ed IV is: " << ivb64_len << endl << endl;
	cout << "==========================================================" << endl;

	/*Also convert tag to base64 format*/
	uint8_t tagb64[64] = {'\0'}; //maybe sufficient with 32-size
	int tagb64dst_len = 64, tagb64_len;

	tagb64_len = base64_encrypt(tag, 16, tagb64, tagb64dst_len);

	cout << "Base64 format of tag is: " << endl;
	cout << tagb64 << endl << endl;
	cout << "Length of base64-ed tag is: " << tagb64_len << endl << endl;
	cout << "==========================================================" << endl;
	
	/*In addition to that, need to convert cipher's length to base64*/
	uint8_t deflenb64[128] = {'\0'}; 
	uint8_t *uint8tdeflen;
	int deflenb64dst_len = 32, deflenb64_len;

	uint8tdeflen = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(to_string(ciphertext_len).c_str()));

	cout << "uint8_t-ed default cipher length is: " << uint8tdeflen << endl << endl;

	deflenb64_len = base64_encrypt(uint8tdeflen, strlen((char*)uint8tdeflen), deflenb64, 32);
	cout << "Base64 format of default cipher length is: " << endl;
	cout << deflenb64 << endl << endl;
	cout << "Length of base64-ed default cipher length is: " << deflenb64_len << endl;
	cout << "==========================================================" << endl;


	/*Send base64-ed secret, its length, MAC tag and its length to ISV*/

	/*cipher*/
	cout << "==========================================================" << endl;
	cout << "Send encrypted file to ISV." << endl;

	cout << "Encrypted secret to be sent in base64 is (display again): " << endl;
	msgio->send(cipherb64, strlen((char*)cipherb64));

	cout << "Complete sending message." << endl;
	cout << "Please wait for 0.25 sec." << endl;
	cout << "==========================================================" << endl;

	usleep(250000);

	/*IV*/
	cout << "Initialization vector to be sent is: " << endl;
	msgio->send(ivb64, strlen((char*)ivb64));

	cout << "Complete sending IV." << endl;
	cout << "Please wait for 0.25 sec." << endl;
	cout << "==========================================================" << endl;

	usleep(250000);
	
	/*MAC tag*/
	cout << "Tag to be sent is: (display again)" << endl;
	msgio->send(tagb64, strlen((char*)tagb64));

	cout << "Complete sending MAC tag." << endl;
	cout << "Please wait for 0.25 sec." << endl;
	cout << "==========================================================" << endl;

	usleep(250000);

	/*default cipher length*/
	cout << "Default cipher's length to be sent is: " << endl;
	msgio->send(deflenb64, deflenb64_len);

	cout << "Complete sending default cipher length." << endl;
	cout << "==========================================================" << endl;

	return mode_flag;
}


int process_vcf(string *tar_filename, string *access_list, uint8_t *sp_key, 
	uint8_t *iv_array, uint8_t *tag_array, ifstream *vcf_ifs)
{
    size_t est_divnum = 0;
    vcf_ifs->seekg(0, ios::end);
    est_divnum = vcf_ifs->tellg();
    vcf_ifs->seekg(0, ios::beg);

    if(est_divnum % DIV_UNIT != 0)
    {
        est_divnum /= DIV_UNIT;
        est_divnum += 1;
    }
    else
    {
        est_divnum /= DIV_UNIT;
    }

    /* randomly generate filename for tarball */
    *tar_filename = generate_random_filename();

	/* Create temporary directory */
    string mkdir_cmd = "mkdir -p encrypted_vcf/" + *tar_filename;
    int sys_ret = system(mkdir_cmd.c_str());

    if(!WIFEXITED(sys_ret))
    {
        cerr << "Failed to create temporary directory." << endl;
        return -1;
    }

    /* start vcf division */
    int divided_total = 0;
    int part_size = 0;
    string vcf_line, divided_vcf;
    
    string divided_filename = "encrypted_vcf/";
    divided_filename += *tar_filename;
    divided_filename += "/";
    divided_filename += *tar_filename;
    divided_filename += ".";
    divided_filename += to_string(divided_total);

    ofstream div_ofs(divided_filename, ios::out | ios::binary);


    while(getline(*vcf_ifs, vcf_line))
    {
        divided_vcf += vcf_line + '\n';
        part_size += vcf_line.length() + 1;

        if(part_size > DIV_UNIT)
		{
			/* align divided_vcf's size to CHUNK_SIZE byte */
			/*
				size_t pas_sz = CHUNK_SIZE - part_size;

				divided_vcf += "\n";

				for(int i = 0; i < pad_sz - 1; i++)
				{
					divided_vcf += "#";
				}
			
			*/
            uint8_t *iv_temp = new uint8_t[12]();
            uint8_t *tag_temp = new uint8_t[16]();
            uint8_t *vcf_cipher = new uint8_t[30000000]();
            int vcf_cipher_len = 0;

            iv_temp = generate_nonce(12);


            for(int i = 0; i < 12; i++)
            {
                iv_array[divided_total * 12 + i] = iv_temp[i];
            }

            vcf_cipher_len = encrypt_data_for_ISV((uint8_t*)divided_vcf.c_str(),
                part_size, sp_key, iv_temp, vcf_cipher, tag_temp);

            for(int i = 0; i < 16; i++)
            {
                tag_array[divided_total * 16 + i] = tag_temp[i];
            }

            div_ofs.write((char*)vcf_cipher, vcf_cipher_len);

			cout << "Processed " << divided_filename << " successfully.\n";
			/*
			cout << "Tail length -> " << divided_vcf.length() << endl;
			cout << "part_size -> " << part_size << endl << endl;
			*/

            divided_vcf = "";
            part_size = 0;

            divided_total++;

            divided_filename = "encrypted_vcf/";
            divided_filename += *tar_filename;
            divided_filename += "/";
            divided_filename += *tar_filename;
            divided_filename += ".";
            divided_filename += to_string(divided_total);


            div_ofs.close();

			div_ofs.open(divided_filename, ios::out);


            if(!div_ofs)
            {
                cerr << "Failed to output encrypted vcf." << endl;
                return -1;
            }

            delete(iv_temp);
            delete(tag_temp);
            delete(vcf_cipher);
        }
    }


	/* encrypt remained vcf */
    uint8_t *iv_temp = new uint8_t[12]();
    uint8_t *tag_temp = new uint8_t[16]();
    uint8_t *vcf_cipher = new uint8_t[30000000]();
    int vcf_cipher_len = 0;

	/* align divided_vcf's size to CHUNK_SIZE byte */
	/*
		size_t pad_sz = CHUNK_SIZE - part_size;

		divided_vcf += "\n";

		for(int i = 0; i < pad_sz; i++)
		{
			divided_vcf += "#";
		}
	*/
	

    iv_temp = generate_nonce(12);

	for(int i = 0; i < 12; i++)
    {
    	iv_array[divided_total * 12 + i] = iv_temp[i];
    }


    vcf_cipher_len = encrypt_data_for_ISV((uint8_t*)divided_vcf.c_str(),
        part_size, sp_key, iv_temp, vcf_cipher, tag_temp);
	
	uint8_t *plaintext = new uint8_t[divided_vcf.length() + 1]();

	int plain_size = decrypt_cipher_from_ISV(vcf_cipher, 
		divided_vcf.length(), sp_key, iv_temp, 12, tag_temp, plaintext);

	/*
	cout << plaintext << endl;
	cout << "\n\npart_size -> " << part_size << endl;
	cout << "actual length -> " << divided_vcf.length() << endl << endl;
	*/

	for(int i = 0; i < 16; i++)
    {
        tag_array[divided_total * 16 + i] = tag_temp[i];
    }

	div_ofs.write((char*)vcf_cipher, vcf_cipher_len);

	cout << "Processed " << divided_filename << " successfully.\n";

	divided_vcf = "";
	part_size = 0;

	divided_total++;

	div_ofs.close();


	string tar_cmd = "tar --remove-files -cvf /tmp/";
	tar_cmd += *tar_filename;
	tar_cmd += ".tar ";
	tar_cmd += "encrypted_vcf/";
	tar_cmd += *tar_filename;

	int tar_ret = system(tar_cmd.c_str());

	if(!WIFEXITED(tar_ret))
	{
		cerr << "Failed to generate tarball." << endl;
		return -1;
	}

	delete(iv_temp);
	delete(tag_temp);
	delete(vcf_cipher);


	return divided_total;
}



#ifndef _WIN32

/* We don't care which signal it is since we're shutting down regardless */

void cleanup_and_exit(int signo)
{
	/* Signal-safe, and we don't care if it fails or is a partial write. */

	ssize_t bytes= write(STDERR_FILENO, "\nterminating\n", 13);

	/*
	 * This destructor consists of signal-safe system calls (close,
	 * shutdown).
	 */

	delete msgio;

	exit(1);
}
#endif

#define NNL <<endl<<endl<<
#define NL <<endl<<

void usage () 
{
	cerr << "usage: sp [ options ] [ port ]" NL
"Required:" NL
"  -A, --ias-signing-cafile=FILE" NL
"                           Specify the IAS Report Signing CA file." NNL
"  -N, --mrsigner=HEXSTRING" NL
"                           Specify the MRSIGNER value of encalves that" NL
"                           are allowed to attest. Enclaves signed by" NL
"                           other signing keys are rejected." NNL
"  -R, --isv-product-id=INT" NL
"                           Specify the ISV Product Id for the service." NL
"                           Only Enclaves built with this Product Id" NL
"                           will be accepted." NNL
"  -V, --min-isv-svn=INT" NL
"                           The minimum ISV SVN that the service provider" NL
"                           will accept. Enclaves with a lower ISV SVN" NL
"                           are rejected." NNL
"Required (one of):" NL
"  -S, --spid-file=FILE     Set the SPID from a file containg a 32-byte" NL
"                           ASCII hex string." NNL
"  -s, --spid=HEXSTRING     Set the SPID from a 32-byte ASCII hex string." NNL
"Required (one of):" NL
"  -I, --ias-pri-api-key-file=FILE" NL
"                           Set the IAS Primary Subscription Key from a" NL
"                           file containing a 32-byte ASCII hex string." NNL
"  -i, --ias-pri-api-key=HEXSTRING" NL
"                           Set the IAS Primary Subscription Key from a" NL
"                           32-byte ASCII hex string." NNL
"Required (one of):" NL
"  -J, --ias-sec-api-key-file=FILE" NL
"                           Set the IAS Secondary Subscription Key from a" NL
"                           file containing a 32-byte ASCII hex string." NNL
"  -j, --ias-sec-api-key=HEXSTRING" NL
"                           Set the IAS Secondary Subscription Key from a" NL
"                           32-byte ASCII hex string." NNL
"Optional:" NL
"  -B, --ca-bundle-file=FILE" NL
"                           Use the CA certificate bundle at FILE (default:" NL
"                           " << DEFAULT_CA_BUNDLE << ")" NNL
"  -D, --no-debug-enclave   Reject Debug-mode enclaves (default: accept)" NNL
"  -G, --list-agents        List available user agent names for --user-agent" NNL
"  -K, --service-key-file=FILE" NL
"                           The private key file for the service in PEM" NL
"                           format (default: use hardcoded key). The " NL
"                           client must be given the corresponding public" NL
"                           key. Can't combine with --key." NNL
"  -P, --production         Query the production IAS server instead of dev." NNL
"  -X, --strict-trust-mode  Don't trust enclaves that receive a " NL
"                           CONFIGURATION_NEEDED response from IAS " NL
"                           (default: trust)" NNL
"  -d, --debug              Print debug information to stderr." NNL
"  -g, --user-agent=NAME    Use NAME as the user agent for contacting IAS." NNL
"  -k, --key=HEXSTRING      The private key as a hex string. See --key-file" NL
"                           for notes. Can't combine with --key-file." NNL
"  -l, --linkable           Request a linkable quote (default: unlinkable)." NNL
"  -p, --proxy=PROXYURL     Use the proxy server at PROXYURL when contacting" NL
"                           IAS. Can't combine with --no-proxy" NNL
"  -r, --api-version=N      Use version N of the IAS API (default: " << to_string(IAS_API_DEF_VERSION) << ")" NNL
"  -v, --verbose            Be verbose. Print message structure details and" NL
"                           the results of intermediate operations to stderr." NNL
"  -x, --no-proxy           Do not use a proxy (force a direct connection), " NL
"                           overriding environment." NNL
"  -z  --stdio              Read from stdin and write to stdout instead of" NL
"                           running as a network server." <<endl;

	::exit(1);
}

/* vim: ts=4: */

