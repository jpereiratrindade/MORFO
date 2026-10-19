# IMG-005A.0 — Migrações, backup e restauração

**Escopo:** preparação segura do SQLite do RIZOMA. Esta etapa não introduz análise científica, EXP-004, JEV, ENTE ou mudanças de interface.

## Migrações

O servidor executa migrações antes de abrir a porta HTTP. `schema_migrations` registra versão, nome, checksum imutável e instante UTC. A migração inteira, incluindo a criação do histórico, usa `BEGIN IMMEDIATE`; qualquer erro provoca `ROLLBACK` e encerra o processo. Uma versão futura desconhecida ou um checksum divergente também impede a inicialização.

A migração `1 — establish-v0.2.0-schema-and-history` reconhece tanto um banco vazio como o esquema legado 0.2.0. Ela cria estruturas ausentes, adiciona `images.user_id` quando necessário, recupera o vínculo por correspondência exata entre `images.user_email` e `users.email`, cria índices e executa `integrity_check`, `foreign_key_check` e verificação dos vínculos conhecidos.

A migração `2 — analysis-review-foundation` adiciona execuções analíticas, regiões candidatas, julgamentos automáticos e eventos humanos append-only. Os testes de migração constroem seus próprios bancos temporários; não dependem de `var/rizoma.sqlite3` nem de qualquer arquivo local não versionado.

Para validar uma cópia sem iniciar o serviço:

```bash
./build/rizoma-server --data-dir /caminho/isolado --migrate-only
```

## Backup consistente antes de migrar

Pare o RIZOMA para evitar alterações de blobs durante o procedimento. Preserve conjuntamente o banco e os diretórios `blobs/`, `derived/` e `staging/`. Não copie apenas o arquivo principal de um SQLite ativo em WAL.

Com o serviço parado, crie o backup do banco pela API de backup do SQLite:

```bash
mkdir -p /destino/backup-rizoma
sqlite3 /dados/rizoma.sqlite3 ".backup '/destino/backup-rizoma/rizoma.sqlite3'"
cp -a /dados/blobs /dados/derived /dados/staging /destino/backup-rizoma/
sha256sum /destino/backup-rizoma/rizoma.sqlite3 > /destino/backup-rizoma/SHA256SUMS
sqlite3 /destino/backup-rizoma/rizoma.sqlite3 'PRAGMA integrity_check; PRAGMA foreign_key_check;'
```

Registre separadamente versão do RIZOMA, instante, operador, origem e hash. A migração de um banco operacional só deve ocorrer depois de conferir esse backup e obter autorização específica.

## Ensaio de restauração

Restaure sempre primeiro em diretório isolado:

```bash
mkdir -p /tmp/rizoma-restore-test
cp -a /destino/backup-rizoma/. /tmp/rizoma-restore-test/
sha256sum -c /destino/backup-rizoma/SHA256SUMS
sqlite3 /tmp/rizoma-restore-test/rizoma.sqlite3 'PRAGMA integrity_check; PRAGMA foreign_key_check;'
./build/rizoma-server --data-dir /tmp/rizoma-restore-test --migrate-only
```

Confira contagens e amostras de `users`, `sessions` e `images`, além da existência e hash dos arquivos referenciados, antes de considerar a restauração válida.

## Diagnóstico de segurança pendente

Antes de disponibilização institucional devem ser corrigidos e testados:

- OTP ainda é persistido em texto simples, apesar da presença de `code_hash`;
- o código OTP é devolvido/exibido no modo de desenvolvimento;
- não há limitação de tentativas nem bloqueio progressivo;
- SMTP é executado por comando externo construído para `popen`, ampliando riscos de injeção, exposição de credenciais e dependência do ambiente;
- o serviço não deve ter sua exposição de rede ampliada até esses controles e HTTPS serem tratados.

Esses riscos foram apenas registrados no IMG-005A.0; alterar autenticação ou SMTP está fora deste incremento.
