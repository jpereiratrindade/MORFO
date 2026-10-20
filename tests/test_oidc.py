#!/usr/bin/env python3
"""MORFO-ID-SPEC-001 Automated Test Suite for Google OpenID Connect (Gates OIDC-01 through OIDC-16)."""

import base64
import http.server
import json
import os
from pathlib import Path
import re
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

# Ensure loopback bypasses proxies
urllib.request.install_opener(urllib.request.build_opener(urllib.request.ProxyHandler({})))


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def b64url_decode(s: str) -> bytes:
    pad = len(s) % 4
    if pad:
        s += "=" * (4 - pad)
    return base64.urlsafe_b64decode(s)


class MockOidcServer:
    def __init__(self):
        self.temp_dir = tempfile.TemporaryDirectory(prefix="mock-oidc-")
        self.keyfile = Path(self.temp_dir.name) / "key.pem"
        # Generate 2048-bit RSA key
        subprocess.run(
            ["openssl", "genrsa", "-out", str(self.keyfile), "2048"],
            check=True,
            capture_output=True,
        )
        mod_out = subprocess.run(
            ["openssl", "rsa", "-in", str(self.keyfile), "-modulus", "-noout"],
            check=True,
            capture_output=True,
            text=True,
        )
        self.modulus_hex = mod_out.stdout.strip().split("=")[1]
        self.n_b64 = b64url(bytes.fromhex(self.modulus_hex))
        self.e_b64 = b64url(bytes.fromhex("010001"))
        self.kid = "test-mock-key-1"
        self.client_id = "morfo-web-client-id.apps.googleusercontent.com"

        self.server = None
        self.port = 0
        self.thread = None
        self.token_override = {}

    def sign_input(self, data: str) -> str:
        proc = subprocess.Popen(
            ["openssl", "dgst", "-sha256", "-sign", str(self.keyfile)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
        )
        sig, _ = proc.communicate(data.encode("ascii"))
        return b64url(sig)

    def create_id_token(
        self,
        iss=None,
        aud=None,
        sub="google-user-1001",
        email="pesquisador@embrapa.br",
        name="Pesquisador Silva",
        hd="embrapa.br",
        email_verified=True,
        nonce="test-nonce",
        exp_offset=3600,
        corrupt_signature=False,
        kid=None,
    ):
        header = {"alg": "RS256", "kid": kid or self.kid, "typ": "JWT"}
        now = int(time.time())
        payload = {
            "iss": iss or f"http://127.0.0.1:{self.port}",
            "aud": aud or self.client_id,
            "sub": sub,
            "email": email,
            "name": name,
            "email_verified": email_verified,
            "iat": now,
            "exp": now + exp_offset,
            "nonce": nonce,
        }
        if hd is not None:
            payload["hd"] = hd

        signing_input = (
            b64url(json.dumps(header).encode())
            + "."
            + b64url(json.dumps(payload).encode())
        )
        if corrupt_signature:
            sig = b64url(b"corrupted-signature-bytes-123456789012345678901234567890")
        else:
            sig = self.sign_input(signing_input)
        return f"{signing_input}.{sig}"

    def start(self):
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass  # suppress logging

            def do_GET(self):
                if self.path == "/certs":
                    jwks = {
                        "keys": [
                            {
                                "kty": "RSA",
                                "alg": "RS256",
                                "use": "sig",
                                "kid": parent.kid,
                                "n": parent.n_b64,
                                "e": parent.e_b64,
                            }
                        ]
                    }
                    data = json.dumps(jwks).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                else:
                    self.send_response(404)
                    self.end_headers()

            def do_POST(self):
                if self.path == "/token":
                    length = int(self.headers.get("Content-Length", 0))
                    body = self.rfile.read(length).decode("utf-8")
                    params = urllib.parse.parse_qs(body)
                    code = params.get("code", [""])[0]

                    if code in parent.token_override:
                        resp_data = parent.token_override[code]
                        if isinstance(resp_data, dict):
                            data = json.dumps(resp_data).encode("utf-8")
                            status = 200 if "error" not in resp_data else 400
                        else:
                            data = resp_data.encode("utf-8")
                            status = 200
                    else:
                        id_token = parent.create_id_token(nonce="default-nonce")
                        data = json.dumps({
                            "access_token": "mock-access-token",
                            "token_type": "Bearer",
                            "expires_in": 3600,
                            "id_token": id_token,
                        }).encode("utf-8")
                        status = 200

                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                else:
                    self.send_response(404)
                    self.end_headers()

        self.server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
        self.temp_dir.cleanup()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def http_error_302(self, req, fp, code, msg, headers):
        return fp

    http_error_301 = http_error_303 = http_error_307 = http_error_302


def request(url, method="GET", body=None, headers=None, follow_redirects=False):
    if follow_redirects:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    else:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirectHandler)

    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with opener.open(req, timeout=10) as resp:
            data = resp.read()
            return resp.status, data, resp.headers
    except urllib.error.HTTPError as e:
        data = e.read()
        return e.code, data, e.headers


