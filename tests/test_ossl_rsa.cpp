#include "oidc.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <sqlite3.h>

using namespace morfo::oidc;

void test_base64url_and_crypto_vectors() {
    // 1. Base64URL encode/decode
    std::string text = "Hello World! Testing OIDC Base64URL encoding/decoding.";
    std::string encoded = base64url_encode(text);
    assert(!encoded.empty());
    assert(encoded.find('+') == std::string::npos);
    assert(encoded.find('/') == std::string::npos);
    assert(encoded.find('=') == std::string::npos);
    std::string decoded = base64url_decode(encoded);
    assert(decoded == text);

    // 2. PKCE code challenge test vector from RFC 7636 Appendix B:
    // code_verifier:  dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk
    // code_challenge: E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM
    std::string rfc_verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    std::string rfc_challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
    std::string calculated_challenge = pkce_code_challenge(rfc_verifier);
    assert(calculated_challenge == rfc_challenge);

    // 3. SHA-256 test vector ("" sha256 is e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855)
    std::string empty_hash = sha256_hex("");
    assert(empty_hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    std::cout << "[PASS] Base64URL, PKCE RFC 7636 and SHA-256 vectors\n";
}

void test_openssl_rsa_jws_verification() {
    // Generate RSA 2048 keypair using OpenSSL 3 EVP APIs
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    assert(ctx != nullptr);
    assert(EVP_PKEY_keygen_init(ctx) == 1);
    assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1);

    EVP_PKEY* pkey = nullptr;
    assert(EVP_PKEY_keygen(ctx, &pkey) == 1);
    assert(pkey != nullptr);
    EVP_PKEY_CTX_free(ctx);

    // Extract public parameters n and e
    BIGNUM* bn_n = nullptr;
    BIGNUM* bn_e = nullptr;
    assert(EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &bn_n) == 1);
    assert(EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &bn_e) == 1);

    std::vector<unsigned char> n_bytes(BN_num_bytes(bn_n));
    BN_bn2bin(bn_n, n_bytes.data());
    std::vector<unsigned char> e_bytes(BN_num_bytes(bn_e));
    BN_bn2bin(bn_e, e_bytes.data());

    BN_free(bn_n);
    BN_free(bn_e);

    JwkKey jwk;
    jwk.kid = "test-key-01";
    jwk.alg = "RS256";
    jwk.n_b64 = base64url_encode(std::string_view(reinterpret_cast<const char*>(n_bytes.data()), n_bytes.size()));
    jwk.e_b64 = base64url_encode(std::string_view(reinterpret_cast<const char*>(e_bytes.data()), e_bytes.size()));

    // Create a JWS payload to sign
    std::string header_b64 = base64url_encode(R"({"alg":"RS256","kid":"test-key-01","typ":"JWT"})");
    std::string payload_b64 = base64url_encode(R"({"iss":"https://accounts.google.com","sub":"123456789","hd":"embrapa.br"})");
    std::string signing_input = header_b64 + "." + payload_b64;

    // Sign using OpenSSL EVP_DigestSign (RS256)
    EVP_MD_CTX* sign_ctx = EVP_MD_CTX_new();
    assert(sign_ctx != nullptr);
    assert(EVP_DigestSignInit(sign_ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1);
    assert(EVP_DigestSignUpdate(sign_ctx, signing_input.data(), signing_input.size()) == 1);

    size_t sig_len = 0;
    assert(EVP_DigestSignFinal(sign_ctx, nullptr, &sig_len) == 1);
    std::vector<unsigned char> sig_bytes(sig_len);
    assert(EVP_DigestSignFinal(sign_ctx, sig_bytes.data(), &sig_len) == 1);
    EVP_MD_CTX_free(sign_ctx);

    std::string_view sig_view(reinterpret_cast<const char*>(sig_bytes.data()), sig_bytes.size());

    // 1. Verify valid signature using morfo::oidc::verify_rsa_signature
    bool ok = verify_rsa_signature(jwk, signing_input, sig_view);
    assert(ok == true);

    // 2. Reject tampered signing input
    std::string tampered_input = signing_input + "x";
    assert(verify_rsa_signature(jwk, tampered_input, sig_view) == false);

    // 3. Reject corrupted signature byte
    std::vector<unsigned char> corrupted_sig = sig_bytes;
    corrupted_sig[0] ^= 0xFF;
    std::string_view bad_sig_view(reinterpret_cast<const char*>(corrupted_sig.data()), corrupted_sig.size());
    assert(verify_rsa_signature(jwk, signing_input, bad_sig_view) == false);

    // 4. Reject signature against wrong key
    JwkKey wrong_jwk = jwk;
    wrong_jwk.n_b64[0] = (wrong_jwk.n_b64[0] == 'A') ? 'B' : 'A';
    assert(verify_rsa_signature(wrong_jwk, signing_input, sig_view) == false);

    EVP_PKEY_free(pkey);
    std::cout << "[PASS] OpenSSL 3 RSA JWS signature verification and negative test cases\n";
}

void test_transaction_manager_replay_prevention() {
    OidcTransactionManager mgr;
    OidcTransaction tx;
    tx.state = "state-abc-123";
    tx.nonce = "nonce-xyz-789";
    tx.code_verifier = "verifier-456";
    tx.correlation_id = "corr-001";
    tx.created_at = std::chrono::steady_clock::now();

    mgr.store(tx);

    // First take must succeed
    auto retrieved = mgr.take("state-abc-123");
    assert(retrieved.has_value());
    assert(retrieved->nonce == "nonce-xyz-789");
    assert(retrieved->code_verifier == "verifier-456");

    // Second take (replay) MUST fail and return std::nullopt
    auto replay = mgr.take("state-abc-123");
    assert(!replay.has_value());

    // Non-existent state must return std::nullopt
    assert(!mgr.take("non-existent-state").has_value());

    std::cout << "[PASS] OIDC Transaction single-use and replay prevention\n";
}

void test_database_identity_links_and_audit() {
    sqlite3* db = nullptr;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

    // Initialize required base schema and Migration 5
    const char* schema = R"(
        CREATE TABLE users(
            id TEXT PRIMARY KEY,
            email TEXT NOT NULL UNIQUE,
            name TEXT NOT NULL,
            created_at TEXT NOT NULL
        );
        INSERT INTO users(id, email, name, created_at) VALUES('usr-001', 'investigador@embrapa.br', 'Investigador', '2026-10-20T10:00:00Z');

        CREATE TABLE identity_links(
            id TEXT PRIMARY KEY,
            user_id TEXT NOT NULL REFERENCES users(id),
            provider TEXT NOT NULL CHECK(provider IN ('google')),
            issuer TEXT NOT NULL,
            subject TEXT NOT NULL,
            hosted_domain TEXT,
            email_at_link TEXT,
            state TEXT NOT NULL CHECK(state IN ('PENDING','ACTIVE','REVOKED')),
            verified_at TEXT,
            linked_at TEXT NOT NULL,
            revoked_at TEXT,
            UNIQUE(issuer, subject)
        );

        CREATE TABLE identity_audit_events(
            id TEXT PRIMARY KEY,
            user_id TEXT,
            link_id TEXT,
            action TEXT NOT NULL,
            occurred_at TEXT NOT NULL,
            decision TEXT NOT NULL,
            evidence_type TEXT NOT NULL,
            correlation_id TEXT NOT NULL
        );
    )";

    char* err_msg = nullptr;
    assert(sqlite3_exec(db, schema, nullptr, nullptr, &err_msg) == SQLITE_OK);

    // 1. Create pending link for new Google subject
    auto link = create_pending_link(db, "https://accounts.google.com", "sub-google-999", "pesquisador@embrapa.br", "Pesquisador", "embrapa.br", "corr-test-1");
    assert(link.state == "PENDING");
    assert(link.email_at_link == "pesquisador@embrapa.br");

    auto found = find_identity_link(db, "https://accounts.google.com", "sub-google-999");
    assert(found.has_value());
    assert(found->state == "PENDING");

    // 2. Approve identity link
    assert(approve_identity_link(db, link.id, "corr-test-2") == true);
    found = find_identity_link_by_id(db, link.id);
    assert(found.has_value());
    assert(found->state == "ACTIVE");

    // 3. Link identity to existing user
    assert(link_identity_to_user(db, link.id, "usr-001", "corr-test-3") == true);
    found = find_identity_link_by_id(db, link.id);
    assert(found.has_value());
    assert(found->user_id == "usr-001");
    assert(found->state == "ACTIVE");

    // 4. Revoke identity link
    assert(revoke_identity_link(db, link.id, "corr-test-4") == true);
    found = find_identity_link_by_id(db, link.id);
    assert(found.has_value());
    assert(found->state == "REVOKED");

    // 5. Verify audit events were recorded
    sqlite3_stmt* stmt = nullptr;
    assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM identity_audit_events", -1, &stmt, nullptr) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int audit_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    assert(audit_count >= 4);

    sqlite3_close(db);
    std::cout << "[PASS] SQLite Migration 5 Identity Links and Audit integrity\n";
}

int main() {
    std::cout << "Running morfo.oidc.crypto unit tests...\n";
    test_base64url_and_crypto_vectors();
    test_openssl_rsa_jws_verification();
    test_transaction_manager_replay_prevention();
    test_database_identity_links_and_audit();
    std::cout << "All morfo.oidc.crypto unit tests PASSED successfully.\n";
    return 0;
}
