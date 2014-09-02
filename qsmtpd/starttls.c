/** \file starttls.c
 \brief functions for STARTTLS SMTP command
 */

#include <qsmtpd/starttls.h>

#include <control.h>
#include <log.h>
#include <netio.h>
#include <qdns.h>
#include <qsmtpd/addrparse.h>
#include <qsmtpd/qsmtpd.h>
#include <qsmtpd/syntax.h>
#include <ssl_timeoutio.h>
#include <tls.h>
#include <version.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static RSA *
tmp_rsa_cb(SSL *s __attribute__ ((unused)), int export, int keylen)
{
	if (!export)
		keylen = 512;
	if (keylen == 512) {
		FILE *in = fdopen(openat(controldir_fd, "rsa512.pem", O_RDONLY | O_CLOEXEC), "r");
		if (in) {
			RSA *rsa = PEM_read_RSAPrivateKey(in, NULL, NULL, NULL);

			fclose(in);

			if (rsa)
				return rsa;
		}
	}
	return RSA_generate_key(keylen, RSA_F4, NULL, NULL);
}

static DH *
tmp_dh_cb(SSL *s __attribute__ ((unused)), int export, int keylen)
{
	FILE *in = NULL;

	if (!export)
		keylen = 1024;
	if (keylen == 512) {
		in = fdopen(openat(controldir_fd, "dh512.pem", O_RDONLY | O_CLOEXEC), "r");
	} else if (keylen == 1024) {
		in = fdopen(openat(controldir_fd, "dh1024.pem", O_RDONLY | O_CLOEXEC), "r");
	}
	if (in) {
		DH *dh = PEM_read_DHparams(in, NULL, NULL, NULL);

		fclose(in);

		if (dh)
			return dh;
	}
	return DH_generate_parameters(keylen, DH_GENERATOR_2, NULL, NULL);
}

static int __attribute__((nonnull(1, 2)))
tls_out(const char *s1, const char *s2)
{
	const char *msg[] = {"454 4.3.0 TLS ", s1, ": ", s2, NULL};

	return net_writen(msg);
}

static int __attribute__((nonnull(1)))
tls_err(const char *s)
{
	return tls_out(s, ssl_error());
}

#define CLIENTCA "control/clientca.pem"
#define CLIENTCRL "control/clientcrl.pem"

static int ssl_verified;

/**
 * @brief verify is authenticated to relay by SSL certificate
 *
 * @retval <1 error code
 * @retval 0 if client is not authenticated
 * @retval >0 if client is authenticated
 */
int
tls_verify(void)
{
	char **clients;
	STACK_OF(X509_NAME) *sk;
	int tlsrelay = 0;

	if (!ssl || ssl_verified || is_authenticated_client())
		return 0;
	ssl_verified = 1; /* don't do this twice */

	/* request client cert to see if it can be verified by one of our CAs
	 * and the associated email address matches an entry in tlsclients */
	if (loadlistfd(openat(controldir_fd, "tlsclients", O_RDONLY | O_CLOEXEC), &clients, checkaddr) < 0)
		return -errno;

	if (clients == NULL)
		return 0;

	sk = SSL_load_client_CA_file(CLIENTCA);
	if (sk == NULL) {
		/* if CLIENTCA contains all the standard root certificates, a
		 * 0.9.6b client might fail with SSL_R_EXCESSIVE_MESSAGE_SIZE;
		 * it is probably due to 0.9.6b supporting only 8k key exchange
		 * data while the 0.9.6c release increases that limit to 100k */
		free(clients);
		return 0;
	}

	/* FIXME: this leaks sk */

	SSL_set_client_CA_list(ssl, sk);
	SSL_set_verify(ssl, SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE, NULL);

	do { /* one iteration */
		X509 *peercert;
		X509_NAME *subj;
		cstring email = { .len = 0, .s = NULL };
		int n;

		if (SSL_set_session_id_context(ssl, VERSIONSTRING, strlen(VERSIONSTRING)) != 1) {
			const char *err = ssl_strerror();
			tlsrelay = tls_out("setting session id failed", err) ? -errno : -EPROTO;
			break;
		}

		/* renegotiate to force the client to send it's certificate */
		n = ssl_timeoutrehandshake(timeout);
		if (n == -ETIMEDOUT) {
			dieerror(ETIMEDOUT);
		} else if (n < 0) {
			const char *err = ssl_strerror();
			tlsrelay = tls_out("rehandshake failed", err) ? -errno : n;
			break;
		}

		if (SSL_get_verify_result(ssl) != X509_V_OK)
			break;

		peercert = SSL_get_peer_certificate(ssl);
		if (!peercert)
			break;

		subj = X509_get_subject_name(peercert);
		/* try if this is a user authenticating with a personal certificate */
		n = X509_NAME_get_index_by_NID(subj, NID_pkcs9_emailAddress, -1);
		if (n < 0)
			/* seems not, maybe it is a host authenticating for relaying? */
			n = X509_NAME_get_index_by_NID(subj, NID_commonName, -1);
		if (n >= 0) {
			const ASN1_STRING *s = X509_NAME_get_entry(subj, n)->value;
			if (s) {
				email.len = (M_ASN1_STRING_length(s) > 0) ? M_ASN1_STRING_length(s) : 0;
				email.s = M_ASN1_STRING_data(s);
			}
		}

		if (email.len != 0) {
			unsigned int i = 0;

			while (clients[i]) {
				if (strcmp(email.s, clients[i]) == 0) {
					xmitstat.tlsclient = strdup(email.s);

					if (xmitstat.tlsclient == NULL)
						tlsrelay = -ENOMEM;
					else
						tlsrelay = 1;
					break;
				}
				i++;
			}
		}

		X509_free(peercert);
	} while (0);

	free(clients);
	SSL_set_client_CA_list(ssl, NULL);
	SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);

	return tlsrelay;
}

