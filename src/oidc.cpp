#include "oidc.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace morfo::oidc {

namespace {

std::string utc_now() {
    auto t = std::time(nullptr);
    std::tm v{};
    gmtime_r(&t, &v);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &v);
    return buf;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string read_file_trimmed(const std::string& path) {
    std::ifstream file(path);
    if (!file) return "";
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return trim(content);
}

std::string json_extract_string(const std::string& body, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto pos = body.find(pat);
    if (pos == std::string::npos) return "";
    auto colon = body.find(':', pos + pat.size());
    if (colon == std::string::npos) return "";
    auto q1 = body.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    std::string value;
    for (std::size_t i = q1 + 1; i < body.size(); ++i) {
        char c = body[i];
        if (c == '"') return value;
        if (c != '\\') {
            value += c;
            continue;
        }
        if (++i >= body.size()) return "";
        char esc = body[i];
        if (esc == '"' || esc == '\\' || esc == '/') value += esc;
        else if (esc == 'b') value += '\b';
        else if (esc == 'f') value += '\f';
        else if (esc == 'n') value += '\n';
        else if (esc == 'r') value += '\r';
        else if (esc == 't') value += '\t';
    }
    return "";
}

bool json_extract_bool(const std::string& body, const std::string& key, bool fallback = false) {
    std::string pat = "\"" + key + "\"";
    auto pos = body.find(pat);
    if (pos == std::string::npos) return fallback;
    auto colon = body.find(':', pos + pat.size());
    if (colon == std::string::npos) return fallback;
    auto val_start = body.find_first_not_of(" \t\r\n", colon + 1);
    if (val_start == std::string::npos) return fallback;
    if (body.compare(val_start, 4, "true") == 0) return true;
    if (body.compare(val_start, 5, "false") == 0) return false;
    if (body.compare(val_start, 6, "\"true\"") == 0) return true;
    if (body.compare(val_start, 7, "\"false\"") == 0) return false;
    return fallback;
}

int64_t json_extract_int64(const std::string& body, const std::string& key, int64_t fallback = 0) {
    std::string pat = "\"" + key + "\"";
    auto pos = body.find(pat);
    if (pos == std::string::npos) return fallback;
    auto colon = body.find(':', pos + pat.size());
    if (colon == std::string::npos) return fallback;
    auto start = body.find_first_of("-0123456789", colon + 1);
    if (start == std::string::npos) return fallback;
    try {
        return std::stoll(body.substr(start));
    } catch (...) {
        return fallback;
    }
}

struct ParsedUrl {
    std::string protocol;
    std::string host;
    std::string port;
    std::string target;
};

ParsedUrl parse_url(const std::string& url) {
    ParsedUrl res;
    std::string u = url;
    auto scheme_end = u.find("://");
    if (scheme_end != std::string::npos) {
        res.protocol = u.substr(0, scheme_end);
        u = u.substr(scheme_end + 3);
    } else {
        res.protocol = "http";
    }
    auto slash = u.find('/');
    std::string host_port = (slash == std::string::npos) ? u : u.substr(0, slash);
    res.target = (slash == std::string::npos) ? "/" : u.substr(slash);

    auto colon = host_port.find(':');
    if (colon != std::string::npos) {
        res.host = host_port.substr(0, colon);
        res.port = host_port.substr(colon + 1);
    } else {
        res.host = host_port;
        res.port = (res.protocol == "https") ? "443" : "80";
    }
    return res;
}

} // namespace

std::string url_encode(std::string_view s) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return escaped.str();
}

