# MORFO / LABECO-ID — Implementação OIDC Web (MORFO-ID-SPEC-001)

**Documento:** MORFO-ID-DOC-001  
**Versão:** 0.1.0 · 20/10/2026  
**Lema:** *O perfeito não existe. Sempre pronto. Sempre incompleto.*  
**Commit base:** `2195b6a` (Export confirmed refinements as ZIP bundles) na branch `main`.  
**Patch binário correspondente:** `MORFO-ID-PATCH-001.patch`

---

## 1. Visão Geral da Entrega

Em conformidade com a especificação **MORFO-ID-SPEC-001**, foi integrada a autenticação federada **Google OpenID Connect (Authorization Code Flow + PKCE S256)** à aplicação MORFO Web (C++26, Boost.Beast/Asio, OpenSSL 3.5, SQLite WAL).

### Principais Garantias e Invariantes Preservados:
1. **Acervo e Permissões:** O isolamento rigoroso por `user_id` e a propriedade das imagens foram preservados integralmente em todos os endpoints de visualização, análise, calibração e refinamento regional.
2. **Sem Auto-vínculo Implícito:** Contas Google recém-autenticadas não herdam acervos existentes nem assumem posse de dados apenas por coincidência de endereço de e-mail (`PENDING` por padrão). A vinculação ou aprovação é realizada explicitamente via CLI administrativo local autenticado no sistema operacional.
3. **Validação Criptográfica Rigorosa:** Verificação oficial JWS (RS256) contra chaves públicas do Google (JWKS), checagem estrita de `iss`, `aud`, `exp`, `iat`, `nonce`, `email_verified=true` e exigência obrigatória de claim assinado **`hd=embrapa.br`**.
4. **Sem Dependência de Gmail API:** Não há solicitação de escopos intrusivos (`gmail.readonly`, `gmail.send`, Google Drive, etc.). Solicitam-se unicamente os escopos mínimos OIDC `openid email profile`.
5. **Modo OIDC Seguro:** Ao ativar o modo OIDC (`MORFO_GOOGLE_CLIENT_ID` presente ou `MORFO_DISABLE_DEV_OTP=1`), o endpoint de solicitação de código OTP desativa a emissão de `dev_code` no payload JSON.
6. **Banco de Dados Aditivo (Migração 5):** Criação das tabelas `identity_links` e `identity_audit_events` com integridade referencial, preservando as migrações 1 a 4 e seus checksums inalterados.

---

## 2. Arquitetura e Fluxo de Autenticação

```text
[Navegador]  -- 1. GET /api/v1/auth/google/start -->  [MORFO Backend]
              <-- 2. 302 Redirect (accounts.google.com) -- [Gera state, nonce, PKCE S256]
[Google IdP] <-------- 3. Autenticação institucional (hd=embrapa.br) --------
             -- 4. 302 Redirect /api/v1/auth/google/callback?code=...&state=... --> [MORFO Backend]
                                                                                |-- Confere state/cookie
                                                                                |-- Troca code via HTTPS POST
                                                                                |-- Valida assinatura JWS/JWKS
                                                                                |-- Confere claims & hd=embrapa.br
                                                                                |-- Consulta identity_links:
                                                                                |     * ACTIVE: emite morfo_session
                                                                                |     * PENDING/REVOKED: sem sessão
                                                                                \-- 302 para / (?auth=success | ?auth_status=pending)
```

---

## 3. Banco de Dados — Migração 5

Adicionada na versão 5 em `src/database.cpp` com checksum SHA-256 verificado:
- **Nome:** `identity-links-and-audit-foundation`
- **Checksum:** `515e374b2909b5190f85fb18da04ca4099762a1e12f32493566c6038bd104fe3`

### Tabelas Criadas:
```sql
CREATE TABLE IF NOT EXISTS identity_links(
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
CREATE INDEX IF NOT EXISTS idx_identity_links_user ON identity_links(user_id);
CREATE INDEX IF NOT EXISTS idx_identity_links_lookup ON identity_links(issuer, subject);

CREATE TABLE IF NOT EXISTS identity_audit_events(
  id TEXT PRIMARY KEY,
  user_id TEXT,
  link_id TEXT,
  action TEXT NOT NULL,
  occurred_at TEXT NOT NULL,
  decision TEXT NOT NULL,
  evidence_type TEXT NOT NULL,
  correlation_id TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_identity_audit_user ON identity_audit_events(user_id);
CREATE INDEX IF NOT EXISTS idx_identity_audit_link ON identity_audit_events(link_id);
```

---

## 4. Comandos Administrativos Locais (CLI)

Para autorizar identidades sem expor endpoints administrativos públicos na web, o executável `morfo-server` oferece os seguintes comandos locais:

### 4.1 Listar Vínculos de Identidade
```bash
./build/morfo-server --data-dir ./var --list-identity-links
```

### 4.2 Aprovar um Vínculo Pendente (Novo Usuário)
```bash
./build/morfo-server --data-dir ./var --approve-identity-link <LINK_ID>
```
*Ativa o vínculo (`state='ACTIVE'`), registrando evento na auditoria.*

### 4.3 Vincular Identidade Google a um Usuário Existente
```bash
./build/morfo-server --data-dir ./var --link-identity <LINK_ID> --user-id <USER_ID>
```
*Atribui o `user_id` legado ao vínculo Google verificado (`state='ACTIVE'`), preservando o acervo do usuário.*

