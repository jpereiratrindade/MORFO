# Governança leve do MORFO

Este documento define o mínimo necessário para que uma evolução seja rastreável sem confundir correção de software com validação científica.

## Unidade de trabalho

Uma mudança relevante começa em uma Issue com problema, hipótese, escopo, critérios de aceite e evidências. A implementação ocorre em branch própria e chega à `main` por Pull Request associado à Issue.

Mudanças triviais podem usar uma descrição curta, mas não dispensam a indicação do efeito e da verificação realizada.

## Dois estados independentes

| Engenharia | Significado |
| --- | --- |
| proposto | problema registrado, ainda sem compromisso de implementação |
| implementado | código concluído, ainda sem evidência completa |
| testado | critérios de aceite e testes executados |
| publicado | versão identificada e disponibilizada |

| Ciência | Significado |
| --- | --- |
| hipótese | afirmação ainda a verificar |
| verificado computacionalmente | comportamento reproduzido por teste ou simulação |
| validado empiricamente | confrontado com protocolo e dados independentes adequados |
| limitado/refutado | validade restrita ou hipótese rejeitada, com evidência registrada |

`testado` nunca implica automaticamente `validado empiricamente`.

## Definição de concluído

Uma entrega está concluída quando possui Issue e PR ligados; critérios de aceite satisfeitos; build e CTest aprovados; contratos e limitações atualizados; estado científico explícito; e, quando publicada, versão e notas de release identificáveis.

## Versionamento

O `project(... VERSION ...)` de `CMakeLists.txt` é a fonte da versão do código. Tags usam `vMAJOR.MINOR.PATCH`. Uma implantação registra separadamente a versão/tag ou commit executado. Protocolos e modelos científicos mantêm sua própria identidade documental e não herdam a versão do aplicativo.

## Etiquetas recomendadas

Criar no GitHub: `status:proposto`, `status:implementado`, `status:testado`, `status:publicado`, `ciencia:hipotese`, `ciencia:computacional`, `ciencia:validado`, `ciencia:limitado`, `tipo:bug`, `tipo:funcionalidade`, `tipo:metodologia` e `bloqueado`.

