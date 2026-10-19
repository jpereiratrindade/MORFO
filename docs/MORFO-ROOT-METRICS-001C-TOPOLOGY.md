# MORFO-ROOT-METRICS-001C — Topologia aparente

## Topologia do Gate 001C

M08 e M09 usam o mesmo grafo do comprimento, após remoção de diagonais
redundantes. Vértice de grau 1 é ponta aparente; grau 0 é pixel isolado; grau
igual ou superior a 3 participa de região de ramificação. Vértices de junção
adjacentes são agrupados e arestas que saem da região contam braços incidentes.

Pontas de componentes que tocam imagem, ROI ou área abstida são registradas
também como truncadas. Resultados agregados e por componente seguem para banco,
JSON e frontend. O vetor `branch_incident_arms` conserva a contagem de braços de
cada região na ordem determinística de varredura.

Testes sintéticos cobrem estrutura em T com três braços, pixel isolado e anel
sem pontas.

## EDT e diâmetros

M05 usa transformada de distância euclidiana separável e determinística em
tempo linear no número de pixels. A EDT opera sobre a máscara completa, antes
do recorte da ROI, usando escalas independentes X/Y. Em cada aresta, o diâmetro
é a média de `2 × EDT` nos dois vértices, com convenção centro a centro.

M06 pondera diâmetro pelo comprimento da aresta e registra média, mínimo,
máximo e fração de comprimento mensurável. Componentes que tocam imagem, ROI
ou abstenção são censurados conservadoramente.

M07 usa limites positivos, finitos e ordenados. Intervalos são fechados à
esquerda e abertos à direita. Com calibração e sem configuração explícita,
limites padrão são 0,5, 1,0 e 2,0 mm. Sem calibração, diâmetros continuam em
pixels e classes só são criadas quando limites explícitos são informados.

Ensaios adicionais cobrem faixa sintética de espessura conhecida, escala
isotrópica de 0,5 mm/pixel, atribuição no limite de 2 mm e censura de objeto
tocando a imagem. O erro de discretização permanece explicitamente associado
à convenção centro a centro.
