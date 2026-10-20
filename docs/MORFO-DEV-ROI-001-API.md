# API de refinamento regional v1

Todos os recursos, salvo o contrato de parâmetros, exigem sessão autenticada e
aplicam isolamento pelo proprietário da imagem.

## Parâmetros e presets

- `GET /api/v1/refinement-parameters` — valores de referência e restrições.
- `GET /api/v1/parameter-presets` — presets do usuário.
- `POST /api/v1/parameter-presets` — cria ou atualiza pelo nome.
- `DELETE /api/v1/parameter-presets/{id}` — remove preset.

Os parâmetros são: `score_threshold`, `brightness_threshold`,
`min_component_area`, `max_component_area`, `min_elongation`,
`top_hat_width`, `top_hat_height`, `top_hat_weight`,
`small_component_limit`, `fill_factor`, `median_width`, `median_height`,
`opening_width` e `opening_height`.

## Sessão e intervenções

- `POST /api/v1/images/{image}/refinements` com `base_profile_run_id` — cria
  rascunho a partir de uma máscara EXP-003 existente.
- `GET /api/v1/refinements/{id}` — estado, proveniência e revisões ordenadas.
- `POST /api/v1/refinements/{id}/preview` — devolve PNG sem persistir revisão.
- `POST /api/v1/refinements/{id}/interventions` — confirma uma intervenção no
  rascunho e cria revisão imutável.
- `POST /api/v1/refinements/{id}/undo` e `/redo` — move o ponteiro atual sem
  apagar revisões.
- `POST /api/v1/refinements/{id}/confirm` — congela o resultado final.
- `GET /api/v1/refinements/{id}/mask` e `/overlay` — artefatos consolidados.
- `GET /api/v1/refinements/{id}/before` e `/difference` — máscara anterior e
  mapa RGB da última mudança (verde adicionado, vermelho removido).
- `GET /api/v1/refinements/{id}/metadata` — metadados JSON reproduzíveis.

Corpo de intervenção retangular:

```json
{
  "intervention_type": "REPROCESS",
  "selection_type": "rectangle",
  "x": 20, "y": 30, "width": 100, "height": 80,
  "score_threshold": 130,
  "brightness_threshold": 100
}
```

Para `polygon` ou `brush`, `points` é vetor plano nas coordenadas do original:
`[x1,y1,x2,y2,...]`. Pincel também aceita `brush_diameter`. Tipos de intervenção:
`REPROCESS`, `INCLUDE` e `EXCLUDE`.

Cada revisão registra algoritmo/versão da sessão, parâmetros efetivos, seleção,
revisão anterior, ordem, data, hashes da máscara, pixels adicionados/removidos e
SHA-256 imutável do original.
