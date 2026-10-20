#include "auth_smtp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace morfo::auth {

namespace {

std::string utc_now() {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string utc_future(int seconds) {
    auto now = std::chrono::system_clock::now() + std::chrono::seconds(seconds);
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string safe_col_text(sqlite3_stmt* stmt, int col) {
    const auto* txt = sqlite3_column_text(stmt, col);
    return txt ? reinterpret_cast<const char*>(txt) : "";
}

} // namespace

std::string normalize_email(std::string_view email) {
    std::string out;
    out.reserve(email.size());
    for (char c : email) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

std::string sha256_hex(std::string_view data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char c : hash) {
        ss << std::setw(2) << static_cast<int>(c);
    }
    return ss.str();
}

std::string compute_hmac_sha256(std::string_view data, std::string_view key) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         md, &md_len);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < md_len; ++i) {
        ss << std::setw(2) << static_cast<int>(md[i]);
    }
    return ss.str();
}

std::string generate_numeric_code(std::size_t digits) {
    std::string code;
    code.resize(digits);
    unsigned char buf[32];
    if (RAND_bytes(buf, static_cast<int>(digits)) != 1) {
        throw std::runtime_error("OpenSSL RAND_bytes failed");
    }
    for (std::size_t i = 0; i < digits; ++i) {
        code[i] = '0' + (buf[i] % 10);
    }
    return code;
}

std::string generate_session_token() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("OpenSSL RAND_bytes failed");
    }
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char b : bytes) {
        ss << std::setw(2) << static_cast<int>(b);
    }
    return ss.str();
}

bool constant_time_compare(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

AuthSmtpConfig AuthSmtpConfig::from_environment() {
    AuthSmtpConfig cfg;
    const char* h = std::getenv("MORFO_SMTP_HOST");
    if (h && *h) cfg.smtp_host = h;
    const char* p = std::getenv("MORFO_SMTP_PORT");
    if (p && *p) {
        try { cfg.smtp_port = std::stoi(p); } catch (...) {}
    }
    const char* u = std::getenv("MORFO_SMTP_USER");
    if (u && *u) cfg.smtp_user = u;
    const char* pass = std::getenv("MORFO_SMTP_PASS");
    if (pass && *pass) cfg.smtp_pass = pass;
    const char* f = std::getenv("MORFO_SMTP_FROM");
    if (f && *f) cfg.smtp_from = f;
    const char* sec = std::getenv("MORFO_AUTH_SECRET");
    if (sec && *sec) {
        cfg.auth_secret = sec;
    } else {
        // Ephemeral process secret if not configured in environment
        cfg.auth_secret = generate_session_token();
    }
    const char* sim = std::getenv("MORFO_SMTP_SIMULATED");
    if (sim && (*sim == '1' || std::string(sim) == "true")) {
        cfg.simulated = true;
    }
    const char* dev = std::getenv("MORFO_DEV_AUTH");
    const char* dev_otp = std::getenv("MORFO_DEV_OTP");
    if ((dev && (*dev == '1' || std::string(dev) == "true")) ||
        (dev_otp && (*dev_otp == '1' || std::string(dev_otp) == "true"))) {
        cfg.dev_mode = true;
    }
    const char* cd = std::getenv("MORFO_COOLDOWN_SECONDS");
    if (cd && *cd) {
        try { cfg.cooldown_seconds = std::stoi(cd); } catch (...) {}
    } else if (cfg.dev_mode) {
        cfg.cooldown_seconds = 0;
    }
    return cfg;
}

int RateLimiter::check_and_record_request(const std::string& email, const std::string& ip, int cooldown_sec) {
    std::lock_guard lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    // 1. Email cooldown
    auto& eh = email_histories_[email];
    if (eh.last_request.time_since_epoch().count() > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - eh.last_request).count();
        if (elapsed < cooldown_sec) {
            return static_cast<int>(cooldown_sec - elapsed);
        }
    }

    // 2. Email 15-minute sliding window (max 5 requests)
    auto cutoff_15m = now - std::chrono::minutes(15);
    eh.window_requests.erase(
        std::remove_if(eh.window_requests.begin(), eh.window_requests.end(),
                       [&](const auto& tp) { return tp < cutoff_15m; }),
        eh.window_requests.end());

    if (eh.window_requests.size() >= 5) {
        auto oldest = eh.window_requests.front();
        auto wait_sec = std::chrono::duration_cast<std::chrono::seconds>(
            oldest + std::chrono::minutes(15) - now).count();
        return std::max(1, static_cast<int>(wait_sec));
    }

    // 3. IP 15-minute sliding window (max 15 requests)
    if (!ip.empty()) {
        auto& ip_reqs = ip_histories_[ip];
        ip_reqs.erase(
            std::remove_if(ip_reqs.begin(), ip_reqs.end(),
                           [&](const auto& tp) { return tp < cutoff_15m; }),
            ip_reqs.end());
        if (ip_reqs.size() >= 15) {
            auto oldest = ip_reqs.front();
            auto wait_sec = std::chrono::duration_cast<std::chrono::seconds>(
                oldest + std::chrono::minutes(15) - now).count();
            return std::max(1, static_cast<int>(wait_sec));
        }
        ip_reqs.push_back(now);
    }

    eh.last_request = now;
    eh.window_requests.push_back(now);
    return 0;
}

