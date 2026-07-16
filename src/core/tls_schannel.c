/*
 * tls_schannel.c — the Windows TLS backend for libvncclient's TLS seam (tls.h),
 * built on the OS SChannel (SSPI) provider. This is the SHIPPED TLS backend: it
 * needs no third-party library and no package manager (SChannel + crypt32 are
 * part of Windows). The vendored GnuTLS backend is used only as an in-container
 * REFERENCE to verify the VeNCrypt protocol behavior this file must match.
 *
 * Security posture (hardening over the reference backend):
 *   - X509 VeNCrypt subtypes ONLY. Anonymous TLS (VeNCrypt TLS* and security
 *     type 18) is REFUSED — SChannel does not offer anonymous cipher suites and
 *     unauthenticated TLS gives no MITM protection.
 *   - The server certificate is verified against the user-supplied CA (PEM) with
 *     hostname checking. Any failure fails the handshake CLOSED — there is no
 *     trust-on-first-use and no "ignore errors" path.
 *   - TLS 1.2 (via the legacy SCHANNEL_CRED; TLS 1.3 would need SCH_CREDENTIALS).
 *
 * Known hardening gaps (documented, not bypasses — all fail closed): no
 * certificate revocation checking (CRL/OCSP); a supplied x509CACrlFile is
 * ignored. Add revocation before high-assurance production use.
 *
 * The VeNCrypt framing (read/write of version, status, subtype list) is factored
 * into vencrypt_negotiate() to keep the wire logic auditable against the
 * reference; the SChannel handshake/record layer follows.
 *
 * !!! PENDING VALIDATION: this module targets the documented SSPI/crypt32 API
 * and mirrors the verified GnuTLS behavior, but it has NOT yet been compiled or
 * exercised on a Windows host (the CI here is Linux-only). It must be built and
 * security-reviewed on Windows before being relied upon. See docs/TESTING.md. */

#include <rfb/rfbclient.h>
#include "tls.h"

#define SECURITY_WIN32
#include <winsock2.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <sspi.h>
#include <wincrypt.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")

/* VeNCrypt subtypes (from rfbproto.h). */
#define VENC_TLS_NONE   257
#define VENC_TLS_VNC    258
#define VENC_TLS_PLAIN  259
#define VENC_X509_NONE  260
#define VENC_X509_VNC   261
#define VENC_X509_PLAIN 262

#define SC_IO_BUFFER 65536

typedef struct {
    CredHandle  cred;
    CtxtHandle  ctx;
    BOOL        cred_ok, ctx_ok;
    SecPkgContext_StreamSizes sizes;

    /* Leftover decrypted plaintext not yet consumed by ReadFromTLS. */
    uint8_t    *plain;
    size_t      plain_len, plain_off;
    /* Encrypted bytes read from the socket but not yet processed. */
    uint8_t     enc[SC_IO_BUFFER];
    size_t      enc_len;
} sc_tls;

/* ---- raw socket helpers (blocking) ------------------------------------- */

static int sock_recv(rfbClient *client, uint8_t *buf, size_t n)
{
    return recv(client->sock, (char *)buf, (int)n, 0);
}
static BOOL sock_send_all(rfbClient *client, const uint8_t *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int s = send(client->sock, (const char *)buf + off, (int)(n - off), 0);
        if (s <= 0)
            return FALSE;
        off += (size_t)s;
    }
    return TRUE;
}

/* ---- VeNCrypt negotiation (matches the verified reference framing) ------ */

static BOOL read_n(rfbClient *client, void *out, unsigned n)
{
    return ReadFromRFBServer(client, (char *)out, n) == TRUE;
}
static BOOL write_n(rfbClient *client, const void *in, unsigned n)
{
    return WriteToRFBServer(client, (const char *)in, n) == TRUE;
}

