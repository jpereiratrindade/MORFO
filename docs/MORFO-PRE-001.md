# MORFO-PRE-001 — Primeiro Perceptor Genérico Experimental

Status: **executável experimental**  
Versão: `pre001-0.1.0-experimental`

## Propósito

Receber imagem sem tema ou classes antecipadas e produzir agrupamentos visuais provisórios. Saídas usam identificadores neutros `MORFOTIPO-NNN`; nunca nomes biológicos.

## Primeiro contrato executável

1. Dividir imagem nativa em células, sem redimensionamento silencioso.
2. Extrair oito descritores genéricos: médias RGB, luminância, dispersão luminosa, contraste local e posição normalizada XY.
3. Agrupar descritores por k-means determinístico com inicialização por maior distância.
4. Calcular distância ao protótipo e escore de novidade dentro do agrupamento.
5. Preservar versão, configuração, protótipos e evidências por célula.
6. Persistir cada execução, células e relatório como registros imutáveis.
7. Produzir mapa PNG alinhado à imagem; cores distinguem morfotipos e maior opacidade sinaliza novidade elevada.

Posição XY ajuda coerência espacial nesta primeira versão, mas pode dominar agrupamentos; resultado é hipótese perceptiva, não categoria natural confirmada.

## Critérios de aceite deste incremento

- mesma imagem e configuração produzem mesmos agrupamentos;
- imagem sintética com duas aparências separa dois morfotipos;
- núcleo não contém vocabulário temático;
- `discovery` deixa de ser opção fictícia e passa a declarar versão experimental executável.
- histórico autenticado recupera execuções e artefatos sem recalcular resultados.

## Próximos incrementos

Permitir aceitar/dividir/unir/rejeitar morfotipos sem sobrescrever versões, detectar novidade entre imagens e avaliar o mesmo núcleo em domínios distintos.
