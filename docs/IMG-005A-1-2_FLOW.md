# IMG-005A.1/2 — Primeiro fluxo integrado de análise e revisão

## Fluxo entregue

Uma imagem pertencente ao usuário autenticado pode iniciar uma execução do baseline experimental. O original é decodificado, seu SHA-256 é copiado para `analysis_runs`, regiões candidatas são registradas em coordenadas nativas e cada região recebe uma hipótese automática independente (`ROOT` quando os dois critérios concordam; `UNKNOWN` quando há apenas um critério).

O frontend existente apresenta as regiões sobre a imagem como **“Possível raiz — não confirmada”** ou “Estrutura desconhecida — revisão necessária”. Concordância algorítmica é evidência técnica, nunca confiança científica calibrada. O pesquisador pode confirmar, corrigir ou suspender o julgamento usando `ROOT`, `MATRIX`, `ARTIFACT` ou `UNKNOWN`. Cada ação cria um novo `review_events`; nenhuma hipótese ou revisão anterior é atualizada ou apagada. `analysis_regions.current_version` é apenas o cursor concorrente para comparação e acesso ao último evento.

O inspetor permite alternar original e negativo sem trocar o sistema de coordenadas dos overlays. Um seletor de execuções permite recuperar análises anteriores, mesmo depois de nova execução ou reinício.

## Baseline experimental

`rizoma-exp-002-003-integrated-baseline` versão `0.1.0-experimental` extrai dois sinais de pixels claros e pouco cromáticos: um limiar global e um limiar estatístico por células de 32 px. Componentes conexos da união geram retângulos candidatos. A concordância sugere `ROOT`; ausência de concordância preserva `UNKNOWN`.

Este executor:

- é derivado dos conceitos demonstrados em EXP-002/003, mas não reproduz uma validação biológica;
- trabalha integralmente em memória e somente no domínio de imagens atualmente aceito pela ingestão;
- não é segmentação pixel a pixel validada;
- não produz medidas radiculares;
- não resolve ainda análise por blocos para imagens arbitrariamente grandes;
- não executa JEV nem ENTE.

## Persistência e autorização

- `analysis_runs`: entrada, algoritmo, versão, parâmetros, SHA-256, estado e autoria;
- `analysis_regions`: retângulo nativo, área candidata, evidências e versão concorrente;
- `analysis_judgments`: hipótese automática separada da decisão humana;
- `review_events`: histórico humano append-only com autor, ação, rótulo, justificativa e versão.

Toda consulta e gravação é autorizada pelo caminho `região → execução → imagem → images.user_id`. Recursos de outro usuário respondem como não encontrados. Uma revisão informa `expected_version`; valor obsoleto resulta em HTTP 409 sem alterar o histórico.

## API do incremento

```text
POST /api/v1/images/{image_id}/analysis
GET  /api/v1/images/{image_id}/analysis
GET  /api/v1/images/{image_id}/analysis-runs
POST /api/v1/analysis-regions/{region_id}/reviews
GET  /api/v1/analysis-regions/{region_id}/reviews
```

O smoke integrado comprova análise, hipótese, duas revisões sucessivas, conflito concorrente, isolamento entre contas e recuperação completa depois do reinício do servidor.
