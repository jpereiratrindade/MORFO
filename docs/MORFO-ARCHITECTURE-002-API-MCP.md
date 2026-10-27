# MORFO-ARCHITECTURE-002-API-MCP — Arquitetura de Serviços Compartilhados, API REST e Servidor MCP

**Versão:** 0.1.0 · **Data:** 2026-10-22 · **Estado:** ARCHITECTURE_CANDIDATE · **Regra:** Sempre pronto. Sempre incompleto.

---

## 1. Visão Geral e Princípios

O MORFO é concebido como uma plataforma científica autônoma de percepção visual, descoberta de padrões e classificação adaptativa. Para atender tanto a pesquisadores quanto a sistemas automatizados e assistentes cognitivos, o MORFO adota uma **arquitetura de núcleo único compartilhado** com três projeções constitutivas:

1. **Interface Web:** Inspeção visual interativa, anotação direta e controle humano.
2. **API REST (OpenAPI 3.2.1):** Integração programática com aplicações, sensores, pipelines analíticos e plataformas como RIZOMA e NEXO.
3. **Servidor MCP (Model Context Protocol 2026):** Exposição de ferramentas e recursos estruturados para que assistentes de inteligência artificial investiguem as evidências produzidas pelo sistema.

```
Pesquisadores                Aplicações e Sensores          Assistentes de IA
      │                                │                           │
      ▼                                ▼                           ▼
┌──────────────┐               ┌───────────────┐           ┌───────────────┐
│Interface Web │               │   API REST    │           │ Servidor MCP  │
│  (Interativa)│               │ (OpenAPI 3.x) │           │(StreamableHTTP│
└──────┬───────┘               └───────┬───────┘           └───────┬───────┘
       │                               │                           │
       └───────────────────────────────┼───────────────────────────┘
                                       ▼
             ┌──────────────────────────────────────────────────┐
             │       Contratos e Serviços de Domínio            │
             │           (C++26 Application Layer)              │
             │  ImageService · ProfileService · AnalysisService │
             │     ReviewService · ComparisonService · Jobs     │
             └─────────────────────────┬────────────────────────┘
                                       ▼
             ┌──────────────────────────────────────────────────┐
             │                   MORFO CORE                     │
             │ • Motores Perceptivos: roots-exp003, discovery   │
             │ • Agrupamentos emergentes e novidades (PRE-001)  │
             │ • Ciclo Reflexivo REA / RIT                      │
             │ • Fila assíncrona de processamento persistente   │
             │ • Armazenamento imutável de blobs por SHA-256    │
             │ • Banco relacional de proveniência (SQLite3 WAL) │
             └──────────────────────────────────────────────────┘
```

### Invariante Fundamental: O Triângulo Epistêmico

O MORFO estabelece uma barreira rigorosa entre **percepção**, **interpretação** e **julgamento**:

| Processo | Ator Responsável | Papel Epistêmico |
| :--- | :--- | :--- |
| **Percepção** | **MORFO Core (C++26)** | Executa algoritmos estritos de visão, extrai métricas, detecta anomalias (`ANOMALY_CANDIDATE`) e descobre agrupamentos determinísticos. **Não depende de IAs externas**. |
| **Interpretação** | **Assistente via MCP** | Formula hipóteses sobre os dados, cruza evidências e aponta discrepâncias entre métodos. **Suas inferências são hipóteses, nunca fatos gravados**. |
| **Julgamento** | **Pesquisador Humano (REA/RIT)** | Toma decisões explícitas de validação, união ou cisão de morfotipos, alimentando a memória adaptativa sob integridade temporal e auditoria. |

---

## 2. Camada de Serviços Compartilhados em C++26

Toda a lógica de domínio reside em serviços C++26 independentes do protocolo de transporte (localizados conceitualmente em `src/services/`):

- **`ImageService`**: Registro, ingestão, verificação de hash SHA-256 e recuperação de metadados de imagens originais.
- **`ProfileService`**: Catálogo e metadados de perfis temáticos versionados (`roots-exp003`, `discovery/PRE-001`, futuros).
- **`AnalysisService`**: Despacho de requisições de análise, enfileiramento assíncrono e geração de artefatos derivados (máscaras, overlays, relatórios).
- **`JobService`**: Gestão do ciclo de vida das tarefas (`QUEUED`, `RUNNING`, `COMPLETED`, `FAILED`, `CANCELLED`).
- **`ReviewService`**: Registro da cadeia *append-only* de julgamentos científicos humanos (núcleo REA/RIT).
- **`ComparisonService`**: Comparação quantitativa, espacial e estatística entre execuções de diferentes métodos ou versões de perfis.

Essa unificação garante que uma análise disparada via MCP execute exatamente as mesmas funções e produza os mesmos hashes de uma análise solicitada via Web ou API REST.

---

## 3. Especificação da API REST (OpenAPI 3.2.1)

A API REST opera sobre HTTP/1.1 e HTTP/2 via Boost.Asio, versionada sob o prefixo `/api/v1`. As respostas seguem o formato JSON estruturado com metadados de paginação e rastreabilidade.

### Catálogo de Endpoints

