#include "auth_smtp.hpp"
#include "database.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace morfo::auth;

#define CHECK(expr) do { \
    if (!(expr)) { \
        throw std::runtime_error(std::string("Assertion failed: ") + #expr + " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
    } \
} while (0)

static void test_crypto_primitives() {
    std::cout << "[TEST] Cryptographic primitives...\n";

    // 1. 6-digit numeric code
    for (int i = 0; i < 50; ++i) {
        std::string code = generate_numeric_code(6);
        CHECK(code.size() == 6);
        for (char c : code) {
            CHECK(c >= '0' && c <= '9');
        }
    }

    // 2. HMAC-SHA256 determinism & consistency
    std::string secret = "test-secret-key-1234567890";
    std::string h1 = compute_hmac_sha256("123456", secret);
    std::string h2 = compute_hmac_sha256("123456", secret);
    std::string h3 = compute_hmac_sha256("654321", secret);
    CHECK(!h1.empty());
    CHECK(h1 == h2);
    CHECK(h1 != h3);
    CHECK(constant_time_compare(h1, h2));
    CHECK(!constant_time_compare(h1, h3));

    // 3. SHA-256 Hex
    std::string token = generate_session_token();
    CHECK(token.size() == 64);
    std::string thash = sha256_hex(token);
    CHECK(thash.size() == 64);

    std::cout << "[PASS] Cryptographic primitives\n";
}

static void test_rate_limiter() {
    std::cout << "[TEST] Rate limiter (cooldown & sliding windows)...\n";
    RateLimiter rl;

    std::string email = "pesquisador@embrapa.br";
    std::string ip = "10.163.80.50";

    // First request must succeed (retry_after == 0)
    int wait1 = rl.check_and_record_request(email, ip, 60);
    CHECK(wait1 == 0);

    // Immediate second request must be blocked by 60s cooldown
    int wait2 = rl.check_and_record_request(email, ip, 60);
    CHECK(wait2 > 0 && wait2 <= 60);

    // Another email from same IP should pass cooldown for that email
    std::string email2 = "outro.pesquisador@embrapa.br";
    int wait3 = rl.check_and_record_request(email2, ip, 60);
    CHECK(wait3 == 0);

    std::cout << "[PASS] Rate limiter\n";
}