void RateLimiter::reset() {
    std::lock_guard lock(mutex_);
    email_histories_.clear();
    ip_histories_.clear();
}

AuthSmtpService::AuthSmtpService(AuthSmtpConfig config)
    : config_(std::move(config)) {}

bool AuthSmtpService::send_email(const std::string& to_email, const std::string& code, std::string& err_out) {
    if (config_.simulated || config_.dev_mode) {
        return true;
    }

    std::ostringstream msg;
    msg << "From: \"" << config_.smtp_from_name << "\" <" << config_.smtp_from << ">\r\n"
        << "To: <" << to_email << ">\r\n"
        << "Subject: MORFO - Codigo de Acesso: " << code << "\r\n"
        << "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        << "Ola!\r\n\r\n"
        << "Voce solicitou acesso ao sistema MORFO (Ecossistema SisTer - Embrapa).\r\n"
        << "Seu codigo temporario de acesso e:\r\n\r\n"
        << "    " << code << "\r\n\r\n"
        << "Este codigo e de uso unico, permite ate 5 tentativas e expira em 10 minutos.\r\n"
        << "Se voce nao solicitou este codigo, por favor ignore este e-mail.\r\n\r\n"
        << "--\r\nMORFO - Percepcao e Classificacao Tematica - Embrapa\r\n";

    std::ostringstream cmd;
    cmd << "curl --silent --show-error --url 'smtp://" << config_.smtp_host << ":" << config_.smtp_port << "'";
    cmd << " --mail-from " << shell_quote(config_.smtp_from);
    cmd << " --mail-rcpt " << shell_quote(to_email);

    if (config_.smtp_port == 587 || config_.smtp_port == 25) {
        cmd << " --ssl-reqd";
    } else if (config_.smtp_port == 465) {
        cmd << " --ssl";
    }

    if (!config_.smtp_user.empty()) {
        cmd << " --user " << shell_quote(config_.smtp_user + ":" + config_.smtp_pass);
    }

    FILE* pipe = popen(cmd.str().c_str(), "w");
    if (!pipe) {
        err_out = "Não foi possível disparar processo de entrega SMTP";
        return false;
    }

    std::string email_data = msg.str();
    fwrite(email_data.data(), 1, email_data.size(), pipe);
    int status = pclose(pipe);

    if (status != 0) {
        err_out = "Falha no envio via SMTP (código de retorno " + std::to_string(status) + ")";
        return false;
    }
    return true;
}

void AuthSmtpService::record_audit(sqlite3* db, const std::string& email, const std::string& ip,
                                  const std::string& action, const std::string& decision, const std::string& details) {
    if (!db) return;
    const char* sql = R"(
        INSERT INTO auth_audit_events(id, email, ip_address, action, decision, details, occurred_at)
        VALUES(?, ?, ?, ?, ?, ?, ?);
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        std::string id = "audit-" + generate_session_token().substr(0, 16);
        std::string now = utc_now();
        sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, ip.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, action.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, decision.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, details.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::optional<UserRecord> AuthSmtpService::find_user_by_email(sqlite3* db, const std::string& email) {
    if (!db) return std::nullopt;
    std::string norm = normalize_email(email);
    const char* sql = "SELECT id, email, name, role, is_active, created_at, IFNULL(last_login_at,'') FROM users WHERE email=? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, norm.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<UserRecord> user;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        UserRecord u;
        u.id = safe_col_text(stmt, 0);
        u.email = safe_col_text(stmt, 1);
        u.name = safe_col_text(stmt, 2);
        u.role = safe_col_text(stmt, 3);
        u.is_active = (sqlite3_column_int(stmt, 4) == 1);
        u.created_at = safe_col_text(stmt, 5);
        u.last_login_at = safe_col_text(stmt, 6);
        user = u;
    }
    sqlite3_finalize(stmt);
    return user;
}