| Método | Endpoint | Descrição | Status de Retorno |
| :--- | :--- | :--- | :--- |
| `POST` | `/api/v1/images` | Registra nova imagem original no acervo imutável | `201 Created` |
| `GET` | `/api/v1/images` | Lista imagens com paginação e filtros | `200 OK` |
| `GET` | `/api/v1/images/{id}` | Recupera metadados, hash SHA-256 e dimensões | `200 OK` |
| `GET` | `/api/v1/profiles` | Lista perfis perceptivos cadastrados e seus estados | `200 OK` |
| `POST` | `/api/v1/analyses` | Dispara processamento de imagem por um perfil (assíncrono) | `202 Accepted` |
| `GET` | `/api/v1/jobs/{id}` | Consulta status, progresso e logs do processamento | `200 OK` |
| `GET` | `/api/v1/analyses/{id}` | Consulta relatório analítico final e métricas | `200 OK` |
| `GET` | `/api/v1/analyses/{id}/mask` | Recupera binário da máscara derivada (PNG) | `200 OK` |
| `GET` | `/api/v1/analyses/{id}/overlay` | Recupera binário do overlay de inspeção (PNG/JPEG) | `200 OK` |
| `POST` | `/api/v1/reviews` | Submete decisão humana sobre morfotipos ou anomalias (REA) | `201 Created` |
| `POST` | `/api/v1/comparisons` | Submete comparação entre duas análises da mesma imagem | `200 OK` |

### Exemplo de Resposta de Despacho (`POST /api/v1/analyses` -> `202 Accepted`):
```json
{
  "job_id": "job_01J9X8K4B2M1QW8Z",
  "status": "QUEUED",
  "image_id": "img_a3f81e9b204c",
  "profile_id": "roots-exp003",
  "parameters": {
    "scale_factor": 1.0,
    "min_component_area": 15
  },
  "created_at": "2026-10-22T14:30:00Z",
  "poll_url": "/api/v1/jobs/job_01J9X8K4B2M1QW8Z"
}
```

---

## 4. Especificação do Servidor MCP (Model Context Protocol 2026)

O servidor MCP do MORFO expõe as capacidades investigativas do sistema para agentes de IA clientes (ChatGPT, Claude, Antigravity, etc.) sem expor caminhos internos de sistema de arquivos ou transferir desnecessariamente imagens pesadas.

### 4.1 Transporte
- **Streamable HTTP:** Endpoint `/mcp` no servidor C++26, mantendo compatibilidade com clientes MCP modernos (especificação julho de 2026 com conexões sem estado e transporte contínuo).
- **Transporte stdio:** Binário auxiliar `morfo-mcp-bridge` ou flag `--mcp-stdio` para execução local integrada.

Chamadas `POST` exigem sessão MORFO autenticada. Toda ferramenta que consulta
dados aplica isolamento pelo `user_id`; identidade ausente é recusada por padrão
tanto no transporte HTTP quanto no dispatcher.

### 4.2 Ferramentas MCP implementadas (`Tools`)

| Ferramenta | Descrição | Parâmetros Principais | Retorno |
| :--- | :--- | :--- | :--- |
| `morfo_list_profiles` | Consulta catálogo e semântica dos perfis | — | Perfis, versões e estado epistêmico |
| `morfo_list_images` | Consulta imagens pertencentes ao usuário autenticado | — | Até 20 imagens e hashes |
| `morfo_get_analysis` | Recupera execução EXP-003 pertencente ao usuário | `analysis_id` | Contagens e estado ontológico |

Comparação de métodos, execução remota, inspeção regional e PRE-001 via MCP
permanecem planejados. Não são anunciados por `tools/list` enquanto não houver
implementação, autorização por proprietário e testes end-to-end.

### 4.3 Recursos MCP (`Resources`)

Recursos atualmente disponibilizados pelo esquema `morfo://`:

- `morfo://profiles/catalog`: Catálogo completo de perfis perceptivos instalados e seus limites operacionais.
- `morfo://system/status`: Versão e regra operacional do serviço.

### 4.4 Prompts MCP (`Prompts`)

O MORFO fornece modelos guiados de investigação epistêmica:

- `morfo_audit_divergence`: Conduz a análise comparativa entre `roots-exp003` e o método genérico `discovery`, solicitando ao assistente que destaque os pixels divergentes e avalie se correspondem a artefatos de iluminação ou matéria orgânica.

`morfo_explore_morphotypes` permanece planejado e não é anunciado pelo servidor.

---

## 5. Roteiro Incremental de Implementação (Roadmap)

### Fase 1: Contratos, Serviços de Domínio e API REST
- Modularizar `src/` isolando os contratos dos serviços (`morfo::service`).
- Implementar despacho HTTP REST `/api/v1/images`, `/api/v1/profiles` e `/api/v1/analyses`.
- Elaborar especificação OpenAPI formal `docs/openapi.yaml`.

### Fase 2: Fila Assíncrona e Reprodutibilidade de Artefatos
- Criar gerenciador de fila de tarefas no SQLite3 com suporte a execução assíncrona em pool de threads.
- Implementar endpoint `/api/v1/jobs/{id}` com acompanhamento de progresso.
- Garantir persistência imutável dos artefatos em disco com chaveamento por hash SHA-256.

### Fase 3: Servidor MCP Nativo
- Implementar endpoint `/mcp` com suporte a JSON-RPC 2.0 e transporte Streamable HTTP.
- Expor catálogo de ferramentas de inspeção (`morfo_list_images`, `morfo_get_results`, `morfo_inspect_regions`).
- Configurar recursos (`morfo://`) para consumo leve por assistentes de IA.

### Fase 4: Integração Epistêmica (REA / RIT)
- Habilitar ferramentas e endpoints de comparação (`morfo_compare_methods` / `/api/v1/comparisons`).
- Implementar fluxo de submissão de revisões humanas (`/api/v1/reviews`), materializando a cadeia append-only sob integridade temporal.
- Conectar mecanismos de autorização e trilha de auditoria para operações de reclassificação.