static int
tls_init()
{
	SSL *myssl;
	SSL_CTX *ctx;
	const char *ciphers = "DEFAULT";
	const char *prot;
	string saciphers;
	unsigned int l;
	X509_STORE *store;
	X509_LOOKUP *lookup;
	char *newprot;
	const char ciphfn[] = "tlsserverciphers";
	int j;

	SSL_library_init();
	STREMPTY(saciphers);

	/* a new SSL context with the bare minimum of options */
	ctx = SSL_CTX_new(SSLv23_server_method());
	if (!ctx) {
		return tls_err("unable to initialize ctx") ? errno : EDONE;
	}

	if (!SSL_CTX_use_certificate_chain_file(ctx, certfilename)) {
		SSL_CTX_free(ctx);
		return tls_err("missing certificate") ? errno : EDONE;
	}
	SSL_CTX_load_verify_locations(ctx, CLIENTCA, NULL);

	/* crl checking */
	store = SSL_CTX_get_cert_store(ctx);
	if ((lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file())) &&
				(X509_load_crl_file(lookup, CLIENTCRL, X509_FILETYPE_PEM) == 1))
		X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK |
						X509_V_FLAG_CRL_CHECK_ALL);


	saciphers.len = lloadfilefd(openat(controldir_fd, ciphfn, O_RDONLY | O_CLOEXEC), &(saciphers.s), 1);
	if (saciphers.len == (size_t)-1) {
		if (errno != ENOENT) {
			int e = errno;
			SSL_CTX_free(ctx);
			err_control2("control/", ciphfn);
			errno = e;
			return -1;
		}
	} else if (saciphers.len) {
		/* convert all '\0's except the last one to ':' */
		size_t i;

		for (i = 0; i < saciphers.len - 1; ++i)
			if (!saciphers.s[i])
				saciphers.s[i] = ':';
		ciphers = saciphers.s;

		SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
	}

	/* a new SSL object, with the rest added to it directly to avoid copying */
	myssl = SSL_new(ctx);
	SSL_CTX_free(ctx);
	if (!myssl) {
		free(saciphers.s);
		return tls_err("unable to initialize ssl") ? errno : EDONE;
	}

	SSL_set_verify(myssl, SSL_VERIFY_NONE, NULL);

	/* this will also check whether public and private keys match */
	if (!SSL_use_RSAPrivateKey_file(myssl, certfilename, SSL_FILETYPE_PEM)) {
		ssl_free(myssl);
		free(saciphers.s);
		return tls_err("no valid RSA private key") ? errno : EDONE;
	}

	j = SSL_set_cipher_list(myssl, ciphers);
	free(saciphers.s);
	if (j != 1) {
		ssl_free(myssl);
		return tls_err("unable to set ciphers") ? errno : EDONE;
	}

	SSL_set_tmp_rsa_callback(myssl, tmp_rsa_cb);
	SSL_set_tmp_dh_callback(myssl, tmp_dh_cb);
	j = SSL_set_rfd(myssl, 0);
	if (j == 1)
		j = SSL_set_wfd(myssl, socketd);
	if (j != 1) {
		ssl_free(myssl);
		return tls_err("unable to set fd") ? errno : EDONE;
	}

	/* protection against CVE-2011-1431 */
	sync_pipelining();

	if (netwrite("220 2.0.0 ready for tls\r\n")) {
		ssl_free(myssl);
		return errno;
	}

	/* can't set ssl earlier, else netwrite above would try to send the data encrypted with the unfinished ssl */
	ssl = myssl;
	j = ssl_timeoutaccept(timeout);
	if (j == -ETIMEDOUT) {
		dieerror(ETIMEDOUT);
	} else if (j < 0) {
		/* neither cleartext nor any other response here is part of a standard */
		const char *err = ssl_strerror();

		ssl_free(ssl);
		ssl = NULL;
		return tls_out("connection failed", err) ? errno : EDONE;
	}

	prot = SSL_get_cipher(myssl);
	l = strlen(prot);
	newprot = realloc(protocol, l + 20);
	if (!newprot) {
		ssl_free(ssl);
		ssl = NULL;
		return ENOMEM;
	}
	protocol = newprot;
	/* populate the protocol string, used in Received */
	protocol[0] = '(';
	memcpy(protocol + 1, prot, l);
	l++;
	memcpy(protocol + l, " encrypted) ESMTPS", 18);
	protocol[l + 18] = '\0';

	/* have to discard the pre-STARTTLS HELO/EHLO argument, if any */
	return 0;
}

/**
 * initialize STARTTLS mode
 *
 * @return 0 on successful initialization, else error code
 */
int
smtp_starttls(void)
{
	if (ssl || !xmitstat.esmtp)
		return 1;
	return tls_init();
}