RequestCodeResult AuthSmtpService::request_code(sqlite3* db, const std::string& email, const std::string& ip_address) {
    RequestCodeResult res;
    std::string norm = normalize_email(email);

    if (norm.empty() || norm.find('@') == std::string::npos) {
        res.ok = false;
        res.status_code = 400;
        res.user_message = "Endereço de e-mail inválido.";
        return res;
    }

    // Enforce rate limiter
    int retry_after = rate_limiter_.check_and_record_request(norm, ip_address, config_.cooldown_seconds);
    if (retry_after > 0) {
        record_audit(db, norm, ip_address, "REQUEST_CODE", "REJECTED_RATE_LIMIT", "Cooldown: " + std::to_string(retry_after) + "s");
        res.ok = false;
        res.status_code = 429;
        res.retry_after_seconds = retry_after;
        res.user_message = "Por favor, aguarde " + std::to_string(retry_after) + " segundos antes de solicitar um novo código.";
        return res;
    }

    // Check if user is registered and authorized
    auto user_opt = find_user_by_email(db, norm);
    if (!user_opt.has_value() || !user_opt->is_active) {
        if (config_.dev_mode && !user_opt.has_value()) {
            add_user(db, norm, norm.substr(0, norm.find('@')), "user");
            user_opt = find_user_by_email(db, norm);
        } else {
            if (!config_.dev_mode) {
                // Constant-time sleep to prevent email enumeration timing attacks
                std::this_thread::sleep_for(std::chrono::milliseconds(90));
            }
            record_audit(db, norm, ip_address, "REQUEST_CODE", "IGNORED_UNAUTHORIZED", "User absent or inactive");
            // Generic response to prevent enumeration
            res.ok = true;
            res.status_code = 200;
            res.user_message = "Se o endereço estiver cadastrado e autorizado, um código de verificação foi enviado para sua caixa de entrada.";
            return res;
        }
    }

    // Invalidate previous pending codes for this email
    {
        const char* invalidate_sql = "UPDATE auth_codes SET used_at=? WHERE email=? AND used_at IS NULL;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, invalidate_sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string now = utc_now();
            sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, norm.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    // Generate cryptographic 6-digit code
    std::string code = generate_numeric_code(6);
    std::string code_hmac = compute_hmac_sha256(code, config_.auth_secret);

    std::string code_id = "code-" + generate_session_token().substr(0, 16);
    std::string now = utc_now();
    std::string expires_at = utc_future(config_.code_ttl_seconds);

    // Save HMAC in database (NEVER store cleartext code!)
    {
        const char* insert_sql = R"(
            INSERT INTO auth_codes(id, email, code, code_hash, code_hmac, attempts, max_attempts, expires_at, used_at, created_at)
            VALUES(?, ?, '[HMAC_PROTECTED]', ?, ?, 0, ?, ?, NULL, ?);
        )";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            res.ok = false;
            res.status_code = 500;
            res.user_message = "Erro interno ao registrar chave de autenticação.";
            return res;
        }
        sqlite3_bind_text(stmt, 1, code_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, norm.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, code_hmac.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, code_hmac.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, config_.max_attempts);
        sqlite3_bind_text(stmt, 6, expires_at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, now.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            res.ok = false;
            res.status_code = 500;
            res.user_message = "Erro ao persistir chave no banco de dados.";
            return res;
        }
        sqlite3_finalize(stmt);
    }

    // Send email via SMTP
    std::string send_err;
    if (!send_email(norm, code, send_err)) {
        record_audit(db, norm, ip_address, "REQUEST_CODE", "SMTP_FAILED", send_err);
        res.ok = false;
        res.status_code = 500;
        res.user_message = "Não foi possível entregar o e-mail através do servidor SMTP.";
        res.error_detail = send_err;
        return res;
    }

    if (config_.dev_mode) {
        res.dev_code = code;
    }

    record_audit(db, norm, ip_address, "REQUEST_CODE", "SUCCESS", "Code sent via SMTP");
    res.ok = true;
    res.status_code = 200;
    res.user_message = "Código de acesso enviado com sucesso para seu e-mail institucional.";
    return res;
}