/* Negotiate VeNCrypt up to the point where the TLS handshake must begin.
 * On success sets *out_subtype to the chosen (X509) subtype. Returns FALSE if
 * the server offers no acceptable (X509) subtype — anonymous TLS is refused. */
static BOOL vencrypt_negotiate(rfbClient *client, uint32_t *out_subtype)
{
    uint8_t major = 0, minor = 0, status = 0, count = 0;

    if (!read_n(client, &major, 1) || !read_n(client, &minor, 1))
        return FALSE;
    rfbClientLog("VeNCrypt server version %d.%d\n", major, minor);
    if (major != 0 || minor != 2) {
        rfbClientLog("Unsupported VeNCrypt version.\n");
        return FALSE;
    }
    major = 0; minor = 2;
    if (!write_n(client, &major, 1) || !write_n(client, &minor, 1) ||
        !read_n(client, &status, 1))
        return FALSE;
    if (status != 0) {
        rfbClientLog("Server refused VeNCrypt 0.2.\n");
        return FALSE;
    }

    if (!read_n(client, &count, 1) || count == 0)
        return FALSE;

    uint32_t chosen = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t sub = 0;
        if (!read_n(client, &sub, 4))
            return FALSE;
        sub = rfbClientSwap32IfLE(sub);
        /* Prefer X509+VNC, then X509+None, then X509+Plain. Anonymous TLS
         * subtypes are intentionally never chosen. */
        if (sub == VENC_X509_VNC) { chosen = sub; }
        else if (sub == VENC_X509_NONE && chosen != VENC_X509_VNC) { chosen = sub; }
        else if (sub == VENC_X509_PLAIN && chosen == 0) { chosen = sub; }
    }
    if (chosen == 0) {
        rfbClientLog("Server offered no X509 VeNCrypt subtype; refusing "
                     "unauthenticated TLS.\n");
        return FALSE;
    }

    uint32_t be = rfbClientSwap32IfLE(chosen);
    if (!write_n(client, &be, 4))
        return FALSE;
    /* Ack for the encrypted subtypes: server sends 1 to proceed. */
    if (!read_n(client, &status, 1) || status != 1) {
        rfbClientLog("Server refused VeNCrypt subtype %u.\n", chosen);
        return FALSE;
    }
    *out_subtype = chosen;
    return TRUE;
}

/* ---- CA trust store from a PEM file ------------------------------------ */

static HCERTSTORE load_ca_store(const char *pem_path)
{
    HANDLE f = CreateFileA(pem_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD size = GetFileSize(f, NULL);
    HCERTSTORE store = NULL;
    char *pem = NULL;
    if (size == INVALID_FILE_SIZE || size == 0 || size > (16u << 20))
        goto done;
    pem = malloc(size + 1);
    if (!pem)
        goto done;
    DWORD got = 0;
    if (!ReadFile(f, pem, size, &got, NULL) || got != size)
        goto done;
    pem[size] = 0;

    store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
                          CERT_STORE_CREATE_NEW_FLAG, NULL);
    if (!store)
        goto done;

    /* Parse each PEM certificate block into the store. */
    const char *p = pem;
    const char *begin;
    while ((begin = strstr(p, "-----BEGIN CERTIFICATE-----")) != NULL) {
        const char *end = strstr(begin, "-----END CERTIFICATE-----");
        if (!end)
            break;
        end += strlen("-----END CERTIFICATE-----");
        DWORD der_len = 0;
        if (CryptStringToBinaryA(begin, (DWORD)(end - begin),
                CRYPT_STRING_BASE64HEADER, NULL, &der_len, NULL, NULL)) {
            BYTE *der = malloc(der_len);
            if (der && CryptStringToBinaryA(begin, (DWORD)(end - begin),
                    CRYPT_STRING_BASE64HEADER, der, &der_len, NULL, NULL)) {
                CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING,
                    der, der_len, CERT_STORE_ADD_ALWAYS, NULL);
            }
            free(der);
        }
        p = end;
    }
