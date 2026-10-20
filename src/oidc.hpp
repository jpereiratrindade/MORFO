#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <sqlite3.h>

namespace morfo::oidc {

struct OidcConfig {
    std::string client_id;
    std::string client_secret;
    std::string redirect_uri;
    std::string issuer = "https://accounts.google.com";
    std::string auth_endpoint = "https://accounts.google.com/o/oauth2/v2/auth";
    std::string token_endpoint = "https://oauth2.googleapis.com/token";
    std::string jwks_uri = "https://www.googleapis.com/oauth2/v3/certs";
    std::string required_hd = "embrapa.br";
    bool dev_otp_disabled = false;

    bool enabled() const { return !client_id.empty(); }
    static OidcConfig from_environment();
};

struct OidcTransaction {
    std::string state;
    std::string nonce;
    std::string code_verifier;
    std::string correlation_id;
    std::chrono::steady_clock::time_point created_at;
};

class OidcTransactionManager {
public:
    OidcTransactionManager() = default;
    void store(const OidcTransaction& tx);
    std::optional<OidcTransaction> take(const std::string& state);
    void cleanup();

private:
    std::mutex mutex_;
    std::map<std::string, OidcTransaction> transactions_;
};

struct IdTokenClaims {
    std::string iss;
    std::string aud;
    std::string sub;
    std::string email;
    std::string name;
    std::string hd;
    bool email_verified = false;
    int64_t exp = 0;
    int64_t iat = 0;
    std::string nonce;
};

struct JwkKey {
    std::string kid;
    std::string alg; // e.g. "RS256"
    std::string n_b64;
    std::string e_b64;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};

HttpResponse fetch_url(const std::string& method,
                      const std::string& url,
                      const std::string& body = "",
                      const std::vector<std::pair<std::string, std::string>>& headers = {},
                      int timeout_seconds = 10);

class JwksKeyStore {
public:
    JwksKeyStore() = default;
    void update_keys_json(const std::string& jwks_json);
    std::optional<JwkKey> find_key(const std::string& kid);
    std::optional<JwkKey> get_or_refresh_key(const std::string& kid, const std::string& jwks_uri);
    void clear();

private:
    std::mutex mutex_;
    std::map<std::string, JwkKey> keys_;
    std::chrono::steady_clock::time_point last_refresh_;
};

// Cryptographic helpers
std::string base64url_encode(std::string_view data);
std::string base64url_decode(std::string_view data);
std::string url_encode(std::string_view s);
std::string sha256_raw(std::string_view data);
std::string sha256_hex(std::string_view data);
std::string pkce_code_challenge(std::string_view verifier);
std::string random_string(std::size_t bytes = 32);
std::string generate_pkce_verifier();

bool verify_rsa_signature(const JwkKey& key, std::string_view signing_input, std::string_view signature_bytes);

IdTokenClaims parse_and_validate_id_token(const std::string& id_token,
                                          const OidcConfig& config,
                                          const std::string& expected_nonce,
                                          JwksKeyStore& keystore);

std::string exchange_code_for_tokens(const OidcConfig& config,
                                     const std::string& code,
                                     const std::string& code_verifier);

struct IdentityLink {
    std::string id;
    std::string user_id;
    std::string provider;
    std::string issuer;
    std::string subject;
    std::string hosted_domain;
    std::string email_at_link;
    std::string state; // "PENDING", "ACTIVE", "REVOKED"
    std::string verified_at;
    std::string linked_at;
    std::string revoked_at;
};

std::optional<IdentityLink> find_identity_link(sqlite3* db, const std::string& issuer, const std::string& subject);
std::optional<IdentityLink> find_identity_link_by_id(sqlite3* db, const std::string& id);
std::vector<IdentityLink> list_identity_links(sqlite3* db);

void record_audit_event(sqlite3* db,
                        const std::string& user_id,
                        const std::string& link_id,
                        const std::string& action,
                        const std::string& decision,
                        const std::string& evidence_type,
                        const std::string& correlation_id);

IdentityLink create_pending_link(sqlite3* db,
                                const std::string& issuer,
                                const std::string& subject,
                                const std::string& email,
                                const std::string& name,
                                const std::string& hosted_domain,
                                const std::string& correlation_id);

bool approve_identity_link(sqlite3* db, const std::string& link_id, const std::string& correlation_id);
bool link_identity_to_user(sqlite3* db, const std::string& link_id, const std::string& user_id, const std::string& correlation_id);
bool revoke_identity_link(sqlite3* db, const std::string& link_id, const std::string& correlation_id);
void update_link_email(sqlite3* db, const std::string& link_id, const std::string& new_email, const std::string& correlation_id);

} // namespace morfo::oidc
