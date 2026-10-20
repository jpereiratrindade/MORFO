# MORFO-DEV-ROI-001 — auditoria e base de implementação

## Estado verificado

O perfil servido como `roots-exp003` chama `morfo::profiles::run_exp003`, que lê o
original, calcula escala de cinza, top-hat e executa `rizoma::adaptive_detect`.
Parâmetros efetivos são `top_hat_radius=8`, `cell=32`, `context=64`, `border=10`,
`seed_z=2.4`, `min_tophat=7.0` e `min_component_area=4`. O resultado é persistido
como `mask.png`, `overlay.png` e `report.json`; o SHA-256 do original é conferido
antes de registrar `morfo_profile_runs`.

A tabela de valores de MORFO-ALG-RAIZ-001 v0.2.0 corresponde ao caminho separado
`legacy_mask`: limiares 140/108, área 4–2500, alongamento 1,5, top-hat elíptico
17×17 com peso 2, limite 80, preenchimento 0,45, mediana 3×3 e abertura 3×3.
Esse caminho **não é** o detector adaptativo EXP-003. A implementação agora o
expõe como `ReferenceParams`/`reference_mask`, sem alterar `run_exp003`.

## Fluxo existente

- Aquisição preserva bytes originais em `blobs`, SHA-256 e derivados separados.
- `POST /api/v1/images/{id}/profiles/roots-exp003` cria execução imutável.
- Artefatos são obtidos em `.../{run}/mask`, `overlay` e `report`.
- SQLite é o mecanismo existente. Tabelas de presets, sessões, seleções e revisões
  regionais foram acrescentadas pelo inicializador de esquema, sem segundo banco.
- Interface atual oferece zoom/pan e revisão de caixas candidatas, mas não possuía
  edição de máscara espacial.

## Núcleo regional entregue

`morfo_regional_refinement` fornece seleções retangulares, poligonais e por pincel
em coordenadas do raster original; incorporação exata
`(M_atual AND NOT R) OR (M_local AND R)`; inclusão/exclusão manual; revisões
ordenadas; undo/redo. O teste sintético verifica bit a bit que pixels externos à
seleção não mudam, sobreposição com precedência da última operação, validação de
parâmetros e navegação do histórico.

## Estado da integração

Contratos HTTP, PNG por revisão, artefatos consolidados, presets nomeados e painel
web estão integrados. A prévia não persiste resultados e usa sequência de pedido
no cliente para impedir resposta antiga de substituir a atual. Confirmação congela
o rascunho; revisões continuam disponíveis após reinício.

## Lacunas conhecidas

Ainda faltam exportação em pacote único, otimização por recorte com contexto e conjunto público de imagens
anotadas com precisão/recall/Dice/IoU. O teste golden privado cobre somente a
reprodutibilidade EXP-003; não constitui validação biológica.

## Comandos de validação

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target morfo-regional-refinement-tests morfo-roots-core-tests -j2
ctest --test-dir build -R 'morfo.(regional-refinement|roots.exp003.core)' --output-on-failure
```

No host, o alvo completo não compila porque os headers Boost não estão instalados
(`boost/asio.hpp: No such file or directory`). No Fedora toolbox `crivo-dev`, o
projeto completo compilou e os seis testes registrados passaram, incluindo smoke
HTTP, migrações, golden privado do EXP-003 e refinamento regional (5,74 s em
2026-10-20).
