# MORFO-CONSTITUTION-001 — Constituição inicial

**Versão:** 0.2.0 · **Data:** 2026-10-22 · **Estado:** CONSTITUTION_CANDIDATE · **Regra:** Sempre pronto. Sempre incompleto.

## Identidade e objeto
MORFO é uma **plataforma autônoma de percepção visual e execução de perfis temáticos versionados**. Sua unidade primária é o arquivo original de imagem, nunca o experimento, a hipótese biológica ou uma classe predefinida.

Uma imagem pode ser analisada por múltiplos perfis sem mudar sua identidade. Um perfil declara sua modalidade, algoritmo, parâmetros, estado de validação, resultados, limitações e procedência.

## Classes e epistemologia
Uma classe descoberta por processamento estatístico não recebe identidade semântica por mera semelhança visual. O perfil `roots-exp003` produz `ANOMALY_CANDIDATE` e `root_identity=UNKNOWN`; nunca `ROOT_CONFIRMED` automaticamente. Estruturas da matriz, ruídos de aquisição, artefatos, diferenças de iluminação e raízes podem todos gerar desvios.

Os estados distinguem `OBSERVED`, `DERIVED`, `INFERRED`, `REVIEWED`, `VALIDATED`, `UNKNOWN`, `NOT_IMPLEMENTED` e `FAILED`. Sem máscara de referência e escala calibrada, não gerar comprimento/densidade radicular como medida científica.

## Invariantes
1. Original imutável; nenhum processamento reescreve seus bytes.
2. Reexecuções criam novo identificador, parâmetros, hash, estado e saídas.
3. Nenhum perfil pode tomar nomes de classes de outro perfil sem contrato explícito.
4. Ausência de evidência deve ser representável; valores desconhecidos não viram zero.
5. Arquivos e resultados pertencem ao usuário e não são publicados sem autorização.
6. Interface e algoritmo não podem declarar validação que não ocorreu.
7. Erro de processamento não deve apagar o original nem históricos anteriores.
8. Experimento de aprendizagem não deve modificar automaticamente a implementação de referência.
9. Evidências de engenharia (CTest) não equivalem a evidências científicas (máscara anotada independente).
10. O projeto RIZOMA original não pode ser alterado por processos de criação do MORFO.
11. Nenhuma inferência ou resposta de assistente externo (MCP) pode reclassificar artefatos ou alterar estados ontológicos sem julgamento humano explícito, assinado e rastreável.
12. Todas as interfaces de acesso (Web, API REST e MCP) operam sobre os mesmos contratos e serviços do núcleo C++26, garantindo reproducibilidade idêntica e ausência de regras epistêmicas paralelas.

## Interfaces constitutivas e fronteiras epistêmicas
O MORFO projeta suas capacidades por meio de três interfaces integradas a um mesmo núcleo de domínio C++26:
- **Interface Web:** visualização, inspeção interativa, acervo e anotação direta pelo pesquisador.
- **API REST (OpenAPI):** integração programática para aplicações externas, sensores, pipelines analíticos e equipamentos.
- **Servidor MCP (Model Context Protocol):** exposição de ferramentas e recursos estruturados para investigação assistida por inteligência artificial.

A separação epistêmica entre os atores é estrita e mandatória:
- **Percepção (MORFO Core):** autoridade exclusiva sobre a extração de dados visuais, agrupamentos estatísticos e detecção de desvios. Não depende de IAs externas para perceber nem descobrir.
- **Interpretação (MCP / Assistentes):** exploração heurística e formulação de hipóteses sobre os resultados produzidos pelo MORFO. Não produz fatos científicos nem altera dados.
- **Validação e Julgamento (Pesquisador / REA / RIT):** decisão humana responsável que chancela, divide ou unifica morfotipos, alimentando o ciclo de aprendizagem sob integridade temporal.

## REA e RIT
REA — Reflexividade Epistêmica e Adaptativa: `OBSERVE → INTERPRET → COMPARE → REVISE_OR_PRESERVE`. Primeira manifestação executável: aceitar, rejeitar, dividir ou unir morfotipos do PRE-001 mediante decisão humana justificada.

RIT — Reflexividade sob Integridade Temporal: todo ajuste mantém identidade do modelo, versão, dados, evento motivador, diferença de desempenho e possibilidade de auditoria/reversão. **A cadeia append-only de revisões PRE-001 implementa o primeiro núcleo RIT. Execução e mapa originais permanecem intactos; mapas materializados das revisões ainda são pendentes.**

## Organização dos perfis
O registro distingue perfis `EXPERIMENTAL`, `VALIDATED` e `PLANNED`. `roots-exp003` executa análise temática; `discovery` executa PRE-001 genérico experimental. Nenhum deles é biologicamente validado. Perfis adicionais deverão criar implementações e testes, e não só opções de interface.

## Origem e autoria
Infraestrutura/UI herdadas de RIZOMA, mantendo avisos originais de licença GPLv3. Núcleo EXP-003 original preservado sem modificação em `profiles/roots/exp003/{src,include}`. O MORFO é projeto derivado/independente, não uma migração destrutiva.
