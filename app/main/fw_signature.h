/**
 * @file fw_signature.h
 * @brief Firmware image signature verification (ECDSA P-256 / SHA-256).
 *
 * The CI signs the SHA-256 of the raw firmware image (tools/fw_sign.py) with a
 * private key that never leaves the CI secret store. The matching public key is
 * compiled in (include/fw_pubkey.h). Every update path hashes the image while
 * it streams to flash and calls verify() BEFORE the update is committed
 * (Update.end() / h2 ota_end), so a rejected image never becomes bootable.
 *
 * Two ways the 64-byte raw signature (r||s) reaches the device:
 *  - HTTPS manifest: "signature" field, base64.
 *  - Manual upload (*.signed.bin): 68-byte trailer appended to the image:
 *        [ image ][ signature r||s : 64 B ][ magic "FWS1" : 4 B ]
 *    FwTrailerStream holds those last bytes back so they never reach flash.
 *
 * Verification is enforced unless FW_SIGNATURE_ENFORCE=0 (development only).
 */
#pragma once

#include <Arduino.h>
#include <mbedtls/sha256.h>

#ifndef FW_SIGNATURE_ENFORCE
#define FW_SIGNATURE_ENFORCE 1
#endif

constexpr size_t FW_SIG_LEN         = 64;                       ///< raw r||s
constexpr size_t FW_SIG_TRAILER_LEN = FW_SIG_LEN + 4;           ///< signature + magic
constexpr char   FW_SIG_MAGIC[4]    = {'F', 'W', 'S', '1'};

/** @brief Streaming SHA-256 over the image, then ECDSA verify of the digest. */
class FwSigVerifier {
public:
    FwSigVerifier();
    ~FwSigVerifier();
    FwSigVerifier(const FwSigVerifier&) = delete;
    FwSigVerifier& operator=(const FwSigVerifier&) = delete;

    void reset();
    void update(const uint8_t* data, size_t len);

    /** @brief Finishes the hash and checks @p sig (64 B raw r||s). One-shot. */
    bool verify(const uint8_t sig[FW_SIG_LEN]);

    /** @brief Same, signature given as base64 (manifest field). */
    bool verifyBase64(const char* sig_b64);

private:
    mbedtls_sha256_context ctx_;
    bool finished_ = false;
};

/**
 * @brief Sink for FwTrailerStream: write @p len bytes of image data to flash.
 * @return false aborts the stream.
 */
using FwSinkFn = bool (*)(const uint8_t* data, size_t len, void* user);

/**
 * @brief Splits a "<image><signature><magic>" upload stream.
 *
 * The total payload length is unknown while streaming (multipart bodies carry
 * boundary overhead), so the last FW_SIG_TRAILER_LEN bytes are always held
 * back. Everything before them is hashed and passed to the sink; at finish()
 * the held-back bytes are the trailer.
 */
class FwTrailerStream {
public:
    void begin(FwSinkFn sink, void* user);
    /** @return false if the sink failed. */
    bool push(const uint8_t* data, size_t len);
    /** @brief Checks magic + signature. Call once after the last push(). */
    bool finish();
    size_t payloadBytes() const { return payload_; }

private:
    FwSigVerifier ver_;
    FwSinkFn sink_ = nullptr;
    void* user_ = nullptr;
    uint8_t tail_[FW_SIG_TRAILER_LEN];
    size_t tail_len_ = 0;
    size_t payload_ = 0;
};

/**
 * @brief Verifies a complete in-memory "<image><signature><magic>" buffer.
 * @param[out] image_size Length of the image without trailer (on success).
 */
bool fw_sig_verify_signed_buffer(const uint8_t* data, size_t size, size_t* image_size);

/** @brief Verifies a complete in-memory image against a detached base64 signature. */
bool fw_sig_verify_buffer_b64(const uint8_t* data, size_t size, const char* sig_b64);
