# MORFO-CONSTITUTION-001 — Constituição inicial

**Versão:** 0.1.0 · **Data:** 2026-10-19 · **Estado:** CONSTITUTION_CANDIDATE · **Regra:** Sempre pronto. Sempre incompleto.

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

## REA e RIT
REA — Reflexividade Epistêmica e Adaptativa: `OBSERVE → INTERPRET → COMPARE → REVISE_OR_PRESERVE`. Manifestação futura: propor abertura, divisão, fusão, revisão ou suspensão de categorias após evidência.

RIT — Reflexividade sob Integridade Temporal: todo ajuste mantém identidade do modelo, versão, dados, evento motivador, diferença de desempenho e possibilidade de auditoria/reversão. **REA/RIT continuam contratos conceituais. PRE-001 executa percepção genérica estática; ainda não adapta nem revisa categorias.**

## Organização dos perfis
O registro distingue perfis `EXPERIMENTAL`, `VALIDATED` e `PLANNED`. `roots-exp003` executa análise temática; `discovery` executa PRE-001 genérico experimental. Nenhum deles é biologicamente validado. Perfis adicionais deverão criar implementações e testes, e não só opções de interface.

## Origem e autoria
Infraestrutura/UI herdadas de RIZOMA, mantendo avisos originais de licença GPLv3. Núcleo EXP-003 original preservado sem modificação em `profiles/roots/exp003/{src,include}`. O MORFO é projeto derivado/independente, não uma migração destrutiva.