OidcConfig OidcConfig::from_environment() {
    OidcConfig cfg;
    const char* cid = std::getenv("MORFO_GOOGLE_CLIENT_ID");
    if (cid && *cid) cfg.client_id = trim(cid);

    const char* csec = std::getenv("MORFO_GOOGLE_CLIENT_SECRET");
    if (csec && *csec) cfg.client_secret = trim(csec);
    else {
        const char* sec_file = std::getenv("MORFO_GOOGLE_CLIENT_SECRET_FILE");
        if (sec_file && *sec_file) cfg.client_secret = read_file_trimmed(sec_file);
    }

    const char* red = std::getenv("MORFO_GOOGLE_REDIRECT_URI");
    if (red && *red) cfg.redirect_uri = trim(red);
    else cfg.redirect_uri = "http://127.0.0.1:8091/api/v1/auth/google/callback";

    const char* iss = std::getenv("MORFO_OIDC_ISSUER");
    if (iss && *iss) cfg.issuer = trim(iss);

    const char* auth_ep = std::getenv("MORFO_OIDC_AUTH_ENDPOINT");
    if (auth_ep && *auth_ep) cfg.auth_endpoint = trim(auth_ep);

    const char* tok_ep = std::getenv("MORFO_OIDC_TOKEN_ENDPOINT");
    if (tok_ep && *tok_ep) cfg.token_endpoint = trim(tok_ep);

    const char* jwks = std::getenv("MORFO_OIDC_JWKS_URI");
    if (jwks && *jwks) cfg.jwks_uri = trim(jwks);

    const char* hd = std::getenv("MORFO_OIDC_REQUIRED_HD");
    if (hd && *hd) cfg.required_hd = trim(hd);

    const char* dis_otp = std::getenv("MORFO_DISABLE_DEV_OTP");
    if (dis_otp && (*dis_otp == '1' || std::string(dis_otp) == "true")) {
        cfg.dev_otp_disabled = true;
    }
    if (cfg.enabled()) {
        cfg.dev_otp_disabled = true;
    }
    return cfg;
}

void OidcTransactionManager::store(const OidcTransaction& tx) {
    std::lock_guard lock(mutex_);
    cleanup();
    transactions_[tx.state] = tx;
}

std::optional<OidcTransaction> OidcTransactionManager::take(const std::string& state) {
    std::lock_guard lock(mutex_);
    cleanup();
    auto it = transactions_.find(state);
    if (it == transactions_.end()) return std::nullopt;
    OidcTransaction tx = it->second;
    transactions_.erase(it);
    return tx;
}

void OidcTransactionManager::cleanup() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = transactions_.begin(); it != transactions_.end();) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.created_at).count() > 600) {
            it = transactions_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string base64url_encode(std::string_view data) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(tab[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tab[((val << 8) >> (valb + 8)) & 0x3F]);
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return out;
}

std::string base64url_decode(std::string_view data) {
    std::string s(data);
    for (char& c : s) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (s.size() % 4 != 0) s.push_back('=');

    static const int inv[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : s) {
        if (c == '=') break;
        int d = inv[c];
        if (d < 0) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string sha256_raw(std::string_view data) {
    unsigned char dig[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), dig, &len, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("SHA-256 computation failed");
    }
    return std::string(reinterpret_cast<char*>(dig), len);
}

std::string sha256_hex(std::string_view data) {
    static const char h[] = "0123456789abcdef";
    std::string raw = sha256_raw(data);
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out += h[c >> 4];
        out += h[c & 15];
    }
    return out;
}

std::string pkce_code_challenge(std::string_view verifier) {
    return base64url_encode(sha256_raw(verifier));
}

std::string random_string(std::size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        throw std::runtime_error("Random number generator unavailable");
    }
    static const char h[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (unsigned char c : buf) {
        out += h[c >> 4];
        out += h[c & 15];
    }
    return out;
}

std::string generate_pkce_verifier() {
    std::vector<unsigned char> buf(48);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        throw std::runtime_error("Random number generator unavailable");
    }
    return base64url_encode(std::string_view(reinterpret_cast<char*>(buf.data()), buf.size()));
}