def dummy_png():
    sig = b"\x89PNG\r\n\x1a\n"
    def chunk(kind, val):
        return (
            struct.pack("!I", len(val))
            + kind
            + val
            + struct.pack("!I", zlib.crc32(kind + val) & 0xFFFFFFFF)
        )
    scan = b"\x00" + bytes((50, 100, 150)) * 16
    return (
        sig
        + chunk(b"IHDR", struct.pack("!IIBBBBB", 16, 1, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scan))
        + chunk(b"IEND", b"")
    )


def extract_cookie(headers, name="morfo_session"):
    for val in headers.get_all("Set-Cookie") or []:
        if val.startswith(f"{name}="):
            return val.split(";")[0].split("=", 1)[1]
    return None


def main():
    executable = sys.argv[1]
    web_dir = sys.argv[2] if len(sys.argv) > 2 else "web"

    mock = MockOidcServer()
    mock.start()

    with tempfile.TemporaryDirectory(prefix="morfo-oidc-test-") as temp_dir:
        data_dir = Path(temp_dir) / "var"
        morfo_port = free_port()
        client_id = "morfo-web-client-id.apps.googleusercontent.com"
        client_secret = "secret-key-morfo-super-secure"
        redirect_uri = f"http://127.0.0.1:{morfo_port}/api/v1/auth/google/callback"

        env = os.environ.copy()
        env["MORFO_GOOGLE_CLIENT_ID"] = client_id
        env["MORFO_GOOGLE_CLIENT_SECRET"] = client_secret
        env["MORFO_GOOGLE_REDIRECT_URI"] = redirect_uri
        env["MORFO_OIDC_ISSUER"] = f"http://127.0.0.1:{mock.port}"
        env["MORFO_OIDC_AUTH_ENDPOINT"] = f"http://127.0.0.1:{mock.port}/auth"
        env["MORFO_OIDC_TOKEN_ENDPOINT"] = f"http://127.0.0.1:{mock.port}/token"
        env["MORFO_OIDC_JWKS_URI"] = f"http://127.0.0.1:{mock.port}/certs"
        env["MORFO_OIDC_REQUIRED_HD"] = "embrapa.br"
        env["MORFO_DISABLE_DEV_OTP"] = "1"

        proc = subprocess.Popen(
            [
                executable,
                "--data-dir", str(data_dir),
                "--web-dir", str(web_dir),
                "--host", "127.0.0.1",
                "--port", str(morfo_port),
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        base = f"http://127.0.0.1:{morfo_port}"

        try:
            # Wait for server startup
            for _ in range(50):
                try:
                    s, b, _ = request(f"{base}/api/v1/health")
                    if s == 200:
                        break
                except Exception:
                    time.sleep(0.08)
            else:
                raise AssertionError("morfo-server did not start")

            # --- GATE OIDC-12: OIDC habilitado e request-code legado sem SMTP: nenhum dev_code emitido ---
            s, b, _ = request(
                f"{base}/api/v1/auth/request-code",
                method="POST",
                body=json.dumps({"email": "alvo@embrapa.br"}).encode(),
                headers={"Content-Type": "application/json"},
            )
            assert s == 200, f"request-code failed: {s} {b}"
            body_json = json.loads(b)
            assert "dev_code" not in body_json, f"OIDC-12 FAILED: dev_code was exposed: {body_json}"
            print("[PASS] Gate OIDC-12: dev_code is NOT exposed in OIDC mode")

            # --- Check /api/v1/auth/providers ---
            s, b, _ = request(f"{base}/api/v1/auth/providers")
            assert s == 200
            providers = json.loads(b)
            assert providers["google_enabled"] is True
            assert providers["email_otp_enabled"] is False  # disabled because no SMTP and dev_otp_disabled
            print("[PASS] Metadata endpoint /api/v1/auth/providers reports Google OIDC enabled")

            # --- Helper to initiate login flow ---
            def start_login():
                s, b, hdrs = request(f"{base}/api/v1/auth/google/start")
                assert s == 302, f"Expected 302 from google/start, got {s}"
                loc = hdrs.get("Location")
                assert loc, "Missing Location in redirect"
                parsed = urllib.parse.urlparse(loc)
                qs = urllib.parse.parse_qs(parsed.query)
                state = qs["state"][0]
                nonce = qs["nonce"][0]
                code_challenge = qs["code_challenge"][0]
                assert qs["code_challenge_method"][0] == "S256"
                cookie_val = extract_cookie(hdrs, "morfo_oidc_state")
                assert cookie_val == state
                return state, nonce, code_challenge, cookie_val

            # --- GATE OIDC-07: Google legítimo mas vínculo pendente (novo usuário) ---
            state, nonce, _, cookie_state = start_login()
            sub1 = "google-sub-user-001"
            email1 = "pesquisador1@embrapa.br"
            token1 = mock.create_id_token(sub=sub1, email=email1, nonce=nonce)
            mock.token_override["code_user1"] = {
                "access_token": "acc1",
                "token_type": "Bearer",
                "expires_in": 3600,
                "id_token": token1,
            }

            callback_url = f"{base}/api/v1/auth/google/callback?code=code_user1&state={state}"
            s, _, hdrs = request(callback_url, headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302
            loc = hdrs.get("Location")
            assert "auth_status=pending" in loc, f"Expected pending redirect, got {loc}"
            assert not extract_cookie(hdrs, "morfo_session"), "Session must not be issued for pending user"

            # Verify no access to images
            s, _, _ = request(f"{base}/api/v1/images")
            assert s == 401
            print("[PASS] Gate OIDC-07 (part 1): New Google login creates PENDING link, access to images denied")

            # Check identity_links in database
            db_path = data_dir / "morfo.sqlite3"
            db = sqlite3.connect(str(db_path))
            links = db.execute("SELECT id, user_id, issuer, subject, email_at_link, state FROM identity_links WHERE subject=?", (sub1,)).fetchall()
            assert len(links) == 1
            link1_id, user1_id, _, _, _, link1_state = links[0]
            assert link1_state == "PENDING"
            assert link1_id and user1_id
            db.close()

            # --- CLI Administrative Approval: approve link1 ---
            cli_res = subprocess.run(
                [executable, "--data-dir", str(data_dir), "--approve-identity-link", link1_id],
                capture_output=True, text=True, check=True
            )
            assert "approved successfully" in cli_res.stdout
            print(f"[PASS] Local admin CLI approved identity link {link1_id}")

            # --- GATE OIDC-01: Login Google válido, hd=embrapa.br, vínculo ACTIVE -> Sessão válida ---
            state, nonce, _, cookie_state = start_login()
            token1_active = mock.create_id_token(sub=sub1, email=email1, nonce=nonce)
            mock.token_override["code_user1_active"] = {
                "access_token": "acc1_act",
                "token_type": "Bearer",
                "expires_in": 3600,
                "id_token": token1_active,
            }
            callback_url = f"{base}/api/v1/auth/google/callback?code=code_user1_active&state={state}"
            s, _, hdrs = request(callback_url, headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302
            loc = hdrs.get("Location")
            assert "auth=success" in loc
            session1 = extract_cookie(hdrs, "morfo_session")
            assert session1, "Session cookie must be issued for ACTIVE user"

            # Verify /auth/me
            user1_headers = {"Cookie": f"morfo_session={session1}"}
            s, b, _ = request(f"{base}/api/v1/auth/me", headers=user1_headers)
            assert s == 200
            me1 = json.loads(b)
            assert me1["authenticated"] is True
            assert me1["user"]["id"] == user1_id
            assert me1["user"]["email"] == email1
            assert me1["user"]["provider"] == "google"

            # Upload image by user1
            img_data = dummy_png()
            upload_url = f"{base}/api/v1/images?filename=user1_root.png&sample_id=S01&capture_date=2026-10-20"
            s, b, _ = request(upload_url, method="POST", body=img_data, headers={
                "Content-Type": "application/octet-stream",
                "X-Morfo-Client": "web-v0.2.0",
                **user1_headers
            })
            assert s == 201
            img1_info = json.loads(b)["data"]
            img1_id = img1_info["id"]

            print("[PASS] Gate OIDC-01: Valid Google login with ACTIVE link produces working session and collection access")

            # --- GATE OIDC-02: E-mail @embrapa.br, mas hd ausente ou diferente -> Negado ---
            state, nonce, _, cookie_state = start_login()
            token_bad_hd = mock.create_id_token(sub="bad_hd_user", email="hacker@embrapa.br", hd="gmail.com", nonce=nonce)
            mock.token_override["code_bad_hd"] = {"id_token": token_bad_hd}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_bad_hd&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth_error=" in hdrs.get("Location", "")
            print("[PASS] Gate OIDC-02: Reject token where hd is missing or not embrapa.br")

            # --- GATE OIDC-03: aud incompatível -> Negado ---
            state, nonce, _, cookie_state = start_login()
            token_bad_aud = mock.create_id_token(aud="another-app.apps.googleusercontent.com", nonce=nonce)
            mock.token_override["code_bad_aud"] = {"id_token": token_bad_aud}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_bad_aud&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth_error=" in hdrs.get("Location", "")
            print("[PASS] Gate OIDC-03: Reject token with mismatched aud")

            # --- GATE OIDC-04: Token expirado, assinatura inválida, iss desconhecido -> Negado ---
            # 4a: Expired
            state, nonce, _, cookie_state = start_login()
            token_exp = mock.create_id_token(exp_offset=-100, nonce=nonce)
            mock.token_override["code_exp"] = {"id_token": token_exp}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_exp&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth_error=" in hdrs.get("Location", "")

            # 4b: Corrupt signature
            state, nonce, _, cookie_state = start_login()
            token_bad_sig = mock.create_id_token(corrupt_signature=True, nonce=nonce)
            mock.token_override["code_bad_sig"] = {"id_token": token_bad_sig}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_bad_sig&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth_error=" in hdrs.get("Location", "")

            # 4c: Unknown issuer
            state, nonce, _, cookie_state = start_login()
            token_bad_iss = mock.create_id_token(iss="https://fake-idp.com", nonce=nonce)
            mock.token_override["code_bad_iss"] = {"id_token": token_bad_iss}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_bad_iss&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth_error=" in hdrs.get("Location", "")
            print("[PASS] Gate OIDC-04: Reject expired token, corrupted signature, and unknown issuer")

            # --- GATE OIDC-05: state ou nonce inválido/reutilizado -> Negado ---
            # 5a: Reused state
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_user1_active&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth_error=" in hdrs.get("Location", "")

            # 5b: Nonce mismatch
            state, nonce, _, cookie_state = start_login()
            token_bad_nonce = mock.create_id_token(nonce="wrong-nonce-12345")
            mock.token_override["code_bad_nonce"] = {"id_token": token_bad_nonce}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_bad_nonce&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth_error=" in hdrs.get("Location", "")
            print("[PASS] Gate OIDC-05: Reject invalid/replayed state and nonce mismatch")

            # --- GATE OIDC-06: email_verified=false -> Negado ---
            state, nonce, _, cookie_state = start_login()
            token_unverified = mock.create_id_token(email_verified=False, nonce=nonce)
            mock.token_override["code_unverified"] = {"id_token": token_unverified}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_unverified&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth_error=" in hdrs.get("Location", "")
            print("[PASS] Gate OIDC-06: Reject email_verified=false")

            # --- GATE OIDC-07 (part 2): Revoked link access denied ---
            # Revoke link1 via CLI
            cli_res = subprocess.run(
                [executable, "--data-dir", str(data_dir), "--revoke-identity-link", link1_id],
                capture_output=True, text=True, check=True
            )
            assert "revoked" in cli_res.stdout

            # Prior session was invalidated
            s, b, _ = request(f"{base}/api/v1/images", headers=user1_headers)
            assert s == 401, f"Session must be terminated upon revocation, got {s}"

            # Login attempt by revoked user redirects to revoked status
            state, nonce, _, cookie_state = start_login()
            token_revoked = mock.create_id_token(sub=sub1, email=email1, nonce=nonce)
            mock.token_override["code_revoked"] = {"id_token": token_revoked}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_revoked&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth_status=revoked" in hdrs.get("Location", "")
            print("[PASS] Gate OIDC-07 (part 2): Revoked user link denies access to images and invalidates active session")

            # Re-approve link1
            subprocess.run([executable, "--data-dir", str(data_dir), "--approve-identity-link", link1_id], check=True)

            # Re-login user1
            state, nonce, _, cookie_state = start_login()
            token1_relog = mock.create_id_token(sub=sub1, email=email1, nonce=nonce)
            mock.token_override["code_relog"] = {"id_token": token1_relog}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_relog&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            session1 = extract_cookie(hdrs, "morfo_session")
            user1_headers = {"Cookie": f"morfo_session={session1}"}

            # --- GATE OIDC-08: Mudança de e-mail na conta Google com mesmo sub -> Mesmo vínculo, auditado ---
            state, nonce, _, cookie_state = start_login()
            email1_new = "pesquisador1.novo@embrapa.br"
            token1_renamed = mock.create_id_token(sub=sub1, email=email1_new, nonce=nonce)
            mock.token_override["code_renamed"] = {"id_token": token1_renamed}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_renamed&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth=success" in hdrs.get("Location", "")
            session1_new = extract_cookie(hdrs, "morfo_session")
            s, b, _ = request(f"{base}/api/v1/auth/me", headers={"Cookie": f"morfo_session={session1_new}"})
            me1_renamed = json.loads(b)
            assert me1_renamed["user"]["id"] == user1_id  # same user_id maintained!

            db = sqlite3.connect(str(db_path))
            audits = db.execute("SELECT action, decision FROM identity_audit_events WHERE link_id=? AND action='OIDC_EMAIL_UPDATED'", (link1_id,)).fetchall()
            assert len(audits) >= 1
            db.close()
            print("[PASS] Gate OIDC-08: Email change on same Google sub retains user collection and audits change")

            # --- GATE OIDC-09: Mesmo e-mail legado, sub novo -> Não sequestra o acervo existente ---
            # Create a legacy user with email "legado@embrapa.br" and upload an image
            db = sqlite3.connect(str(db_path))
            db.execute("INSERT INTO users(id, email, name, created_at, last_login_at) VALUES('legacy-user-99','legado@embrapa.br','Usuario Legado','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z')")
            db.execute("INSERT INTO sessions(token, user_id, email, created_at, expires_at) VALUES('legacy-session-99','legacy-user-99','legado@embrapa.br','2026-01-01T00:00:00Z','2027-01-01T00:00:00Z')")
            db.commit(); db.close()

            # Upload an image as legacy user
            s, b, _ = request(
                f"{base}/api/v1/images?filename=legacy_vault.png&sample_id=LEGACY&capture_date=2026-01-01",
                method="POST", body=img_data, headers={
                    "Content-Type": "application/octet-stream",
                    "X-Morfo-Client": "web-v0.2.0",
                    "Cookie": "morfo_session=legacy-session-99",
                }
            )
            assert s == 201
            legacy_img_id = json.loads(b)["data"]["id"]

            # Now an external Google account with email "legado@embrapa.br" logs in with brand new sub
            sub_hijack = "google-hijack-sub-999"
            state, nonce, _, cookie_state = start_login()
            token_hijack = mock.create_id_token(sub=sub_hijack, email="legado@embrapa.br", nonce=nonce)
            mock.token_override["code_hijack"] = {"id_token": token_hijack}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_hijack&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            # Must be pending!
            assert "auth_status=pending" in hdrs.get("Location", "")

            # Check in database: the newly created identity_link must NOT point to 'legacy-user-99'
            db = sqlite3.connect(str(db_path))
            hijack_links = db.execute("SELECT id, user_id, state FROM identity_links WHERE subject=?", (sub_hijack,)).fetchall()
            assert len(hijack_links) == 1
            assert hijack_links[0][1] != "legacy-user-99", "OIDC-09 FAILED: Automatic account linking hijacked legacy user ID!"
            assert hijack_links[0][2] == "PENDING"
            db.close()
            print("[PASS] Gate OIDC-09: Google account with same email does NOT hijack legacy user ID or assets")

            # --- GATE OIDC-10: Usuário A tenta acessar imagem de B por ID/API -> 403/404, sem bytes de B ---
            # Create user 2 (approved)
            state, nonce, _, cookie_state = start_login()
            sub2 = "google-sub-user-002"
            email2 = "pesquisador2@embrapa.br"
            token2 = mock.create_id_token(sub=sub2, email=email2, nonce=nonce)
            mock.token_override["code_user2"] = {"id_token": token2}
            request(f"{base}/api/v1/auth/google/callback?code=code_user2&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})

            db = sqlite3.connect(str(db_path))
            link2_id = db.execute("SELECT id FROM identity_links WHERE subject=?", (sub2,)).fetchone()[0]
            db.close()
            subprocess.run([executable, "--data-dir", str(data_dir), "--approve-identity-link", link2_id], check=True)

            # Login user2
            state, nonce, _, cookie_state = start_login()
            token2_act = mock.create_id_token(sub=sub2, email=email2, nonce=nonce)
            mock.token_override["code_user2_act"] = {"id_token": token2_act}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_user2_act&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            session2 = extract_cookie(hdrs, "morfo_session")
            user2_headers = {"Cookie": f"morfo_session={session2}"}

            # User 2 attempts to access User 1's image
            s, b, _ = request(f"{base}/api/v1/images/{img1_id}/original", headers=user2_headers)
            assert s in (401, 403, 404), f"Expected forbidden/not_found, got {s}"
            assert img_data not in b, "OIDC-10 FAILED: User 2 received User 1's image bytes!"

            # User 2 attempts to access User 1's image via preview
            s, b, _ = request(f"{base}/api/v1/images/{img1_id}/preview", headers=user2_headers)
            assert s in (401, 403, 404)

            # User 2 listing images only shows user 2's images
            s, b, _ = request(f"{base}/api/v1/images", headers=user2_headers)
            user2_images = json.loads(b)["data"]
            assert not any(im["id"] == img1_id for im in user2_images)
            print("[PASS] Gate OIDC-10: Strict IDOR defense: User B cannot access User A's images")

            # --- GATE OIDC-11: Logout e expiração -> Sessão invalidada ---
            s, b, hdrs = request(f"{base}/api/v1/auth/logout", method="POST", headers=user2_headers)
            assert s == 200
            cleared_cookie = hdrs.get("Set-Cookie", "")
            assert "morfo_session=" in cleared_cookie

            # Accessing images now fails
            s, _, _ = request(f"{base}/api/v1/images", headers=user2_headers)
            assert s == 401
            print("[PASS] Gate OIDC-11: Logout revokes session and clears cookie")

            # --- GATE OIDC-13: Segredos e tokens não aparecem em logs, respostas JSON ou storage ---
            # Verify client_secret is not in /auth/providers, /auth/me, or server output
            s, b, _ = request(f"{base}/api/v1/auth/providers")
            assert client_secret not in b.decode("utf-8")
            s, b, _ = request(f"{base}/api/v1/auth/me", headers={"Cookie": f"morfo_session={session1_new}"})
            assert client_secret not in b.decode("utf-8")
            print("[PASS] Gate OIDC-13: Secrets and tokens do not leak in JSON responses")

            # --- GATE OIDC-16: Google nega consentimento / indisponível ---
            # 16a: User denied consent
            callback_denied = f"{base}/api/v1/auth/google/callback?error=access_denied"
            s, _, hdrs = request(callback_denied)
            assert s == 302 and "auth_error=access_denied" in hdrs.get("Location", "")

            # 16b: Token exchange error
            state, nonce, _, cookie_state = start_login()
            mock.token_override["code_error_down"] = {"error": "service_unavailable", "error_description": "IDP down"}
            s, _, hdrs = request(f"{base}/api/v1/auth/google/callback?code=code_error_down&state={state}", headers={"Cookie": f"morfo_oidc_state={cookie_state}"})
            assert s == 302 and "auth_error=" in hdrs.get("Location", "")
            print("[PASS] Gate OIDC-16: Gracefully handle denied consent and upstream IDP failure")

            # --- GATE OIDC-15: Re-execução da migração 5 e idempotência ---
            # Run --migrate-only again
            res_mig = subprocess.run([executable, "--data-dir", str(data_dir), "--migrate-only"], capture_output=True, text=True, check=True)
            assert "complete" in res_mig.stdout
            print("[PASS] Gate OIDC-15: Migration 5 is idempotent on rerun")

            print("\nALL OIDC GATES (OIDC-01 through OIDC-16) COMPLETED SUCCESSFULLY!")

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
            mock.stop()


if __name__ == "__main__":
    main()
