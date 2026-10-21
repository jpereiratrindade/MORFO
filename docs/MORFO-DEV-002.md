# MORFO-DEV-002 — Fundação de Validação Científica

Status: **fundação executável**  
Princípio: **toda classificação é hipótese computacional**.

## Contrato científico

Uma mudança algorítmica só pode ser chamada de avanço quando comparada por protocolo congelado, unidade experimental explícita, entradas identificadas por SHA-256, ambiente e parâmetros registrados, estimativa de incerteza e análise de perdas. Resultados visualmente melhores não bastam.

O MORFO-AVALIA é independente dos algoritmos avaliados. Seu núcleo recebe predições e referências; não chama nem favorece EXP-003. Evidência persistida é append-only.

## Modalidades

- `NONE`: descoberta sem referência. Permite Silhouette e estabilidade; proíbe linguagem de acurácia.
- `SPECIALIST`: referência anotada sem acesso às predições. Permite precisão, revocação, F1, Dice, IoU e ARI.
- `EXPERIMENTAL`: referência física ou fenotípica. Permite MAE, RMSE, viés e concordância futura.

## Protocolo inicial — EXP-003

Hipótese: EXP-003 segmenta regiões radiculares melhor que linha de base convencional, sem degradar robustez entre sessões.

- Unidade experimental: planta; sessão de aquisição quando múltiplas plantas não puderem ser separadas.
- Métodos: limiar convencional congelado, EXP-003 preservado, novo núcleo perceptivo.
- Desenvolvimento e teste: separados por `group_key`; banco rejeita vazamento entre divisões.
- Referência: máscaras independentes, revisadas por especialistas; divergências e adjudicação registradas como metadados.
- Primária: Dice por unidade. Secundárias: IoU, precisão, revocação e F1.
- Incerteza: bootstrap percentil de 95%, reamostrando unidades experimentais, nunca recortes.
- Divulgação: média + intervalo + contagens TP/TN/FP/FN + falhas e limitações.

Critérios numéricos ficam deliberadamente fora desta fundação até conjunto de referência representativo ser congelado. Fixá-los antes disso criaria falsa autoridade.

## Implementado

- C++26: Dice, IoU, precisão, revocação, F1, acurácia, ARI, Silhouette, MAE, RMSE, viés e bootstrap determinístico.
- SQLite migração 6: protocolos, conjuntos, unidades, execuções, observações e resumos.
- Travas: evidência imutável; referência obrigatória para métricas supervisionadas; grupo experimental não cruza desenvolvimento/teste.
- Testes unitários e de invariantes do banco.
- Fluxo autenticado para criar/versionar protocolos e vincular execuções EXP-003.
- Conjuntos em rascunho com edição de grupo/split, remoção de unidades e manifesto SHA-256 recalculado.
- Ingestão de máscaras PNG independentes, com autoria, data, hash e validação dimensional.
- Congelamento de conjunto e protocolo, tornando unidades e referências imutáveis.

## Próximo incremento

Executor comparativo dos métodos congelados, métricas por unidade, bootstrap, relatório HTML/CSV e bancadas funcionais Comparação–Auditoria. Painel só exibirá dados reais; nenhuma métrica demonstrativa será semeada.
