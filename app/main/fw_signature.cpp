#include "fw_signature.h"

#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>
#include <uuid/log.h>

#if __has_include("fw_pubkey.h")
#include "fw_pubkey.h"   // const uint8_t FW_PUBKEY[65]  (0x04 || X || Y)
#else
#error "include/fw_pubkey.h missing: run `python3 tools/fw_sign.py keygen --out <private.pem>` and commit the generated header."
#endif

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

FwSigVerifier::FwSigVerifier() { mbedtls_sha256_init(&ctx_); reset(); }
FwSigVerifier::~FwSigVerifier() { mbedtls_sha256_free(&ctx_); }

void FwSigVerifier::reset() {
    mbedtls_sha256_free(&ctx_);
    mbedtls_sha256_init(&ctx_);
    mbedtls_sha256_starts(&ctx_, 0);
    finished_ = false;
}

void FwSigVerifier::update(const uint8_t* data, size_t len) {
    if (!finished_ && len) mbedtls_sha256_update(&ctx_, data, len);
}

bool FwSigVerifier::verify(const uint8_t sig[FW_SIG_LEN]) {
    if (finished_) return false;
    finished_ = true;

    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx_, hash);

#if !FW_SIGNATURE_ENFORCE
    logger.warning("[SIG] verification disabled (FW_SIGNATURE_ENFORCE=0)");
    return true;
#else
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    bool ok = false;
    int rc;
    if ((rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1)) == 0 &&
        (rc = mbedtls_ecp_point_read_binary(&grp, &Q, FW_PUBKEY, sizeof(FW_PUBKEY))) == 0 &&
        (rc = mbedtls_ecp_check_pubkey(&grp, &Q)) == 0 &&
        (rc = mbedtls_mpi_read_binary(&r, sig, 32)) == 0 &&
        (rc = mbedtls_mpi_read_binary(&s, sig + 32, 32)) == 0 &&
        (rc = mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &Q, &r, &s)) == 0) {
        ok = true;
    }
    if (!ok) logger.warning("[SIG] verify failed (mbedtls -0x%04X)", (unsigned)-rc);

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return ok;
#endif
}

bool FwSigVerifier::verifyBase64(const char* sig_b64) {
    uint8_t sig[FW_SIG_LEN + 4];
    size_t n = 0;
    if (!sig_b64 || mbedtls_base64_decode(sig, sizeof(sig), &n,
                                          (const uint8_t*)sig_b64, strlen(sig_b64)) != 0 ||
        n != FW_SIG_LEN) {
        finished_ = true;
        logger.warning("[SIG] malformed signature");
        return false;
    }
    return verify(sig);
}

// ---------------------------------------------------------------------------

void FwTrailerStream::begin(FwSinkFn sink, void* user) {
    ver_.reset();
    sink_ = sink;
    user_ = user;
    tail_len_ = 0;
    payload_ = 0;
}

bool FwTrailerStream::push(const uint8_t* data, size_t len) {
    // Treat tail_ + data as one logical stream; release all but its last
    // FW_SIG_TRAILER_LEN bytes.
    while (len) {
        if (tail_len_ < FW_SIG_TRAILER_LEN) {
            const size_t n = min(len, FW_SIG_TRAILER_LEN - tail_len_);
            memcpy(tail_ + tail_len_, data, n);
            tail_len_ += n; data += n; len -= n;
            continue;
        }
        // tail_ is full and more data follows: the oldest bytes are payload.
        const size_t n = min(len, FW_SIG_TRAILER_LEN);
        // Oldest n bytes of tail_ become payload, new bytes shift in.
        ver_.update(tail_, n);
        if (sink_ && !sink_(tail_, n, user_)) return false;
        payload_ += n;
        memmove(tail_, tail_ + n, FW_SIG_TRAILER_LEN - n);
        memcpy(tail_ + FW_SIG_TRAILER_LEN - n, data, n);
        data += n; len -= n;
    }
    return true;
}

bool FwTrailerStream::finish() {
    if (tail_len_ != FW_SIG_TRAILER_LEN ||
        memcmp(tail_ + FW_SIG_LEN, FW_SIG_MAGIC, sizeof(FW_SIG_MAGIC)) != 0) {
        logger.warning("[SIG] signature trailer missing (unsigned image?)");
#if !FW_SIGNATURE_ENFORCE
        // Unsigned dev image: the held-back bytes are image data after all.
        if (sink_ && !sink_(tail_, tail_len_, user_)) return false;
        payload_ += tail_len_;
        return true;
#else
        return false;
#endif
    }
    return ver_.verify(tail_);
}

// ---------------------------------------------------------------------------

bool fw_sig_verify_signed_buffer(const uint8_t* data, size_t size, size_t* image_size) {
    if (size <= FW_SIG_TRAILER_LEN ||
        memcmp(data + size - sizeof(FW_SIG_MAGIC), FW_SIG_MAGIC, sizeof(FW_SIG_MAGIC)) != 0) {
        logger.warning("[SIG] signature trailer missing (unsigned image?)");
        if (!FW_SIGNATURE_ENFORCE && image_size) *image_size = size;
        return !FW_SIGNATURE_ENFORCE;
    }
    const size_t img = size - FW_SIG_TRAILER_LEN;
    FwSigVerifier v;
    v.update(data, img);
    if (!v.verify(data + img)) return false;
    if (image_size) *image_size = img;
    return true;
}

bool fw_sig_verify_buffer_b64(const uint8_t* data, size_t size, const char* sig_b64) {
    FwSigVerifier v;
    v.update(data, size);
    return v.verifyBase64(sig_b64);
}
