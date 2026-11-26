// The real cryptography of the signed manifest (updatesig.h's `Crypto`): ECDSA on P-256 and SHA-256 with the mbedTLS that is already in the board's
// core (the TLS code links all of it for certificate checks, so it adds about half a kilobyte of flash and no new third-party code).
// No Arduino or ESP headers: tools/test_updatesig.cpp compiles this very file against a host mbedTLS and runs every vector through it
// (-DWITH_MBEDTLS), so what the board runs is what the tests proved, not a copy of it.
//
// Where it runs: ota.cpp's data task (core 0), no lock held, never the render task. Needs no random numbers. About 1 KB of stack plus what the
// bignum code allocates (a few KB of heap, freed before it returns); the software ECDSA verify is estimated at 70 to 400 ms on the S3.
// The three-valued result is the point: a library that could not run (out of memory, an internal error) is `CouldNot`, never `Invalid`.
#pragma once
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

#include "updatesig.h"

namespace updatesig {

// What one mbedTLS return code from the P-256 steps means for the signature. 0 is "so far so good" (the caller goes on); the codes that say
// "this key, point or signature is not acceptable" are Invalid; every other one (MBEDTLS_ERR_MPI_ALLOC_FAILED, MBEDTLS_ERR_ECP_ALLOC_FAILED,
// a feature that is not built in, an internal error, ...) says nothing about the signature and is CouldNot.
inline Verify verdictFromEcp(int ret) {
  switch (ret) {
    case 0: return Verify::Valid;
    case MBEDTLS_ERR_ECP_VERIFY_FAILED:   // the signature does not match (also r or s outside 1..n-1)
    case MBEDTLS_ERR_ECP_BAD_INPUT_DATA:  // not a point of the right form, or an argument out of range
    case MBEDTLS_ERR_ECP_INVALID_KEY:     // the point is not on the curve
      return Verify::Invalid;
    default: return Verify::CouldNot;
  }
}

// pub: 65 bytes (0x04, X, Y); hash: SHA-256 of the canonical bytes; sig: r then s, 32 bytes each, big endian.
inline Verify p256VerifyMbedtls(const uint8_t pub[PUBKEY_LEN], const uint8_t hash[32], const uint8_t sig[otalogic::SIG_RAW_LEN]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point Q;
  mbedtls_mpi r, s;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&Q);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
  if (ret == 0) ret = mbedtls_ecp_point_read_binary(&grp, &Q, pub, PUBKEY_LEN);
  if (ret == 0) ret = mbedtls_ecp_check_pubkey(&grp, &Q);
  if (ret == 0) ret = mbedtls_mpi_read_binary(&r, sig, 32);
  if (ret == 0) ret = mbedtls_mpi_read_binary(&s, sig + 32, 32);
  if (ret == 0) ret = mbedtls_ecdsa_verify(&grp, hash, 32, &Q, &r, &s);  // checks 1 <= r, s < n itself
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&s);
  mbedtls_ecp_point_free(&Q);
  mbedtls_ecp_group_free(&grp);
  return verdictFromEcp(ret);
}

struct MbedCrypto : Crypto {
  bool sha256(const uint8_t *msg, size_t len, uint8_t out[32]) override { return mbedtls_sha256(msg, len, out, 0) == 0; }
  Verify p256Verify(const uint8_t pub[PUBKEY_LEN], const uint8_t hash[32], const uint8_t sig[otalogic::SIG_RAW_LEN]) override {
    return p256VerifyMbedtls(pub, hash, sig);
  }
};

}  // namespace updatesig