VerifyCodeResult AuthSmtpService::verify_code(sqlite3* db, const std::string& email, const std::string& code, const std::string& ip_address) {
    VerifyCodeResult res;
    std::string norm = normalize_email(email);

    std::string clean_code;
    for (char c : code) {
        if (std::isdigit(static_cast<unsigned char>(c))) clean_code.push_back(c);
    }

    if (norm.empty() || clean_code.size() != 6) {
        res.ok = false;
        res.status_code = 400;
        res.user_message = "Código deve conter exatamente 6 dígitos numéricos.";
        return res;
    }

    // Fetch latest active code
    const char* select_sql = R"(
        SELECT id, email, code_hmac, attempts, max_attempts, expires_at
        FROM auth_codes
        WHERE email=? AND used_at IS NULL
        ORDER BY created_at DESC LIMIT 1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        res.ok = false;
        res.status_code = 500;
        res.user_message = "Erro interno ao consultar código.";
        return res;
    }
    sqlite3_bind_text(stmt, 1, norm.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        record_audit(db, norm, ip_address, "VERIFY_CODE", "FAILED_NO_PENDING_CODE", "No active code for email");
        res.ok = false;
        res.status_code = 400;
        res.user_message = "Código inválido ou inexistente. Solicite um novo código.";
        return res;
    }

    std::string code_id = safe_col_text(stmt, 0);
    std::string stored_hmac = safe_col_text(stmt, 2);
    int attempts = sqlite3_column_int(stmt, 3);
    int max_attempts = sqlite3_column_int(stmt, 4);
    std::string expires_at = safe_col_text(stmt, 5);
    sqlite3_finalize(stmt);

    std::string now = utc_now();

    // Check expiration
    if (now > expires_at) {
        // Invalidate expired code
        const char* exp_sql = "UPDATE auth_codes SET used_at=? WHERE id=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, exp_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, code_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        record_audit(db, norm, ip_address, "VERIFY_CODE", "FAILED_EXPIRED", "Code expired at " + expires_at);
        res.ok = false;
        res.status_code = 400;
        res.user_message = "Este código expirou (validade de 10 minutos). Solicite uma nova chave.";
        return res;
    }

    // Check attempts limit
    attempts += 1;
    if (attempts > max_attempts) {
        // Invalidate code immediately
        const char* block_sql = "UPDATE auth_codes SET attempts=?, used_at=? WHERE id=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, block_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s, 1, attempts);
            sqlite3_bind_text(s, 2, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, code_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        record_audit(db, norm, ip_address, "VERIFY_CODE", "BLOCKED_MAX_ATTEMPTS", "Attempts exceeded");
        res.ok = false;
        res.status_code = 403;
        res.remaining_attempts = 0;
        res.user_message = "Número máximo de tentativas excedido (5). Este código foi invalidado.";
        return res;
    }

    // Verify HMAC
    std::string expected_hmac = compute_hmac_sha256(clean_code, config_.auth_secret);
    if (!constant_time_compare(stored_hmac, expected_hmac)) {
        int remaining = max_attempts - attempts;
        if (remaining <= 0) {
            // Invalidate code on 5th failed attempt
            const char* block_sql = "UPDATE auth_codes SET attempts=?, used_at=? WHERE id=?;";
            sqlite3_stmt* s = nullptr;
            if (sqlite3_prepare_v2(db, block_sql, -1, &s, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(s, 1, attempts);
                sqlite3_bind_text(s, 2, now.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 3, code_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }
            record_audit(db, norm, ip_address, "VERIFY_CODE", "BLOCKED_MAX_ATTEMPTS", "Attempts exhausted");
            res.ok = false;
            res.status_code = 403;
            res.remaining_attempts = 0;
            res.user_message = "Número máximo de tentativas excedido (5). Este código foi invalidado.";
            return res;
        }

        // Increment attempts count
        const char* upd_sql = "UPDATE auth_codes SET attempts=? WHERE id=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, upd_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s, 1, attempts);
            sqlite3_bind_text(s, 2, code_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        record_audit(db, norm, ip_address, "VERIFY_CODE", "FAILED_MISMATCH", "Attempt " + std::to_string(attempts));
        res.ok = false;
        res.status_code = 400;
        res.remaining_attempts = remaining;
        res.user_message = "Código incorreto. Tentativas restantes: " + std::to_string(remaining);
        return res;
    }

    // Code is valid! Mark as used atomically
    {
        const char* use_sql = "UPDATE auth_codes SET attempts=?, used_at=? WHERE id=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, use_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s, 1, attempts);
            sqlite3_bind_text(s, 2, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, code_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }

    // Retrieve full user record
    auto user_opt = find_user_by_email(db, norm);
    if (!user_opt.has_value() || !user_opt->is_active) {
        res.ok = false;
        res.status_code = 403;
        res.user_message = "Usuário inativo ou não autorizado.";
        return res;
    }

    // Create session token (store sha256 in database)
    std::string raw_token = generate_session_token();
    std::string token_hash = sha256_hex(raw_token);
    std::string session_exp = utc_future(86400 * 7); // 7 days
    std::string user_id = user_opt->id;

    {
        const char* sess_sql = R"(
            INSERT INTO sessions(token, token_hash, user_id, email, created_at, expires_at, last_seen_at)
            VALUES(?, ?, ?, ?, ?, ?, ?);
        )";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, sess_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, token_hash.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, user_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, norm.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 6, session_exp.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 7, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }

    // Update last_login_at
    {
        const char* login_sql = "UPDATE users SET last_login_at=? WHERE id=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, login_sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }

    record_audit(db, norm, ip_address, "VERIFY_CODE", "SUCCESS", "Session issued");
    res.ok = true;
    res.status_code = 200;
    res.session_token = raw_token;
    res.user = *user_opt;
    res.user_message = "Autenticação realizada com sucesso.";
    return res;
}

std::optional<UserRecord> AuthSmtpService::validate_session(sqlite3* db, const std::string& session_token) {
    if (!db || session_token.empty()) return std::nullopt;
    std::string token_hash = sha256_hex(session_token);

    const char* sql = R"(
        SELECT u.id, u.email, u.name, u.role, u.is_active, u.created_at, IFNULL(u.last_login_at,''), s.expires_at
        FROM sessions s
        JOIN users u ON s.user_id = u.id
        WHERE (s.token_hash=? OR s.token=?) LIMIT 1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_token.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<UserRecord> user;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string exp = safe_col_text(stmt, 7);
        std::string now = utc_now();
        if (now <= exp) {
            UserRecord u;
            u.id = safe_col_text(stmt, 0);
            u.email = safe_col_text(stmt, 1);
            u.name = safe_col_text(stmt, 2);
            u.role = safe_col_text(stmt, 3);
            u.is_active = (sqlite3_column_int(stmt, 4) == 1);
            u.created_at = safe_col_text(stmt, 5);
            u.last_login_at = safe_col_text(stmt, 6);

            if (u.is_active) {
                user = u;
            }
        }
    }
    sqlite3_finalize(stmt);

    if (user.has_value()) {
        const char* touch_sql = "UPDATE sessions SET last_seen_at=? WHERE token_hash=? OR token=?;";
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, touch_sql, -1, &s, nullptr) == SQLITE_OK) {
            std::string now = utc_now();
            sqlite3_bind_text(s, 1, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, token_hash.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, session_token.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }
    return user;
}

bool AuthSmtpService::revoke_session(sqlite3* db, const std::string& session_token) {
    if (!db || session_token.empty()) return false;
    std::string token_hash = sha256_hex(session_token);

    const char* sql = "DELETE FROM sessions WHERE token_hash=? OR token=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_token.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool AuthSmtpService::add_user(sqlite3* db, const std::string& email, const std::string& name, const std::string& role) {
    if (!db) return false;
    std::string norm = normalize_email(email);
    std::string r = (role == "admin" || role == "researcher") ? role : "user";
    std::string id = "usr-" + sha256_hex(norm).substr(0, 16);
    std::string now = utc_now();

    const char* sql = R"(
        INSERT INTO users(id, email, name, role, is_active, created_at, last_login_at)
        VALUES(?, ?, ?, ?, 1, ?, '')
        ON CONFLICT(email) DO UPDATE SET name=excluded.name, role=excluded.role, is_active=1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, norm.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, r.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, now.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<UserRecord> AuthSmtpService::list_users(sqlite3* db) {
    std::vector<UserRecord> list;
    if (!db) return list;

    const char* sql = "SELECT id, email, name, role, is_active, created_at, IFNULL(last_login_at,'') FROM users ORDER BY created_at ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        UserRecord u;
        u.id = safe_col_text(stmt, 0);
        u.email = safe_col_text(stmt, 1);
        u.name = safe_col_text(stmt, 2);
        u.role = safe_col_text(stmt, 3);
        u.is_active = (sqlite3_column_int(stmt, 4) == 1);
        u.created_at = safe_col_text(stmt, 5);
        u.last_login_at = safe_col_text(stmt, 6);
        list.push_back(std::move(u));
    }
    sqlite3_finalize(stmt);
    return list;
}

bool AuthSmtpService::set_user_active(sqlite3* db, const std::string& email, bool active) {
    if (!db) return false;
    std::string norm = normalize_email(email);

    const char* sql = "UPDATE users SET is_active=? WHERE email=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, active ? 1 : 0);
    sqlite3_bind_text(stmt, 2, norm.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

} // namespace morfo::auth