bool verify_rsa_signature(const JwkKey& key, std::string_view signing_input, std::string_view signature_bytes) {
    std::string n_bin = base64url_decode(key.n_b64);
    std::string e_bin = base64url_decode(key.e_b64);
    if (n_bin.empty() || e_bin.empty()) return false;

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    if (!bld) return false;

    BIGNUM* n = BN_bin2bn(reinterpret_cast<const unsigned char*>(n_bin.data()), static_cast<int>(n_bin.size()), nullptr);
    BIGNUM* e = BN_bin2bn(reinterpret_cast<const unsigned char*>(e_bin.data()), static_cast<int>(e_bin.size()), nullptr);
    if (!n || !e) {
        if (n) BN_free(n);
        if (e) BN_free(e);
        OSSL_PARAM_BLD_free(bld);
        return false;
    }

    OSSL_PARAM_BLD_push_BN(bld, "n", n);
    OSSL_PARAM_BLD_push_BN(bld, "e", e);
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    EVP_PKEY* pkey = nullptr;
    if (ctx) {
        EVP_PKEY_fromdata_init(ctx);
        EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);
        EVP_PKEY_CTX_free(ctx);
    }

    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    BN_free(n);
    BN_free(e);

    if (!pkey) return false;

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        return false;
    }

    bool valid = false;
    if (EVP_DigestVerifyInit(md_ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
        EVP_DigestVerifyUpdate(md_ctx, signing_input.data(), signing_input.size()) == 1) {
        int r = EVP_DigestVerifyFinal(md_ctx, reinterpret_cast<const unsigned char*>(signature_bytes.data()), signature_bytes.size());
        valid = (r == 1);
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return valid;
}

HttpResponse fetch_url(const std::string& method,
                      const std::string& url,
                      const std::string& body,
                      const std::vector<std::pair<std::string, std::string>>& custom_headers,
                      int timeout_seconds) {
    ParsedUrl p = parse_url(url);
    asio::io_context ioc;
    tcp::resolver resolver(ioc);
    auto const results = resolver.resolve(p.host, p.port);

    HttpResponse resp;
    http::request<http::string_body> req{
        method == "POST" ? http::verb::post : http::verb::get,
        p.target,
        11
    };
    req.set(http::field::host, p.host);
    req.set(http::field::user_agent, "MORFO-OIDC-Client/0.1.0");
    for (const auto& [k, v] : custom_headers) {
        req.set(k, v);
    }
    if (!body.empty()) {
        req.body() = body;
        req.prepare_payload();
    }

    if (p.protocol == "https") {
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), p.host.c_str())) {
            throw std::runtime_error("SNI configuration failed");
        }
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(timeout_seconds));
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::write(stream, req);
        beast::flat_buffer b;
        http::response<http::string_body> res;
        http::read(stream, b, res);

        resp.status = static_cast<int>(res.result_int());
        resp.body = res.body();
        for (const auto& f : res) {
            resp.headers[std::string(f.name_string())] = std::string(f.value());
        }
        beast::error_code ec;
        stream.shutdown(ec);
    } else {
        beast::tcp_stream stream(ioc);
        stream.expires_after(std::chrono::seconds(timeout_seconds));
        stream.connect(results);

        http::write(stream, req);
        beast::flat_buffer b;
        http::response<http::string_body> res;
        http::read(stream, b, res);

        resp.status = static_cast<int>(res.result_int());
        resp.body = res.body();
        for (const auto& f : res) {
            resp.headers[std::string(f.name_string())] = std::string(f.value());
        }
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    }

    return resp;
}

void JwksKeyStore::update_keys_json(const std::string& jwks_json) {
    std::lock_guard lock(mutex_);
    keys_.clear();
    std::size_t pos = 0;
    while ((pos = jwks_json.find('{', pos)) != std::string::npos) {
        auto end = jwks_json.find('}', pos);
        if (end == std::string::npos) break;
        std::string block = jwks_json.substr(pos, end - pos + 1);
        std::string kid = json_extract_string(block, "kid");
        std::string alg = json_extract_string(block, "alg");
        std::string kty = json_extract_string(block, "kty");
        std::string n = json_extract_string(block, "n");
        std::string e = json_extract_string(block, "e");

        if (!kid.empty() && !n.empty() && !e.empty() && (kty.empty() || kty == "RSA")) {
            keys_[kid] = JwkKey{kid, alg.empty() ? "RS256" : alg, n, e};
        }
        pos = end + 1;
    }
    last_refresh_ = std::chrono::steady_clock::now();
}

std::optional<JwkKey> JwksKeyStore::find_key(const std::string& kid) {
    std::lock_guard lock(mutex_);
    auto it = keys_.find(kid);
    if (it != keys_.end()) return it->second;
    return std::nullopt;
}

std::optional<JwkKey> JwksKeyStore::get_or_refresh_key(const std::string& kid, const std::string& jwks_uri) {
    auto k = find_key(kid);
    if (k) return k;

    // Refresh from jwks_uri
    try {
        HttpResponse res = fetch_url("GET", jwks_uri);
        if (res.status == 200) {
            update_keys_json(res.body);
            return find_key(kid);
        }
    } catch (...) {
        // Log or handle network error
    }
    return std::nullopt;
}

void JwksKeyStore::clear() {
    std::lock_guard lock(mutex_);
    keys_.clear();
}