### 4.4 Revogar Vínculo de Identidade
```bash
./build/morfo-server --data-dir ./var --revoke-identity-link <LINK_ID>
```
*Marca o estado como `REVOKED` e invalida imediatamente todas as sessões ativas do usuário no banco.*

---

## 5. Configuração de Ambiente para Produção / Piloto

Não commitar arquivos com credenciais no repositório Git. Configurar via variáveis de ambiente ou arquivo restrito (`chmod 0600`):

```bash
# Credenciais do cliente Google OAuth Web Application
export MORFO_GOOGLE_CLIENT_ID="SEU_CLIENT_ID.apps.googleusercontent.com"
export MORFO_GOOGLE_CLIENT_SECRET="SEU_CLIENT_SECRET"
# Ou via caminho para arquivo de permissão restrita:
# export MORFO_GOOGLE_CLIENT_SECRET_FILE="/etc/morfo/secrets/google_client_secret"

# URI de retorno registrada no Google Cloud Console
export MORFO_GOOGLE_REDIRECT_URI="http://127.0.0.1:8091/api/v1/auth/google/callback"

# Desabilitar dev_code no login por e-mail legado
export MORFO_DISABLE_DEV_OTP=1
```

---

## 6. Instruções de Aplicação e Rollback do Patch

### 6.1 Identificação do Patch
- **Arquivo:** `MORFO-ID-PATCH-001.patch`
- **Geração:** `git diff --binary > MORFO-ID-PATCH-001.patch`
- **Validação de integridade:** conferir o hash SHA-256 reportado na entrega final com `sha256sum MORFO-ID-PATCH-001.patch`.

### 6.2 Verificação Prévia
No diretório raiz do repositório MORFO:
```bash
git apply --check MORFO-ID-PATCH-001.patch
```
*Se a saída for silenciosa (código de saída 0), o patch é aplicável perfeitamente.*

### 6.3 Aplicação do Patch
```bash
git apply MORFO-ID-PATCH-001.patch
```

### 6.4 Compilação e Execução da Suíte de Testes
```bash
# No Silverblue (Fedora Toolbox crivo-dev):
toolbox run -c crivo-dev bash -c "cd /run/media/jpereiratrindade/labeco10T/dev/cpp/MORFO && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel && ctest --test-dir build --output-on-failure"
```

### 6.5 Procedimento de Reversão (Rollback)
```bash
git apply --reverse MORFO-ID-PATCH-001.patch
```

---

## 7. Evidência da Matriz de Testes (Gates OIDC-01 a OIDC-16)

A suíte automatizada `tests/test_oidc.py` foi registrada no CTest como `morfo.oidc-security`. Todos os 16 gates foram validados com 100% de sucesso sem conexão externa (usando provedor Mock OIDC com chave RSA gerada localmente):

| Gate | Cenário Testado | Resultado Observado | Estado |
|---|---|---|:---:|
| **OIDC-01** | Login Google válido, `hd=embrapa.br`, vínculo ACTIVE | Emite cookie `morfo_session`, `/auth/me` autenticado, acesso liberado | **APROVADO** |
| **OIDC-02** | E-mail `@embrapa.br`, mas `hd` ausente ou diferente (`gmail.com`) | Rejeitado, redirect com erro explicativo | **APROVADO** |
| **OIDC-03** | `aud` incompatível com o client_id configurado | Rejeitado antes da sessão | **APROVADO** |
| **OIDC-04** | Token expirado, assinatura RSA corrompida ou `iss` desconhecido | Rejeitado na validação JWS | **APROVADO** |
| **OIDC-05** | `state` adulterado, ausente ou reusado (replay attack), `nonce` divergente | Rejeitado imediatamente (uso único) | **APROVADO** |
| **OIDC-06** | `email_verified=false` no ID token | Rejeitado | **APROVADO** |
| **OIDC-07** | Conta Google legítima com vínculo PENDING ou REVOKED | Redireciona para pendência/revogação; zero acesso a imagens | **APROVADO** |
| **OIDC-08** | Mudança do e-mail da conta Google com mesmo `sub` | Mantém o mesmo `user_id`, atualiza vínculo e audita alteração | **APROVADO** |
| **OIDC-09** | Mesmo e-mail de usuário legado com novo `sub` Google | Não sequestra acervo; cria vínculo isolado PENDING | **APROVADO** |
| **OIDC-10** | Usuário A tenta acessar original/preview de Usuário B | HTTP 401/403/404; nenhum byte de A vaza para B | **APROVADO** |
| **OIDC-11** | Logout e expiração | Sessão removida do banco, cookie expirado, `/images` retorna 401 | **APROVADO** |
| **OIDC-12** | OIDC habilitado e request-code legado sem SMTP | Nenhum `dev_code` devolvido na resposta JSON | **APROVADO** |
| **OIDC-13** | Inspeção de respostas JSON, metadados e logs | Segredos e tokens nunca vazam para o cliente | **APROVADO** |
| **OIDC-14** | Testes anteriores (http-smoke, migrações, roots golden EXP-003) | 7/7 testes CTest verdes, sem regressão | **APROVADO** |
| **OIDC-15** | Reexecução da migração 5 e rollback | Migração 5 idempotente, tabelas íntegras | **APROVADO** |
| **OIDC-16** | Google nega consentimento (`error=access_denied`) ou endpoint indisponível | Tratamento gracioso com redirect legível, sem sessão órfã | **APROVADO** |