done:
    free(pem);
    CloseHandle(f);
    return store;
}

/* Verify the server certificate chains to our CA store and matches the host.
 * Fails closed on any error. */
static BOOL verify_server_cert(rfbClient *client, sc_tls *tls, HCERTSTORE ca)
{
    PCCERT_CONTEXT server_cert = NULL;
    BOOL ok = FALSE;
    if (QueryContextAttributes(&tls->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                               &server_cert) != SEC_E_OK || !server_cert)
        return FALSE;

    /* Build the chain using ONLY our CA store as the trust anchor source. */
    CERT_CHAIN_PARA chain_para;
    ZeroMemory(&chain_para, sizeof(chain_para));
    chain_para.cbSize = sizeof(chain_para);

    HCERTCHAINENGINE engine = NULL;
    CERT_CHAIN_ENGINE_CONFIG cfg;
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.cbSize = sizeof(cfg);
    cfg.hExclusiveRoot = ca; /* trust ONLY the supplied CA, not the OS store */
    /* Treat CA certs in the bundle as anchors even when they are intermediates
     * (not self-signed roots), matching GnuTLS's set_x509_trust_file. */
    cfg.dwExclusiveFlags = CERT_CHAIN_EXCLUSIVE_ENABLE_CA_FLAG;
    if (!CertCreateCertificateChainEngine(&cfg, &engine))
        goto done;

    PCCERT_CHAIN_CONTEXT chain = NULL;
    if (!CertGetCertificateChain(engine, server_cert, NULL, server_cert->hCertStore,
                                 &chain_para, 0, NULL, &chain))
        goto done;

    if (chain->TrustStatus.dwErrorStatus != CERT_TRUST_NO_ERROR) {
        rfbClientLog("Server certificate not trusted (0x%lx).\n",
                     (unsigned long)chain->TrustStatus.dwErrorStatus);
        CertFreeCertificateChain(chain);
        goto done;
    }

    /* Hostname + SSL policy check. */
    wchar_t whost[256];
    ZeroMemory(whost, sizeof(whost)); /* overlong host -> empty -> policy rejects */
    MultiByteToWideChar(CP_UTF8, 0, client->serverHost ? client->serverHost : "",
                        -1, whost, 256);
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_para;
    ZeroMemory(&ssl_para, sizeof(ssl_para));
    ssl_para.cbSize = sizeof(ssl_para);
    ssl_para.dwAuthType = AUTHTYPE_SERVER;
    ssl_para.pwszServerName = whost;

    CERT_CHAIN_POLICY_PARA pol_para;
    ZeroMemory(&pol_para, sizeof(pol_para));
    pol_para.cbSize = sizeof(pol_para);
    pol_para.pvExtraPolicyPara = &ssl_para;

    CERT_CHAIN_POLICY_STATUS pol_status;
    ZeroMemory(&pol_status, sizeof(pol_status));
    pol_status.cbSize = sizeof(pol_status);

    if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
                                         &pol_para, &pol_status) &&
        pol_status.dwError == 0) {
        ok = TRUE;
    } else {
        rfbClientLog("Server certificate policy check failed (0x%lx).\n",
                     (unsigned long)pol_status.dwError);
    }
    CertFreeCertificateChain(chain);
done:
    if (engine) CertFreeCertificateChainEngine(engine);
    if (server_cert) CertFreeCertificateContext(server_cert);
    return ok;
}

/* ---- SChannel handshake ------------------------------------------------ */