static void test_auth_service_full_lifecycle() {
    std::cout << "[TEST] AuthSmtpService full lifecycle...\n";

    fs::path temp_dir = fs::temp_directory_path() / ("morfo-auth-test-" + generate_session_token().substr(0, 8));
    fs::create_directories(temp_dir);
    fs::path db_path = temp_dir / "morfo.sqlite3";

    // Apply database migrations
    rizoma::database::migrate(db_path);

    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    CHECK(rc == SQLITE_OK && db != nullptr);

    AuthSmtpConfig cfg;
    cfg.simulated = true;
    cfg.dev_mode = true; // Returns dev_code for test verification
    cfg.auth_secret = "test-secret-salt-abc";
    cfg.cooldown_seconds = 0; // Disable cooldown for rapid test execution
    cfg.max_attempts = 5;
    cfg.code_ttl_seconds = 600;

    AuthSmtpService svc(cfg);

    // 1. Add user via administrative CLI API
    std::cout << "  [Step 1] add_user...\n";
    bool added = svc.add_user(db, "jpereiratrindade@gmail.com", "José Pedro", "admin");
    CHECK(added);

    auto users = svc.list_users(db);
    CHECK(users.size() == 1);
    CHECK(users[0].email == "jpereiratrindade@gmail.com");
    CHECK(users[0].role == "admin");
    CHECK(users[0].is_active);

    // 2. Request OTP code
    std::cout << "  [Step 2] request_code...\n";
    auto req_res = svc.request_code(db, "jpereiratrindade@gmail.com", "127.0.0.1");
    CHECK(req_res.ok);
    CHECK(req_res.status_code == 200);
    CHECK(!req_res.dev_code.empty());
    std::string valid_code = req_res.dev_code;
    CHECK(valid_code.size() == 6);

    // Verify DB does NOT store plaintext code
    sqlite3_stmt* check_st = nullptr;
    rc = sqlite3_prepare_v2(db, "SELECT code_hmac FROM auth_codes WHERE email=? AND used_at IS NULL", -1, &check_st, nullptr);
    CHECK(rc == SQLITE_OK);
    sqlite3_bind_text(check_st, 1, "jpereiratrindade@gmail.com", -1, SQLITE_TRANSIENT);
    int step_rc = sqlite3_step(check_st);
    CHECK(step_rc == SQLITE_ROW);
    const auto* hmac_txt = sqlite3_column_text(check_st, 0);
    CHECK(hmac_txt != nullptr);
    std::string stored_hmac = reinterpret_cast<const char*>(hmac_txt);
    CHECK(!stored_hmac.empty());
    CHECK(stored_hmac != valid_code);
    CHECK(stored_hmac == compute_hmac_sha256(valid_code, cfg.auth_secret));
    sqlite3_finalize(check_st);

    // 3. Test wrong codes and max 5 attempts lockout
    std::cout << "  [Step 3] wrong codes & lockout...\n";
    for (int attempt = 1; attempt <= 4; ++attempt) {
        auto fail_res = svc.verify_code(db, "jpereiratrindade@gmail.com", "000000", "127.0.0.1");
        CHECK(!fail_res.ok);
        CHECK(fail_res.status_code == 400);
        CHECK(fail_res.remaining_attempts == (5 - attempt));
    }

    // 5th failed attempt must block & invalidate the code
    auto lock_res = svc.verify_code(db, "jpereiratrindade@gmail.com", "000000", "127.0.0.1");
    CHECK(!lock_res.ok);
    CHECK(lock_res.status_code == 403); // Forbidden, blocked

    // Even with the correct code now, verification must be rejected because attempts exceeded
    auto late_res = svc.verify_code(db, "jpereiratrindade@gmail.com", valid_code, "127.0.0.1");
    CHECK(!late_res.ok);

    // 4. Request new code (must invalidate previous code)
    std::cout << "  [Step 4] request new code...\n";
    auto req2 = svc.request_code(db, "jpereiratrindade@gmail.com", "127.0.0.1");
    CHECK(req2.ok);
    std::string valid_code2 = req2.dev_code;
    CHECK(valid_code2 != valid_code);

    // 5. Successful verification creates session
    std::cout << "  [Step 5] verify valid code...\n";
    auto ok_res = svc.verify_code(db, "jpereiratrindade@gmail.com", valid_code2, "127.0.0.1");
    CHECK(ok_res.ok);
    CHECK(ok_res.status_code == 200);
    CHECK(!ok_res.session_token.empty());
    CHECK(ok_res.user.email == "jpereiratrindade@gmail.com");
    CHECK(ok_res.user.role == "admin");

    std::string token = ok_res.session_token;

    // Verify session token is hashed in DB (not stored in cleartext)
    sqlite3_stmt* sess_st = nullptr;
    rc = sqlite3_prepare_v2(db, "SELECT token_hash FROM sessions WHERE user_id=?", -1, &sess_st, nullptr);
    CHECK(rc == SQLITE_OK);
    sqlite3_bind_text(sess_st, 1, ok_res.user.id.c_str(), -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(sess_st) == SQLITE_ROW);
    const auto* thash_txt = sqlite3_column_text(sess_st, 0);
    CHECK(thash_txt != nullptr);
    std::string stored_thash = reinterpret_cast<const char*>(thash_txt);
    CHECK(!stored_thash.empty());
    CHECK(stored_thash != token);
    CHECK(stored_thash == sha256_hex(token));
    sqlite3_finalize(sess_st);

    // 6. Validate active session
    std::cout << "  [Step 6] validate_session...\n";
    auto valid_user = svc.validate_session(db, token);
    CHECK(valid_user.has_value());
    CHECK(valid_user->email == "jpereiratrindade@gmail.com");
    CHECK(valid_user->role == "admin");

    // 7. Single use check: code cannot be verified again
    auto reuse_res = svc.verify_code(db, "jpereiratrindade@gmail.com", valid_code2, "127.0.0.1");
    CHECK(!reuse_res.ok);

    // 8. Logout revokes session
    bool revoked = svc.revoke_session(db, token);
    CHECK(revoked);
    auto after_logout = svc.validate_session(db, token);
    CHECK(!after_logout.has_value());

    // 9. Deactivated user cannot authenticate
    svc.set_user_active(db, "jpereiratrindade@gmail.com", false);
    auto req_deactivated = svc.request_code(db, "jpereiratrindade@gmail.com", "127.0.0.1");
    auto user_info = svc.find_user_by_email(db, "jpereiratrindade@gmail.com");
    CHECK(user_info.has_value() && !user_info->is_active);

    // 10. Audit log verified
    sqlite3_stmt* audit_st = nullptr;
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM auth_audit_events WHERE email=?", -1, &audit_st, nullptr);
    CHECK(rc == SQLITE_OK);
    sqlite3_bind_text(audit_st, 1, "jpereiratrindade@gmail.com", -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(audit_st) == SQLITE_ROW);
    int audit_count = sqlite3_column_int(audit_st, 0);
    CHECK(audit_count >= 5);
    sqlite3_finalize(audit_st);

    sqlite3_close(db);
    std::error_code ec;
    fs::remove_all(temp_dir, ec);

    std::cout << "[PASS] AuthSmtpService full lifecycle\n";
}

int main() {
    try {
        test_crypto_primitives();
        test_rate_limiter();
        test_auth_service_full_lifecycle();
        std::cout << "\nALL MORFO AUTH SMTP TESTS PASSED!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FATAL TEST FAILURE: " << e.what() << "\n";
        return 1;
    }
}
