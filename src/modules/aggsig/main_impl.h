/**********************************************************************
 * Copyright (c) 2017 Andrew Poelstra, Pieter Wuille                  *
 * Distributed under the MIT software license, see the accompanying   *
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.*
 **********************************************************************/

#ifndef _SECP256K1_MODULE_AGGSIG_MAIN_
#define _SECP256K1_MODULE_AGGSIG_MAIN_

#include "include/secp256k1.h"
#include "include/secp256k1_aggsig.h"
#include "hash.h"

enum nonce_progress {
    /* Nonce has not been generated by us or recevied from another party */
    NONCE_PROGRESS_UNKNOWN = 0,
    /* Public nonce has been recevied from another party */
    NONCE_PROGRESS_OTHER = 1,
    /* Public nonce has been generated by us but not used in signing. */
    NONCE_PROGRESS_OURS = 2,
    /* Public nonce has been generated by us and used in signing. An attempt to
     * use a nonce twice will result in an error. */
    NONCE_PROGRESS_SIGNED = 3
};

struct secp256k1_aggsig_context_struct {
    enum nonce_progress *progress;
    secp256k1_pubkey *pubkeys;
    secp256k1_scalar *secnonce;
    secp256k1_gej pubnonce_sum;
    size_t n_sigs;
    secp256k1_rfc6979_hmac_sha256_t rng;
};

/* Compute the hash of all the data that every pubkey needs to sign */
static void secp256k1_compute_prehash(const secp256k1_context *ctx, unsigned char *output, const secp256k1_pubkey *pubkeys, size_t n_pubkeys, secp256k1_ge *nonce_ge, const unsigned char *msghash32) {
    size_t i;
    unsigned char buf[33];
    size_t buflen = sizeof(buf);
    secp256k1_sha256_t hasher;
    secp256k1_sha256_initialize(&hasher);
    /* Encode pubkeys */
    for (i = 0; i < n_pubkeys; i++) {
        CHECK(secp256k1_ec_pubkey_serialize(ctx, buf, &buflen, &pubkeys[i], SECP256K1_EC_COMPRESSED));
        secp256k1_sha256_write(&hasher, buf, sizeof(buf));
    }
    /* Encode nonce */
    CHECK(secp256k1_eckey_pubkey_serialize(nonce_ge, buf, &buflen, 1));
    secp256k1_sha256_write(&hasher, buf, sizeof(buf));
    /* Encode message */
    secp256k1_sha256_write(&hasher, msghash32, 32);
    /* Finish */
    secp256k1_sha256_finalize(&hasher, output);
}

/* Add the index to the above hash to customize it for each pubkey */
static int secp256k1_compute_sighash(secp256k1_scalar *r, const unsigned char *prehash, size_t index) {
    unsigned char output[32];
    int overflow;
    secp256k1_sha256_t hasher;
    secp256k1_sha256_initialize(&hasher);
    /* Encode index as a UTF8-style bignum */
    while (index > 0) {
        unsigned char ch = index & 0x7f;
        secp256k1_sha256_write(&hasher, &ch, 1);
        index >>= 7;
    }
    secp256k1_sha256_write(&hasher, prehash, 32);
    secp256k1_sha256_finalize(&hasher, output);
    secp256k1_scalar_set_b32(r, output, &overflow);
    return !overflow;
}

secp256k1_aggsig_context* secp256k1_aggsig_context_create(const secp256k1_context *ctx, const secp256k1_pubkey *pubkeys, size_t n_pubkeys, const unsigned char *seed) {
    secp256k1_aggsig_context* aggctx;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(pubkeys != NULL);
    ARG_CHECK(seed != NULL);

    aggctx = (secp256k1_aggsig_context*)checked_malloc(&ctx->error_callback, sizeof(*aggctx));
    aggctx->progress = (enum nonce_progress*)checked_malloc(&ctx->error_callback, n_pubkeys * sizeof(*aggctx->progress));
    aggctx->pubkeys = (secp256k1_pubkey*)checked_malloc(&ctx->error_callback, n_pubkeys * sizeof(*aggctx->pubkeys));
    aggctx->secnonce = (secp256k1_scalar*)checked_malloc(&ctx->error_callback, n_pubkeys * sizeof(*aggctx->secnonce));
    aggctx->n_sigs = n_pubkeys;
    secp256k1_gej_set_infinity(&aggctx->pubnonce_sum);
    memcpy(aggctx->pubkeys, pubkeys, n_pubkeys * sizeof(*aggctx->pubkeys));
    memset(aggctx->progress, 0, n_pubkeys * sizeof(*aggctx->progress));
    secp256k1_rfc6979_hmac_sha256_initialize(&aggctx->rng, seed, 32);

    return aggctx;
}