static BOOL schannel_handshake(rfbClient *client, sc_tls *tls, const char *ca_path)
{
    SCHANNEL_CRED sc;
    ZeroMemory(&sc, sizeof(sc));
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION /* we verify against our CA */
               | SCH_CRED_NO_DEFAULT_CREDS
               | SCH_USE_STRONG_CRYPTO;
    /* TLS 1.2 via the legacy SCHANNEL_CRED. (TLS 1.3 requires the newer
     * SCH_CREDENTIALS struct; SP_PROT_TLS1_3_CLIENT is a no-op here and is
     * undefined on older SDKs. QEMU negotiates TLS 1.2 fine.) */
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;

    if (AcquireCredentialsHandleA(NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
            NULL, &sc, NULL, NULL, &tls->cred, NULL) != SEC_E_OK)
        return FALSE;
    tls->cred_ok = TRUE;

    wchar_t whost[256];
    ZeroMemory(whost, sizeof(whost));
    MultiByteToWideChar(CP_UTF8, 0, client->serverHost ? client->serverHost : "",
                        -1, whost, 256);

    DWORD req = ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY |
                ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT |
                ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
    DWORD out_flags = 0;
    SECURITY_STATUS ss;
    BOOL first = TRUE;

    for (;;) {
        SecBuffer inbuf[2], outbuf[1];
        SecBufferDesc in_desc, out_desc;

        inbuf[0].BufferType = SECBUFFER_TOKEN;
        inbuf[0].pvBuffer = tls->enc;
        inbuf[0].cbBuffer = (unsigned long)tls->enc_len;
        inbuf[1].BufferType = SECBUFFER_EMPTY;
        inbuf[1].pvBuffer = NULL; inbuf[1].cbBuffer = 0;
        in_desc.ulVersion = SECBUFFER_VERSION; in_desc.cBuffers = 2; in_desc.pBuffers = inbuf;

        outbuf[0].BufferType = SECBUFFER_TOKEN;
        outbuf[0].pvBuffer = NULL; outbuf[0].cbBuffer = 0;
        out_desc.ulVersion = SECBUFFER_VERSION; out_desc.cBuffers = 1; out_desc.pBuffers = outbuf;

        ss = InitializeSecurityContextW(&tls->cred, first ? NULL : &tls->ctx,
                first ? whost : NULL, req, 0, 0,
                first ? NULL : &in_desc, 0,
                first ? &tls->ctx : NULL, &out_desc, &out_flags, NULL);
        first = FALSE;
        /* Only mark the context valid once one actually exists, so FreeTLS does
         * not DeleteSecurityContext an uninitialized handle on a hard failure. */
        if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED)
            tls->ctx_ok = TRUE;

        if (outbuf[0].pvBuffer && outbuf[0].cbBuffer) {
            BOOL sent = sock_send_all(client, outbuf[0].pvBuffer, outbuf[0].cbBuffer);
            FreeContextBuffer(outbuf[0].pvBuffer);
            if (!sent)
                return FALSE;
        }

        if (ss == SEC_E_OK) {
            /* The server routinely coalesces its Finished flight with the first
             * encrypted application record (VeNCrypt sub-auth data). SChannel
             * hands that back as SECBUFFER_EXTRA — those bytes are already off
             * the socket, so preserve them for ReadFromTLS instead of blocking
             * on a recv that will never complete. */
            if (inbuf[1].BufferType == SECBUFFER_EXTRA && inbuf[1].cbBuffer) {
                memmove(tls->enc, tls->enc + (tls->enc_len - inbuf[1].cbBuffer),
                        inbuf[1].cbBuffer);
                tls->enc_len = inbuf[1].cbBuffer;
            } else {
                tls->enc_len = 0;
            }
            break;
        }

        if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE) {
            /* Preserve any leftover (SECBUFFER_EXTRA) then read more. */
            if (ss == SEC_I_CONTINUE_NEEDED && inbuf[1].BufferType == SECBUFFER_EXTRA) {
                memmove(tls->enc, tls->enc + (tls->enc_len - inbuf[1].cbBuffer),
                        inbuf[1].cbBuffer);
                tls->enc_len = inbuf[1].cbBuffer;
            } else if (ss == SEC_I_CONTINUE_NEEDED) {
                tls->enc_len = 0;
            }
            if (tls->enc_len >= sizeof(tls->enc))
                return FALSE;
            int r = sock_recv(client, tls->enc + tls->enc_len,
                              sizeof(tls->enc) - tls->enc_len);
            if (r <= 0)
                return FALSE;
            tls->enc_len += (size_t)r;
            continue;
        }
        rfbClientLog("SChannel handshake failed (0x%lx).\n", (unsigned long)ss);
        return FALSE;
    }

    if (QueryContextAttributes(&tls->ctx, SECPKG_ATTR_STREAM_SIZES, &tls->sizes) != SEC_E_OK)
        return FALSE;

    /* Verify the peer certificate against the user CA (fail closed). Any
     * pipelined app data preserved above in tls->enc stays for ReadFromTLS. */
    HCERTSTORE ca = ca_path ? load_ca_store(ca_path) : NULL;
    if (!ca) {
        rfbClientLog("No/invalid CA certificate provided; refusing TLS.\n");
        return FALSE;
    }
    BOOL verified = verify_server_cert(client, tls, ca);
    CertCloseStore(ca, 0);
    return verified;
}

