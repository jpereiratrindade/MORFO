# MORFO-ROOT-METRICS-001B — Validação de comprimento

## Método

M04 opera na máscara dentro de `ROI ∩ valid_mask`. O afinamento binário usa
Guo-Hall determinístico até convergência. Cada pixel restante vira vértice de
grafo com vizinhança 8. Uma aresta diagonal é descartada quando existe caminho
ortogonal de dois passos entre seus extremos por pixel intermediário do
esqueleto; cada aresta restante é contada uma vez.

Comprimento sem calibração usa geometria dos pixels: aresta ortogonal vale 1 e
diagonal vale `√2`. Com calibração anisotrópica:

`Lij = sqrt((dx × scale_x)² + (dy × scale_y)²)`

Resultados registram pixels de esqueleto, arestas, comprimento em pixels
geométricos e, quando há calibração válida, milímetros e centímetros. Medidas
por componente e agregadas são persistidas e exportadas em JSON/CSV.

## Ensaios

- Linha horizontal de 11 pixels: 10 px.
- Linha vertical de 11 pixels: 10 px.
- Linha diagonal de 11 pixels: `10 × √2` px.
- Escala anisotrópica vertical de 2 mm/px: 20 mm para 10 arestas.
- Faixa com conexões ortogonais: diagonais redundantes não inflam comprimento.
- Objeto cortado pela ROI: somente domínio interno participa e componente é
  marcado como truncado.

## Limites

Comprimento descreve esqueleto bidimensional da máscara candidata EXP-003, não
comprimento biológico confirmado. Afinamento e discretização podem alterar
topologia em objetos pequenos, ruidosos ou tocando limites; esses contatos são
preservados nos indicadores de truncamento. Diâmetro, pontas e ramificações
pertencem ao Gate 001C.
