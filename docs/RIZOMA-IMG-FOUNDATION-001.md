# RIZOMA-IMG-FOUNDATION-001 — Constituição operacional da infraestrutura de imagens

**Versão:** 0.2.0  
**Data:** 14/10/2026  
**Status:** base executável de experimentação local + programa incremental de implementação  
**Fundamento:** *Sempre pronto. Sempre incompleto.*

## 1. Reorientação do objeto

**Objeto primário:** imagem digital adquirida por um equipamento, sistema ou operador. O sistema administra o ciclo de vida do arquivo de imagem e os metadados associados. O experimento é contexto, não requisito para o nascimento de um registro. O método analítico é uma operação versionada, jamais atributo intrínseco ou condição de validade do arquivo original.

*Receber → validar tecnicamente → identificar → preservar → preparar → inspecionar → analisar (no futuro) → publicar resultados validados (no futuro).*

### Entidades conceituais permanentes

- `ImageAsset`: original, bytes, formato, digest, identificador próprio, proveniência conhecida e proveniência ainda ausente;
- `ImageDerivative`: saída de transformação com vínculo e parâmetros explícitos;
- `ProcessingRun`: execução técnica rastreável, com software, entrada, saída, estado e erros;
- `CaptureContext`: metadados opcionais de campanha, sensor, orientação, exposição, profundidade, calibração etc.;
- `ImageCollection`: conjunto lógico de imagens agrupadas pelo pesquisador, sem duplicar binários;
- `ImageReview`: decisão explícita de uma pessoa ou validador sobre o uso pretendido;
- `AnalysisResult`: entidade futura; não antecipar campos de métricas sem protocolo.

Nesta versão persistimos um subconjunto no banco (`images`, `events`). As entidades de futuro são contratos de evolução, não funcionalidades já implementadas.

## 2. Invariantes

1. O original jamais pode ser alterado por operações de pré-processamento.
2. Cada derivado conhece o original e a versão do procedimento que o criou.
3. Arquivo sem nome original, sensor, calibração ou contexto experimental continua registrável; o desconhecido permanece explícito.
4. Qualidade técnica, qualidade científica, resultado observado e inferência não se confundem.
5. Reprocessamento cria nova versão; não substitui silenciosamente evidências anteriores.
6. Referências a arquivos não derivam de caminhos fornecidos por clientes.
7. Métricas físicas não podem nascer sem calibração conhecida e protocolo validado.
8. Interface e backend não dependem operacionalmente do TRAMA, TRAMA-RS ou CRIVO.
9. Um estado `NOT_ANALYZED` não é um erro nem um valor zero.
10. A estratégia de serviço online exige segurança verificável antes de publicação.

## 3. Delimitação do primeiro corte

**Pronto agora:** upload local PNG/JPEG, validação básica por decodificação, checksum, armazenamento endereçado pelo hash, derivados PNG de inspeção, banco SQLite WAL, manifesto CSV, navegação web, inspeção de imagem e testes HTTP.

**Intencionalmente incompleto:** upload em partes/streaming para grande escala, TIFF/RAW/proprietário, EXIF/ICC de forma integral, pipelines de processamento assíncronos e versionados, autenticação multiusuário, coletâneas, calibração, segmentação e métricas radiculares, escalabilidade e publicação.

## 4. Modelo de estados recomendado

```text
RECEIVED
   ├─► REJECTED_TECHNICAL (não registrar como imagem aceita)
   └─► PRESERVED
           └─► PREPARATION_PENDING
                    ├─► PREPARED
                    └─► PREPARATION_FAILED
                              └─► RETRY_PENDING

Análise (futuro): NOT_ANALYZED → QUEUED → RUNNING → REVIEW_REQUIRED
                           → VALIDATED | REJECTED | FAILED
```

A versão 0.2.0 executa o caminho de sucesso síncrono. Ainda não possui fila persistente nem a máquina de estados completa; ela deve ser um próximo incremento prioritário.

## 5. Arquitetura-alvo separada por responsabilidades

```text
Browser profissional (acervo, upload, revisão)
       │ HTTPS (em deployment autenticado futuro)
       ▼
C++26 / API de arquivos e metadados
  ├─ policy/autenticação/autorização (futuro)
  ├─ streaming + staging + limites de ingestão (futuro)
  ├─ hash e controle de integridade
  ├─ inventário SQLite / migrações
  ├─ originais em armazenamento privado
  ├─ fila de processamento persistente (futuro)
  └─ API de consulta e entrega de derivados
       │
       ▼
Worker C++26 / adaptadores de imagem
  ├─ decodificação segura (suporte técnico condicionado ao formato)
  ├─ orientação/perfil de cor/calibração (futuro)
  ├─ pré-processamentos parametrizados (futuro)
  └─ geração de derivados versionados

CRIVO: verificação independente e evidências, nunca a origem de resultados científicos.
```

## 6. Gates recomendados

### IMG-G0 — Integridade da recepção (implementado parcialmente)

- Decodificação real PNG/JPEG; limites de bytes/pixels; SHA-256; proibição de caminhos fornecidos pelo cliente; deduplicação; teste de persistência após reinício.
- Pendente: stress test, concorrência, recuperação de orfandade, validação EXIF/profundidade, fuzzing com corpus de arquivos corrompidos.

### IMG-G1 — Processamento seguro e reprodutível (próximo)

- Ingestão em fluxo, fila persistente, estado de trabalho, logs e identificação completa de cada executável, parâmetros e dependências.
- Parar/rerodar processamento sem perder o original; geração atômica de artefatos; falhas isoladas.
- Derivados com metadados completos: hash de entrada e saída, transformações, formato, cor, orientação, algoritmos e parâmetros.

### IMG-G2 — Operação multiusuário (antes da rede)

- Login, ACL por coleção/projeto, HTTPS/reverse proxy, limites por usuário, logs de auditoria com retenção adequada, quotas, políticas de dados.
- Upload retomável, operações em lote, busca e paginação.
- Backup consistente SQLite+blobs com teste de restauração e verificação de checksums.

### IMG-G3 — Protocolos de análise radicular (depois da caracterização do equipamento)

- Exemplos representativos e dados anotados; definição explícita de modalidade e escala.
- Métodos de segmentação com máscaras versionadas, revisão humana e métricas validadas.
- Comparação com ferramenta de referência e critérios de erro por tipo de imagem.
- Recorte experimental e temporal tratados como contexto e não como substitutos da imagem.

## 7. Contrato com a equipe

*Desenvolver o RIZOMA em C++26 e uma interface web da família visual SisTer/TRAMA. Implementar antes de qualquer IA ou métrica radicular um sistema verificável de recebimento, preservação, indexação, inspeção e pré-processamento versionado de imagens. O arquivo original é soberano. Nunca prometer resultados de análise que o sistema ainda não entrega. Cada expansão deve preservar compatibilidade ou oferecer migração explícita, evidência de testes e relatório de limitações.*

**Próxima implementação sugerida:** IMG-G1 (fila persistente, upload streaming, registro de execuções) antes de integrar modelos de segmentação.