/* TODO extend this to export the nonce if the user wants */
int secp256k1_aggsig_generate_nonce(const secp256k1_context* ctx, secp256k1_aggsig_context* aggctx, size_t index) {
    int retry;
    unsigned char data[32];
    secp256k1_gej pubnon;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(secp256k1_ecmult_gen_context_is_built(&ctx->ecmult_gen_ctx));
    ARG_CHECK(aggctx != NULL);
    ARG_CHECK(index < aggctx->n_sigs);

    if (aggctx->progress[index] != NONCE_PROGRESS_UNKNOWN) {
        return 0;
    }

    /* generate nonce from the RNG */
    do {
        secp256k1_rfc6979_hmac_sha256_generate(&aggctx->rng, data, 32);
        secp256k1_scalar_set_b32(&aggctx->secnonce[index], data, &retry);
        retry |= secp256k1_scalar_is_zero(&aggctx->secnonce[index]);
    } while (retry); /* This branch true is cryptographically unreachable. Requires sha256_hmac output > Fp. */
    secp256k1_ecmult_gen(&ctx->ecmult_gen_ctx, &pubnon, &aggctx->secnonce[index]);
    memset(data, 0, 32);  /* TODO proper clear */
    /* Negate nonce if needed to get y to be a quadratic residue */
    if (!secp256k1_gej_has_quad_y_var(&pubnon)) {
        secp256k1_scalar_negate(&aggctx->secnonce[index], &aggctx->secnonce[index]);
        secp256k1_gej_neg(&pubnon, &pubnon);
    }
    secp256k1_gej_add_var(&aggctx->pubnonce_sum, &aggctx->pubnonce_sum, &pubnon, NULL);
    aggctx->progress[index] = NONCE_PROGRESS_OURS;
    return 1;
}

int secp256k1_aggsig_partial_sign(const secp256k1_context* ctx, secp256k1_aggsig_context* aggctx, secp256k1_aggsig_partial_signature *partial, const unsigned char *msghash32, const unsigned char *seckey32, size_t index) {
    size_t i;
    secp256k1_scalar sighash;
    secp256k1_scalar sec;
    secp256k1_ge tmp_ge;
    int overflow;
    unsigned char prehash[32];

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(secp256k1_ecmult_gen_context_is_built(&ctx->ecmult_gen_ctx));
    ARG_CHECK(aggctx != NULL);
    ARG_CHECK(partial != NULL);
    ARG_CHECK(msghash32 != NULL);
    ARG_CHECK(seckey32 != NULL);
    ARG_CHECK(index < aggctx->n_sigs);

    /* check state machine */
    for (i = 0; i < aggctx->n_sigs; i++) {
        if (aggctx->progress[i] == NONCE_PROGRESS_UNKNOWN) {
            return 0;
        }
    }
    if (aggctx->progress[index] != NONCE_PROGRESS_OURS) {
        return 0;
    }

    /* sign */
    /* If the total public nonce has wrong sign, negate our
     * secret nonce. Everyone will negate the public one
     * at combine time. */
    secp256k1_ge_set_gej(&tmp_ge, &aggctx->pubnonce_sum);  /* TODO cache this */
    if (!secp256k1_gej_has_quad_y_var(&aggctx->pubnonce_sum)) {
        secp256k1_scalar_negate(&aggctx->secnonce[index], &aggctx->secnonce[index]);
        secp256k1_ge_neg(&tmp_ge, &tmp_ge);
    }

    secp256k1_compute_prehash(ctx, prehash, aggctx->pubkeys, aggctx->n_sigs, &tmp_ge, msghash32);
    if (secp256k1_compute_sighash(&sighash, prehash, index) == 0) {
        return 0;
    }
    secp256k1_scalar_set_b32(&sec, seckey32, &overflow);
    if (overflow) {
        secp256k1_scalar_clear(&sec);
        return 0;
    }
    secp256k1_scalar_mul(&sec, &sec, &sighash);
    secp256k1_scalar_add(&sec, &sec, &aggctx->secnonce[index]);

    /* finalize */
    secp256k1_scalar_get_b32(partial->data, &sec);
    secp256k1_scalar_clear(&sec);
    aggctx->progress[index] = NONCE_PROGRESS_SIGNED;
    return 1;
}