IdTokenClaims parse_and_validate_id_token(const std::string& id_token,
                                          const OidcConfig& config,
                                          const std::string& expected_nonce,
                                          JwksKeyStore& keystore) {
    auto dot1 = id_token.find('.');
    if (dot1 == std::string::npos) throw std::runtime_error("Malformed ID token: missing first dot");
    auto dot2 = id_token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) throw std::runtime_error("Malformed ID token: missing second dot");

    std::string_view header_b64(id_token.data(), dot1);
    std::string_view payload_b64(id_token.data() + dot1 + 1, dot2 - dot1 - 1);
    std::string_view sig_b64(id_token.data() + dot2 + 1, id_token.size() - dot2 - 1);
    std::string_view signing_input(id_token.data(), dot2);

    std::string header_json = base64url_decode(header_b64);
    std::string payload_json = base64url_decode(payload_b64);
    std::string sig_bytes = base64url_decode(sig_b64);

    std::string alg = json_extract_string(header_json, "alg");
    if (alg != "RS256") {
        throw std::runtime_error("Unsupported algorithm in ID token header: " + alg);
    }
    std::string kid = json_extract_string(header_json, "kid");

    auto key = keystore.get_or_refresh_key(kid, config.jwks_uri);
    if (!key) {
        throw std::runtime_error("Signing key not found in JWKS: kid=" + kid);
    }

    if (!verify_rsa_signature(*key, signing_input, sig_bytes)) {
        throw std::runtime_error("Invalid cryptographic signature in ID token");
    }

    IdTokenClaims claims;
    claims.iss = json_extract_string(payload_json, "iss");
    claims.aud = json_extract_string(payload_json, "aud");
    claims.sub = json_extract_string(payload_json, "sub");
    claims.email = json_extract_string(payload_json, "email");
    claims.name = json_extract_string(payload_json, "name");
    claims.hd = json_extract_string(payload_json, "hd");
    claims.email_verified = json_extract_bool(payload_json, "email_verified", false);
    claims.exp = json_extract_int64(payload_json, "exp", 0);
    claims.iat = json_extract_int64(payload_json, "iat", 0);
    claims.nonce = json_extract_string(payload_json, "nonce");

    // Check issuer
    bool valid_issuer = (claims.iss == "https://accounts.google.com" ||
                         claims.iss == "accounts.google.com" ||
                         claims.iss == config.issuer);
    if (!valid_issuer) {
        throw std::runtime_error("Issuer mismatch in ID token: " + claims.iss);
    }

    // Check audience
    if (claims.aud != config.client_id) {
        throw std::runtime_error("Audience mismatch in ID token: " + claims.aud + " vs " + config.client_id);
    }

    // Check expiration and iat
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (claims.exp <= now - 30) {
        throw std::runtime_error("ID token expired (exp=" + std::to_string(claims.exp) + ")");
    }
    if (claims.iat > now + 300) {
        throw std::runtime_error("ID token iat is in the future");
    }

    // Check nonce
    if (claims.nonce != expected_nonce) {
        throw std::runtime_error("Nonce mismatch in ID token: " + claims.nonce);
    }

    // Check email_verified
    if (!claims.email_verified) {
        throw std::runtime_error("Email not verified by identity provider");
    }

    // Check hd claim
    if (claims.hd.empty() || claims.hd != config.required_hd) {
        throw std::runtime_error("Hosted domain (hd) missing or invalid: '" + claims.hd + "' != '" + config.required_hd + "'");
    }

    if (claims.sub.empty()) {
        throw std::runtime_error("Missing subject (sub) in ID token");
    }

    return claims;
}

std::string exchange_code_for_tokens(const OidcConfig& config,
                                     const std::string& code,
                                     const std::string& code_verifier) {
    std::string post_data = "grant_type=authorization_code"
                            "&code=" + url_encode(code) +
                            "&redirect_uri=" + url_encode(config.redirect_uri) +
                            "&client_id=" + url_encode(config.client_id) +
                            "&code_verifier=" + url_encode(code_verifier);
    if (!config.client_secret.empty()) {
        post_data += "&client_secret=" + url_encode(config.client_secret);
    }

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/x-www-form-urlencoded"}
    };

    HttpResponse res = fetch_url("POST", config.token_endpoint, post_data, headers, 15);
    if (res.status != 200) {
        std::string err_desc = json_extract_string(res.body, "error_description");
        if (err_desc.empty()) err_desc = json_extract_string(res.body, "error");
        if (err_desc.empty()) err_desc = "HTTP status " + std::to_string(res.status);
        throw std::runtime_error("Token exchange failed: " + err_desc);
    }
    return res.body;
}