/* ---- tls.h entry points ------------------------------------------------ */

rfbBool HandleAnonTLSAuth(rfbClient *client)
{
    (void)client;
    rfbClientLog("Anonymous TLS refused (no server authentication).\n");
    return FALSE;
}

rfbBool HandleVeNCryptAuth(rfbClient *client)
{
    uint32_t subtype = 0;
    if (!vencrypt_negotiate(client, &subtype))
        return FALSE;
    client->subAuthScheme = subtype;

    sc_tls *tls = calloc(1, sizeof(*tls));
    if (!tls)
        return FALSE;

    char ca_buf[1024];
    ca_buf[0] = 0;
    const char *ca_path = NULL;
    if (client->GetCredential) {
        rfbCredential *cred = client->GetCredential(client, rfbCredentialTypeX509);
        if (cred) {
            if (cred->x509Credential.x509CACertFile) {
                strncpy(ca_buf, cred->x509Credential.x509CACertFile, sizeof(ca_buf) - 1);
                ca_buf[sizeof(ca_buf) - 1] = 0;
                ca_path = ca_buf;
            }
            /* We own the returned credential; free the fields our
             * GetCredential set, then the struct. */
            free(cred->x509Credential.x509CACertFile);
            free(cred->x509Credential.x509CACrlFile);
            free(cred->x509Credential.x509ClientCertFile);
            free(cred->x509Credential.x509ClientKeyFile);
            free(cred);
        }
    }

    /* schannel_handshake owns `tls`; set it as the session up front so FreeTLS
     * cleans up SSPI handles on failure. */
    client->tlsSession = tls;
    if (!schannel_handshake(client, tls, ca_path)) {
        FreeTLS(client); /* frees tls and clears client->tlsSession */
        return FALSE;
    }
    return TRUE;
}

