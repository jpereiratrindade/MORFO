# MORFO-ROOT-METRICS-001C — Topologia aparente

## Entrega parcial do Gate 001C

M08 e M09 usam o mesmo grafo do comprimento, após remoção de diagonais
redundantes. Vértice de grau 1 é ponta aparente; grau 0 é pixel isolado; grau
igual ou superior a 3 participa de região de ramificação. Vértices de junção
adjacentes são agrupados e arestas que saem da região contam braços incidentes.

Pontas de componentes que tocam imagem, ROI ou área abstida são registradas
também como truncadas. Resultados agregados e por componente seguem para banco,
JSON e frontend. O vetor `branch_incident_arms` conserva a contagem de braços de
cada região na ordem determinística de varredura.

Testes sintéticos cobrem estrutura em T com três braços, pixel isolado e anel
sem pontas. EDT, diâmetros e classes M05–M07 continuam pendentes antes do Gate
001C ser declarado concluído.
