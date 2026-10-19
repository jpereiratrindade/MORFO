# MORFO-TEAM-MANDATE-001 — Diretrizes inequívocas para equipe

## Missão
Criar **MORFO v0.1.0** como novo repositório C++26 ao lado de RIZOMA, reutilizando código de interface, ingestão, autenticação de piloto, banco, preservação e visualizador, sem alterar uma linha sequer em RIZOMA.

## Contrato de implantação
- Destino padrão: mesma pasta `dev/cpp` de RIZOMA, diretório novo `MORFO`.
- Instalação idempotente por recusa: **nunca sobrescrever** destino existente.
- Não copiar `var/`, chaves, cookies, bancos, imagens de usuários ou builds do RIZOMA. Fixtures fornecidas pelo usuário são locais e ignoradas pelo Git.
- Servidor MORFO: loopback `127.0.0.1:8091`; base `var/morfo.sqlite3`; cookie `morfo_session`.
- Preservar licença GPLv3 e créditos do código herdado.

## Alcance v0.1.0
1. Recuperar a interface web existente, com identidade MORFO e preservação de funcionalidades antigas.
2. Inserir um catálogo de perfis (`GET /api/v1/profiles`) distinguindo `EXPERIMENTAL` e `PLANNED`.
3. Registrar o perfil `roots-exp003`: código fonte idêntico ao EXP-003 fornecido, mediana/MAD local, top-hat, componentes e limiares originais.
4. Expor análise sob API autenticada e vinculada à imagem, com `mask.png`, `overlay.png`, `report.json` e registro no SQLite por execução.
5. Na UI, oferecer seletor de tema e visualização dos resultados de raízes do backend C++26.
6. Preservar o estado `NOT_VALIDATED` e `root_identity=UNKNOWN`; não reportar medidas agronômicas de comprimento/densidade.
7. Garantir teste pixel a pixel do EXP-003 para a fixture de referência.
8. Não alegar classificador genérico, reconhecimento semântico autônomo, REA/RIT executáveis ou percepção neural nesta primeira versão.

## Critérios de aceite
- `./create-morfo.sh --dry-run --source .../RIZOMA` não grava arquivos.
- `./create-morfo.sh --source .../RIZOMA` cria MORFO e deixa origem intacta.
- `cmake -S . -B build && cmake --build build --parallel && ctest --test-dir build --output-on-failure` conclui sem falhas no Toolbox alvo.
- EXP-003: 850×757 px, **3339 pixels positivos e 30 componentes**, comparação exata com PNG referência.
- `/api/v1/profiles` retorna `roots-exp003` e `discovery` marcado `PLANNED`.
- O perfil de raízes grava máscara/overlay/relatório, com acesso exclusivo do proprietário da imagem e histórico por execução.
- Imagens maiores que 12 MP recebem erro explícito; não reamostrar a imagem para fabricar conformidade.
- Teste de UI manual com imagem real e verificação de navegação/zoom.

## Próxima fase, somente após passar todos os gates
Implementar o motor PRE (percepção não supervisionada/autossupervisionada) sob contrato de observação, protótipos, novidade e proposta de classes. REA decide revisar ou preservar interpretações com evidência; RIT mantém identidade, histórico temporal e reprodução. Comparar PRE com EXP-003 em dataset anotado independentemente. O perfil raízes **não** é o núcleo geral.

## Formato do relatório da equipe
Informar SHA da origem, alterações em MORFO, testes executados e seus resultados, artefatos produzidos, riscos conhecidos e decisão por gate (`PASS`, `FAIL`, `BLOCKED`). Não declarar sucesso sem evidências de execução real.