int ReadFromTLS(rfbClient *client, char *out, unsigned int n)
{
    sc_tls *tls = client->tlsSession;
    if (!tls) { errno = EINTR; return -1; }

    /* Serve buffered plaintext first. */
    if (tls->plain && tls->plain_off < tls->plain_len) {
        size_t avail = tls->plain_len - tls->plain_off;
        size_t take = avail < n ? avail : n;
        memcpy(out, tls->plain + tls->plain_off, take);
        tls->plain_off += take;
        if (tls->plain_off >= tls->plain_len) {
            free(tls->plain);
            tls->plain = NULL; tls->plain_len = tls->plain_off = 0;
        }
        return (int)take;
    }

    for (;;) {
        /* Try to decrypt what we already have. */
        if (tls->enc_len > 0) {
            SecBuffer bufs[4];
            bufs[0].BufferType = SECBUFFER_DATA; bufs[0].pvBuffer = tls->enc;
            bufs[0].cbBuffer = (unsigned long)tls->enc_len;
            bufs[1].BufferType = SECBUFFER_EMPTY;
            bufs[2].BufferType = SECBUFFER_EMPTY;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
            SECURITY_STATUS ss = DecryptMessage(&tls->ctx, &desc, 0, NULL);
            if (ss == SEC_E_OK) {
                SecBuffer *data = NULL, *extra = NULL;
                for (int i = 0; i < 4; i++) {
                    if (bufs[i].BufferType == SECBUFFER_DATA && !data) data = &bufs[i];
                    if (bufs[i].BufferType == SECBUFFER_EXTRA && !extra) extra = &bufs[i];
                }
                size_t dlen = data ? data->cbBuffer : 0;
                size_t take = dlen < n ? dlen : n;
                if (data && take)
                    memcpy(out, data->pvBuffer, take);
                if (data && dlen > take) {
                    /* Buffer the remainder for the next call. */
                    tls->plain = malloc(dlen - take);
                    if (tls->plain) {
                        memcpy(tls->plain, (uint8_t *)data->pvBuffer + take, dlen - take);
                        tls->plain_len = dlen - take; tls->plain_off = 0;
                    }
                }
                size_t extra_len = extra ? extra->cbBuffer : 0;
                if (extra_len)
                    memmove(tls->enc, extra->pvBuffer, extra_len);
                tls->enc_len = extra_len;
                return (int)take;
            }
            if (ss == SEC_I_CONTEXT_EXPIRED)
                return 0; /* clean TLS close_notify -> EOF */
            if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                errno = EINTR;
                return -1;
            }
            /* need more bytes */
        }
        if (tls->enc_len >= sizeof(tls->enc)) { errno = EINTR; return -1; }
        int r = sock_recv(client, tls->enc + tls->enc_len, sizeof(tls->enc) - tls->enc_len);
        if (r == 0) return 0;
        if (r < 0) { errno = EINTR; return -1; }
        tls->enc_len += (size_t)r;
    }
}

int WriteToTLS(rfbClient *client, const char *buf, unsigned int n)
{
    sc_tls *tls = client->tlsSession;
    if (!tls) { errno = EINTR; return -1; }

    unsigned int total = 0;
    while (total < n) {
        unsigned int chunk = n - total;
        if (chunk > tls->sizes.cbMaximumMessage)
            chunk = tls->sizes.cbMaximumMessage;

        size_t need = tls->sizes.cbHeader + chunk + tls->sizes.cbTrailer;
        uint8_t *msg = malloc(need);
        if (!msg) { errno = EINTR; return -1; }
        memcpy(msg + tls->sizes.cbHeader, buf + total, chunk);

        SecBuffer bufs[4];
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = msg; bufs[0].cbBuffer = tls->sizes.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = msg + tls->sizes.cbHeader; bufs[1].cbBuffer = chunk;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = msg + tls->sizes.cbHeader + chunk; bufs[2].cbBuffer = tls->sizes.cbTrailer;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };

        if (EncryptMessage(&tls->ctx, 0, &desc, 0) != SEC_E_OK) {
            free(msg); errno = EINTR; return -1;
        }
        size_t out_len = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        BOOL sent = sock_send_all(client, msg, out_len);
        free(msg);
        if (!sent) { errno = EINTR; return -1; }
        total += chunk;
    }
    return (int)n;
}

void FreeTLS(rfbClient *client)
{
    sc_tls *tls = client->tlsSession;
    if (!tls)
        return;
    if (tls->ctx_ok) DeleteSecurityContext(&tls->ctx);
    if (tls->cred_ok) FreeCredentialsHandle(&tls->cred);
    free(tls->plain);
    free(tls);
    client->tlsSession = NULL;
}

#ifdef LIBVNCSERVER_HAVE_SASL
int GetTLSCipherBits(rfbClient *client)
{
    (void)client;
    return 128; /* SCH_USE_STRONG_CRYPTO enforces >=128-bit suites */
}
#endif
