# RIZOMA-IMG-EXP-002 — Hipótese falsificável

**Data:** 15/10/2026  
**Versão:** 0.1.0  
**Status:** prova de conceito controlada e reprodutível; sem validação radicular.

## Hipótese H1

Um estimador local robusto da matriz do solo, combinado com morfologia de estruturas e **abstenção explícita**, pode reduzir interferências triviais do fundo em comparação com uma heurística de limiares globais, **sem pesos pré-treinados**.

## Hipótese concorrente H0

As regras geométricas, máscaras de borda ou priors de brilho apenas transferem erros; qualquer aparente redução de falsos positivos não é acompanhada de melhora de recall/precisão em raízes verificadas por especialista. Portanto, não interpretar a queda no número de candidatos como melhora científica.

## Protocolos de falsificação

- **A**: imagem de solo sem raízes; algoritmo deve apresentar poucas anomalias e admitir `UNKNOWN` quando inadequado;
- **B**: linhas sintéticas claras dentro de matriz homogênea; deve registrar candidatos sem confundir fronteira externa;
- **C**: borda de scanner em posição deslocada (não apenas nos 10 px extremos); verificar se o modelo ainda a confunde com raiz;
- **D**: raízes claras reais contra solo heterogêneo: anotar referência humana e calcular precisão/recall/Dice;
- **E**: raízes escuras e raiz quase da mesma cor do solo: testar possibilidade de omissão;
- **F**: imagens consecutivas, revisar se a adaptação histórica melhora sem esquecer dados anteriores.

## Estados epistêmicos

`OBSERVED(image)` → `DERIVED(local anomaly)` → `INFERRED(root candidate)` → `UNKNOWN`/`INSUFFICIENT_EVIDENCE` enquanto não houver julgamento robusto; reinterpretação posterior com origem causal verificável.

JEV julga, não gera evidência nem executa ações. ENTE conserva trajetória epistêmica, não determina biologicamente o que é raiz. Detector distingue candidatos, não define verdade.

## Métricas deste piloto

Com a imagem `monolito_12mar20241.tiff` (850 × 757): heurística anterior 4.828 pixels/137 componentes, 1.714 pixels nas últimas 10 linhas; matriz adaptativa 3.339 pixels/30 componentes, zero nas últimas 10 linhas **por exclusão deliberada da margem**. Interseção 1.884 pixels; Jaccard das máscaras ≈0,300. Isso demonstra diferença de métodos, **não superioridade de detecção radicular**.

## Critérios de não aprovação

Sem ground truth biológico, nenhuma máscara deve ser denominada “raízes confirmadas”; nunca confundir `unknown` com classe negativa, nunca inferir densidade de raízes em unidade física sem calibração, nunca substituir originais por negativos ou prévias, nunca atribuir ao JEV ou ao ENTE capacidades de visão ainda não implementadas.

## Fonte

Comparação rastreável ao commit base do RIZOMA `ea9675e6dfa3823f51239a42726811de7eb2ad85` e ao contrato JEV localizado no repositório ELO, branch `main` consultada em 15/10/2026. Nenhum arquivo do RIZOMA de produção foi modificado.