std::optional<IdentityLink> find_identity_link(sqlite3* db, const std::string& issuer, const std::string& subject) {
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id, user_id, provider, issuer, subject, hosted_domain, email_at_link, state, verified_at, linked_at, revoked_at FROM identity_links WHERE issuer=? AND subject=?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(st, 1, issuer.data(), static_cast<int>(issuer.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, subject.data(), static_cast<int>(subject.size()), SQLITE_TRANSIENT);

    std::optional<IdentityLink> link;
    if (sqlite3_step(st) == SQLITE_ROW) {
        auto col = [st](int c) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, c));
            return p ? p : "";
        };
        link = IdentityLink{
            col(0), col(1), col(2), col(3), col(4), col(5), col(6), col(7), col(8), col(9), col(10)
        };
    }
    sqlite3_finalize(st);
    return link;
}

std::optional<IdentityLink> find_identity_link_by_id(sqlite3* db, const std::string& id) {
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id, user_id, provider, issuer, subject, hosted_domain, email_at_link, state, verified_at, linked_at, revoked_at FROM identity_links WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(st, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);

    std::optional<IdentityLink> link;
    if (sqlite3_step(st) == SQLITE_ROW) {
        auto col = [st](int c) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, c));
            return p ? p : "";
        };
        link = IdentityLink{
            col(0), col(1), col(2), col(3), col(4), col(5), col(6), col(7), col(8), col(9), col(10)
        };
    }
    sqlite3_finalize(st);
    return link;
}

std::vector<IdentityLink> list_identity_links(sqlite3* db) {
    std::vector<IdentityLink> list;
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id, user_id, provider, issuer, subject, hosted_domain, email_at_link, state, verified_at, linked_at, revoked_at FROM identity_links ORDER BY linked_at DESC";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return list;

    auto col = [st](int c) -> std::string {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, c));
        return p ? p : "";
    };
    while (sqlite3_step(st) == SQLITE_ROW) {
        list.push_back(IdentityLink{
            col(0), col(1), col(2), col(3), col(4), col(5), col(6), col(7), col(8), col(9), col(10)
        });
    }
    sqlite3_finalize(st);
    return list;
}

