# RIZOMA-IMG-EXP-003 — Concordância, divergência e evidências

**Status:** laboratório experimental independente em C++26; **não validado cientificamente**.

**Fundamento:** Sempre pronto. Sempre incompleto.

## Motivação

Dois procedimentos independentes (heurística de brilho/top-hat e detector estatístico de matriz por mediana+MAD local) produzem mapas binários de anomalias. O objetivo deste incremento é comparar os mapas em **coordenadas idênticas**, sem inferir a identidade biológica dos elementos.

## Código

A implementação `src/comparison.cpp` faz classificação pixel a pixel usando exatamente quatro categorias:

- `0` = nenhum algoritmo marcou (não equivale a ausência de raiz);
- `85` = marcado apenas pela heurística anterior;
- `170` = marcado apenas pelo detector adaptativo;
- `255` = marcado por ambos.

O PNG `09_classes_comparacao.png` contém esses níveis (a visualização colorida é `08_mapa_concordancia_rgb.png`).

As regiões no CSV são componentes conexas com vizinhança de 8 pixels **na união das duas máscaras**, ordenadas por número de pixels divergentes, não pela probabilidade de serem raízes. O campo `touches_border` sinaliza regiões que tocam a faixa configurada para revisão. A faixa de exclusão do detector adaptativo é uma regra fixa, não evidência de aprendizagem.

## Referência experimental única

Imagem TIFF de 850 × 757 pixels, SHA-256 `a93ee88702fd32d075ef337288fda61539eedb21d97cacb5921ce9897b973956`.

Resultado da execução de referência:

| Categoria | Pixels |
|---|---:|
| Nenhum dos detectores | 637167 |
| Ambos | 1884 |
| Somente heurística | 2944 |
| Somente adaptativo | 1455 |
| União | 6283 |

Jaccard entre máscaras: 0.29985676. A união se distribuiu em 123 regiões conectadas. Esses dados NÃO são medidas de raízes.

A maior discordância (região 1) representa 1.714 pixels na borda inferior. Não é legítimo concluir melhora científica pela exclusão dessa faixa, pois a regra foi prescrita no código.

## Tratamento epistêmico

- Fato observado: há uma imagem de entrada, com bytes identificados por SHA-256.
- Fato derivado: dois algoritmos produziram máscaras e métricas de comparação.
- Hipótese inferencial: alguns candidatos podem representar raízes.
- Estado biológico **UNKNOWN**: ainda sem validação por referência anotada.

O arquivo `11_evidencias_para_julgamento.json` é uma **proposta de intercâmbio**. `JEV` e `ENTE` NÃO foram invocados, vinculados ou testados neste incremento. `Noul/InsufficientEvidence` é uma proposta explícita de julgamento prudente, não saída real do JEV. Nenhum modelo foi treinado.

## Como executar

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/rizoma-img-exp --input "$HOME/Imagens/Raizes/Monolito/monolito_12mar20241.tiff" --output ./results-003
```

## Critérios de aceite da etapa

1. Reproduzir a soma de todos os pixels nas quatro classes.
2. Comprovar que a interseção corresponde exatamente aos pixels marcados por ambos.
3. Não representar como `ROOT` nenhuma classe do comparador.
4. Verificar visualmente pelo menos algumas regiões discordantes em imagens reais e coletar anotações independentes antes de avaliar sensibilidade/especificidade.
5. Preservar SHA-256 da fonte, parâmetros, versão de algoritmo, mecanismos de abstenção e dados de auditoria.

## Limitações

- Uma única imagem não permite estabelecer taxa de acerto, precisão, sensibilidade ou generalização.
- O método de matriz não é aprendizagem incremental persistente e não identifica raízes biologicamente.
- Os mapas podem selecionar artefatos, omitir raízes e fragmentar estruturas contínuas.
- Não há calibração, orientação instrumental, comprimento radicular nem métricas físicas.
- Não há JEV ou ENTE integrados. A interface contratual permanece apenas preparada conceitualmente.

Próxima etapa: **EXP-004 — anotação humana verificável + comparação com referência + avaliação de política JEV/ENTE sem promover candidatos não validados a raízes.**
