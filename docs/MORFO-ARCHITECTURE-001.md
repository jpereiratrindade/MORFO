# MORFO-ARCHITECTURE-001 — Arquitetura inicial de implementação

**Versão:** 0.1.0 · **Estado:** EXPERIMENTAL

```
Browser: interface herdada do RIZOMA
  ├─ acervo/upload/pan/zoom/pirâmide multirresolução
  └─ seletor MORFO de perfis
            │ HTTP/API com autenticação institucional original (piloto)
            ▼
Servidor MORFO C++26 (origem: RIZOMA)
  ├─ identidade e política de acesso / SQLite WAL (`var/morfo.sqlite3`)
  ├─ armazenamento por hash de originais + derivados
  ├─ GET /api/v1/profiles                       → catálogo
  ├─ POST /api/v1/images/{id}/profiles/roots-exp003
  ├─ GET  /api/v1/images/{id}/profiles/roots-exp003
  └─ GET  /api/v1/images/{id}/profiles/roots-exp003/{run}/mask|overlay|report
                         │
                         ▼
                Perfil temático `roots-exp003`
                • código original EXP-003 (mediana/MAD, top-hat, geometria)
                • máscara PNG + sobreposição + relatório JSON
                • estado NOT_VALIDATED
                         │
                         ▼
                Futuro módulo `discovery` — PRE/REA/RIT
                • não supervisionado/autossupervisionado
                • criação de categorias com evidência, revisões versionadas
```

## Implantação e isolamento
- Por padrão, `0.0.0.0:8091`, acessível pela rede local quando permitido pelo firewall; não concorre com o RIZOMA (`8089`).
- `var/`, banco e blobs separados; não importar dados privados do RIZOMA.
- Cookie de sessão `morfo_session`, separado do cookie RIZOMA; modelo atual continua restrito a `@embrapa.br` por herança.
- Esta entrega não prova segurança de operação Internet/multiusuário. Exigir HTTPS, backup/restauração, observabilidade, controle de quotas e testes de segurança adicionais antes de exposição.
- Código legado interno mantém alguns identificadores `rizoma::` por compatibilidade; refatoração posterior com gates.

## Restrições operacionais
- O perfil EXP-003 processa imagens até **12 megapixels** sem reamostragem; limites maiores aguardam divisão por blocos e reconciliação de bordas validada.
- A interface web usa máscara gerada pelo backend C++26, não sua heurística antiga baseada em RGB para produzir o resultado do novo perfil.
- Resultados existentes da interface legada são preservados como recursos herdados, mas **não** representam a análise do perfil novo.
- O serviço de perfil é síncrono nessa fase e pode ocupar recurso de requisição; fila persistente e execução assíncrona são uma prioridade posterior.
- Extensões como classificação automática genérica, segmentação neural, calibração de escala e medições agronômicas não são alegadas como prontas.

## Gates
- G0: instalar do lado de RIZOMA sem alteração do repositório de origem.
- G1: compilar em C++26 e preservar interface/acervo.
- G2: **golden test**: imagem 850×757 => exatamente 3.339 pixels e 30 componentes; máscara pixel a pixel igual ao EXP-003.
- G3: POST tema raízes retorna máscara/overlay/relatório e bloqueia leitura cruzada entre usuários.
- G4: teste científico independente com anotações humanas; avaliar sensibilidade/precisão, Dice/IoU, raízes finas, artefatos, solo e borda.
- G5: criar protótipo genérico de descoberta (PRE) e comparar com EXP-003; REA/RIT com contrato de estados e migrações.

## Delimitação legal e científica
RIZOMA é GPLv3; os avisos são preservados. A fixture real incluída no pacote é privada, fica em `tests/fixtures/private/` e é ignorada pelo Git; confirmar consentimento antes de distribuir/compartilhar. A saída reproduzível é evidência computacional, não identificação biológica validada.