void record_audit_event(sqlite3* db,
                        const std::string& user_id,
                        const std::string& link_id,
                        const std::string& action,
                        const std::string& decision,
                        const std::string& evidence_type,
                        const std::string& correlation_id) {
    std::string id = random_string(12);
    std::string now = utc_now();
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO identity_audit_events(id, user_id, link_id, action, occurred_at, decision, evidence_type, correlation_id) VALUES(?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;

    sqlite3_bind_text(st, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
    if (!user_id.empty()) sqlite3_bind_text(st, 2, user_id.data(), static_cast<int>(user_id.size()), SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 2);
    if (!link_id.empty()) sqlite3_bind_text(st, 3, link_id.data(), static_cast<int>(link_id.size()), SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 3);
    sqlite3_bind_text(st, 4, action.data(), static_cast<int>(action.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, now.data(), static_cast<int>(now.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, decision.data(), static_cast<int>(decision.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, evidence_type.data(), static_cast<int>(evidence_type.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);

    sqlite3_step(st);
    sqlite3_finalize(st);
}

IdentityLink create_pending_link(sqlite3* db,
                                const std::string& issuer,
                                const std::string& subject,
                                const std::string& email,
                                const std::string& name,
                                const std::string& hosted_domain,
                                const std::string& correlation_id) {
    std::string link_id = random_string(12);
    std::string new_user_id = random_string(12);
    std::string now = utc_now();

    // Create an isolated user row for this pending identity so that foreign key integrity is preserved
    // and no existing images or data are associated with it.
    std::string user_email = email;
    sqlite3_stmt* check_email = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE email=?", -1, &check_email, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(check_email, 1, email.data(), static_cast<int>(email.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(check_email) == SQLITE_ROW) {
            user_email = email + "~pending-" + random_string(6);
        }
        sqlite3_finalize(check_email);
    }

    sqlite3_stmt* ust = nullptr;
    const char* usql = "INSERT INTO users(id, email, name, created_at, last_login_at) VALUES(?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, usql, -1, &ust, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(ust, 1, new_user_id.data(), static_cast<int>(new_user_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(ust, 2, user_email.data(), static_cast<int>(user_email.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(ust, 3, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(ust, 4, now.data(), static_cast<int>(now.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(ust, 5, now.data(), static_cast<int>(now.size()), SQLITE_TRANSIENT);
        sqlite3_step(ust);
        sqlite3_finalize(ust);
    }

    sqlite3_stmt* lst = nullptr;
    const char* lsql = "INSERT INTO identity_links(id, user_id, provider, issuer, subject, hosted_domain, email_at_link, state, verified_at, linked_at, revoked_at) VALUES(?,?,?,?,?,?,?,?,NULL,?,NULL)";
    if (sqlite3_prepare_v2(db, lsql, -1, &lst, nullptr) == SQLITE_OK) {
        std::string provider = "google";
        std::string state = "PENDING";
        sqlite3_bind_text(lst, 1, link_id.data(), static_cast<int>(link_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(lst, 2, new_user_id.data(), static_cast<int>(new_user_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(lst, 3, provider.data(), static_cast<int>(provider.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(lst, 4, issuer.data(), static_cast<int>(issuer.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(lst, 5, subject.data(), static_cast<int>(subject.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(lst, 6, hosted_domain.data(), static_cast<int>(hosted_domain.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(lst, 7, email.data(), static_cast<int>(email.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(lst, 8, state.data(), static_cast<int>(state.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(lst, 9, now.data(), static_cast<int>(now.size()), SQLITE_TRANSIENT);
        sqlite3_step(lst);
        sqlite3_finalize(lst);
    }

    record_audit_event(db, new_user_id, link_id, "OIDC_REGISTER_PENDING", "PENDING_APPROVAL", "OIDC_ID_TOKEN", correlation_id);

    return IdentityLink{
        link_id, new_user_id, "google", issuer, subject, hosted_domain, email, "PENDING", "", now, ""
    };
}

bool approve_identity_link(sqlite3* db, const std::string& link_id, const std::string& correlation_id) {
    auto link = find_identity_link_by_id(db, link_id);
    if (!link) return false;

    std::string now = utc_now();
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE identity_links SET state='ACTIVE', verified_at=? WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, now.data(), static_cast<int>(now.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, link_id.data(), static_cast<int>(link_id.size()), SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc == SQLITE_DONE) {
        record_audit_event(db, link->user_id, link_id, "ADMIN_APPROVE_LINK", "ALLOW", "MANUAL_ADMIN_CLI", correlation_id);
        return true;
    }
    return false;
}

bool link_identity_to_user(sqlite3* db, const std::string& link_id, const std::string& user_id, const std::string& correlation_id) {
    auto link = find_identity_link_by_id(db, link_id);
    if (!link) return false;

    // Check target user exists
    sqlite3_stmt* check_user = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE id=?", -1, &check_user, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(check_user, 1, user_id.data(), static_cast<int>(user_id.size()), SQLITE_TRANSIENT);
    bool exists = (sqlite3_step(check_user) == SQLITE_ROW);
    sqlite3_finalize(check_user);
    if (!exists) return false;

    std::string now = utc_now();
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE identity_links SET user_id=?, state='ACTIVE', verified_at=? WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, user_id.data(), static_cast<int>(user_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, now.data(), static_cast<int>(now.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, link_id.data(), static_cast<int>(link_id.size()), SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc == SQLITE_DONE) {
        record_audit_event(db, user_id, link_id, "ADMIN_LINK_USER", "ALLOW", "MANUAL_ADMIN_CLI", correlation_id);
        return true;
    }
    return false;
}

bool revoke_identity_link(sqlite3* db, const std::string& link_id, const std::string& correlation_id) {
    auto link = find_identity_link_by_id(db, link_id);
    if (!link) return false;

    std::string now = utc_now();
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE identity_links SET state='REVOKED', revoked_at=? WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, now.data(), static_cast<int>(now.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, link_id.data(), static_cast<int>(link_id.size()), SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    // Invalidate sessions for that user
    sqlite3_stmt* sst = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE user_id=?", -1, &sst, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(sst, 1, link->user_id.data(), static_cast<int>(link->user_id.size()), SQLITE_TRANSIENT);
        sqlite3_step(sst);
        sqlite3_finalize(sst);
    }

    if (rc == SQLITE_DONE) {
        record_audit_event(db, link->user_id, link_id, "ADMIN_REVOKE_LINK", "DENY", "MANUAL_ADMIN_CLI", correlation_id);
        return true;
    }
    return false;
}

void update_link_email(sqlite3* db, const std::string& link_id, const std::string& new_email, const std::string& correlation_id) {
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE identity_links SET email_at_link=? WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, new_email.data(), static_cast<int>(new_email.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, link_id.data(), static_cast<int>(link_id.size()), SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

    record_audit_event(db, "", link_id, "OIDC_EMAIL_UPDATED", "ALLOW", "OIDC_ID_TOKEN", correlation_id);
}

} // namespace morfo::oidc