int secp256k1_aggsig_combine_signatures(const secp256k1_context* ctx, secp256k1_aggsig_context* aggctx, unsigned char *sig64, const secp256k1_aggsig_partial_signature *partial, size_t n_sigs) {
    size_t i;
    secp256k1_scalar s;
    secp256k1_ge final;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(aggctx != NULL);
    ARG_CHECK(sig64 != NULL);
    ARG_CHECK(partial != NULL);
    (void) ctx;

    if (n_sigs != aggctx->n_sigs) {
        return 0;
    }

    secp256k1_scalar_set_int(&s, 0);
    for (i = 0; i < n_sigs; i++) {
        secp256k1_scalar tmp;
        int overflow;
        secp256k1_scalar_set_b32(&tmp, partial[i].data, &overflow);
        if (overflow) {
            return 0;
        }
        secp256k1_scalar_add(&s, &s, &tmp);
    }

    /* If we need to negate the public nonce, everyone will
     * have negated their secret nonces in the previous step. */
    if (!secp256k1_gej_has_quad_y_var(&aggctx->pubnonce_sum)) {
        secp256k1_gej_neg(&aggctx->pubnonce_sum, &aggctx->pubnonce_sum);
    }

    secp256k1_scalar_get_b32(sig64, &s);
    secp256k1_ge_set_gej(&final, &aggctx->pubnonce_sum);
    secp256k1_fe_normalize_var(&final.x);
    secp256k1_fe_get_b32(sig64 + 32, &final.x);
    return 1;
}

#ifdef USE_ENDOMORPHISM
SECP256K1_INLINE static void secp256k1_aggsig_endo_split(secp256k1_scalar *s1, secp256k1_scalar *s2, secp256k1_gej *p1, secp256k1_gej *p2) {
    secp256k1_scalar tmp = *s1;
    secp256k1_scalar_split_lambda(s1, s2, &tmp);
    secp256k1_gej_mul_lambda(p2, p1);

    if (secp256k1_scalar_is_high(s1)) {
        secp256k1_scalar_negate(s1, s1);
        secp256k1_gej_neg(p1, p1);
    }
    if (secp256k1_scalar_is_high(s2)) {
        secp256k1_scalar_negate(s2, s2);
        secp256k1_gej_neg(p2, p2);
    }
}
#endif

