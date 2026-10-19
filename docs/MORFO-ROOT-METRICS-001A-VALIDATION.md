# MORFO-ROOT-METRICS-001A — Validação matemática

## Escopo

Gate 001A implementa M01 (área projetada), M02 (ocupação) e M03 (componentes
conectados) sobre a máscara binária original do `roots-exp003`. Os resultados
declaram a natureza **morfometria bidimensional de candidatos radiculares
detectados pelo EXP-003** e estado `NOT_VALIDATED`.

## Convenções

- Qualquer valor raster diferente de zero representa verdadeiro.
- O domínio de medição é `ROI ∩ valid_mask`. Candidatos fora desse domínio não
  participam das métricas nem dos componentes.
- `excluded_or_abstained_pixels` conta pixels da ROI onde `valid_mask` é falso.
- Área em pixels é contagem de centros de pixels. Com calibração, cada pixel
  representa `scale_x × scale_y` mm²; cm² usa divisão por 100.
- Calibração física existe somente quando ambas as escalas são finitas e
  estritamente positivas. Sem ela, resultados físicos são `null` (`std::nullopt`).
- Componentes usam 8 vizinhos. IDs são determinísticos e começam em 1, segundo
  a primeira ocorrência na varredura da esquerda para direita, de cima para baixo.
- Centroide usa média das coordenadas inteiras dos centros dos pixels; origem
  `(0,0)` corresponde ao centro do pixel superior esquerdo.
- Caixa delimitadora tem limites inclusivos.
- Contato com limite da imagem, limite da ROI e área abstida é registrado em
  indicadores independentes. Vizinhança fora da imagem também encerra a ROI.
- Se não há pixel válido, ocupação é indisponível. Razões nunca dividem por zero.

## Ensaios determinísticos

`morfo.root-metrics.001a` cobre máscara vazia, retângulo 10×10, calibração
anisotrópica, conectividade diagonal, ordenação estável, recorte por ROI,
abstenção, domínio válido vazio, contato com imagem e rejeição de calibração
incompleta ou não positiva. Asserções verificam conservação:

`candidate_pixels + matrix_pixels = valid_pixels`

e, nos casos exercitados, soma das áreas dos componentes igual à área candidata.

## Limitações

As medidas descrevem geometria da máscara candidata, não identidade biológica.
O Gate 001A não calcula comprimento, esqueleto, diâmetro, topologia ou distribuição
espacial; esses itens pertencem aos gates seguintes. A integração antecipada oferece
execução HTTP autenticada, persistência SQLite e consulta no painel para tornar os
resultados de área, ocupação e componentes verificáveis pelo pesquisador.