int secp256k1_aggsig_verify(const secp256k1_context* ctx, const unsigned char *sig64, const unsigned char *msg32, const secp256k1_pubkey *pubkeys, size_t n_pubkeys) {
    secp256k1_gej pt[SECP256K1_ECMULT_MULTI_MAX_N];
    secp256k1_scalar sc[SECP256K1_ECMULT_MULTI_MAX_N];
    secp256k1_gej pk_sum;
    secp256k1_fe fe_tmp;
    secp256k1_ge r_ge;
    secp256k1_ge ge_tmp;
    size_t i;
    size_t offset;
    int overflow;
    unsigned char prehash[32];

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(sig64 != NULL);
    ARG_CHECK(msg32 != NULL);
    ARG_CHECK(pubkeys != NULL);
    (void) ctx;

    if (n_pubkeys == 0) {
        return 0;
    }

    /* Compute sum sG - e_i*P_i, which should be R */
    secp256k1_gej_set_infinity(&pk_sum);
    secp256k1_scalar_set_b32(&sc[0], sig64, &overflow);
    if (overflow) {
        return 0;
    }
    secp256k1_gej_set_ge(&pt[0], &secp256k1_ge_const_g);

    if (!secp256k1_fe_set_b32(&fe_tmp, sig64 + 32)) {
        return 0;
    }
    if (!secp256k1_ge_set_xquad(&r_ge, &fe_tmp)) {
        return 0;
    }
    secp256k1_compute_prehash(ctx, prehash, pubkeys, n_pubkeys, &r_ge, msg32);

    i = 0;
#ifdef USE_ENDOMORPHISM
    secp256k1_aggsig_endo_split(&sc[0], &sc[1], &pt[0], &pt[1]);
    offset = 2;
    while (i < n_pubkeys) {
        size_t n = (2*(n_pubkeys - i) < SECP256K1_ECMULT_MULTI_MAX_N - offset) ? 2*(n_pubkeys - i) : SECP256K1_ECMULT_MULTI_MAX_N - offset;
        size_t j;
        secp256k1_gej multi;

        /* TODO if n = 1 or 2 then we should use `secp256k1_ecmult` (and require an appropriate ctx) */
        for (j = 0; j < n/2; j++) {
            if (secp256k1_compute_sighash(&sc[2*j + offset], prehash, i + j) == 0) {
                return 0;
            }
            secp256k1_scalar_negate(&sc[2*j + offset], &sc[2*j + offset]);
            secp256k1_pubkey_load(ctx, &ge_tmp, &pubkeys[i + j]);
            secp256k1_gej_set_ge(&pt[2*j + offset], &ge_tmp);

            secp256k1_aggsig_endo_split(&sc[2*j + offset], &sc[2*j + 1 + offset], &pt[2*j + offset], &pt[2*j + 1 + offset]);
        }
        secp256k1_ecmult_multi(&multi, sc, pt, n + offset);
        secp256k1_gej_add_var(&pk_sum, &pk_sum, &multi, NULL);

        i += n / 2;
        offset = 0;
    }
#else
    offset = 1;
    while (i < n_pubkeys) {
        size_t n = (n_pubkeys - i < SECP256K1_ECMULT_MULTI_MAX_N - offset) ? n_pubkeys - i : SECP256K1_ECMULT_MULTI_MAX_N - offset;
        size_t j;
        secp256k1_gej multi;

        /* TODO if n = 1 or 2 then we should use `secp256k1_ecmult` (and require an appropriate ctx) */
        for (j = 0; j < n; j++) {
            if (secp256k1_compute_sighash(&sc[j + offset], prehash, i + j) == 0) {
                return 0;
            }
            secp256k1_scalar_negate(&sc[j + offset], &sc[j + offset]);
            secp256k1_pubkey_load(ctx, &ge_tmp, &pubkeys[i + j]);
            secp256k1_gej_set_ge(&pt[j + offset], &ge_tmp);
        }
        secp256k1_ecmult_multi(&multi, sc, pt, n + offset);
        secp256k1_gej_add_var(&pk_sum, &pk_sum, &multi, NULL);

        i += n;
        offset = 0;
    }
#endif

    secp256k1_ge_neg(&r_ge, &r_ge);
    secp256k1_gej_add_ge_var(&pk_sum, &pk_sum, &r_ge, NULL);
    return secp256k1_gej_is_infinity(&pk_sum);
}

void secp256k1_aggsig_context_destroy(secp256k1_aggsig_context *aggctx) {
    if (aggctx == NULL) {
        return;
    }
    memset(aggctx->pubkeys, 0, aggctx->n_sigs * sizeof(*aggctx->pubkeys));
    memset(aggctx->secnonce, 0, aggctx->n_sigs * sizeof(*aggctx->secnonce));
    memset(aggctx->progress, 0, aggctx->n_sigs * sizeof(*aggctx->progress));
    free(aggctx->pubkeys);
    free(aggctx->secnonce);
    free(aggctx->progress);
    secp256k1_rfc6979_hmac_sha256_finalize(&aggctx->rng);
    free(aggctx);
}

#endif
