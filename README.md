# MORFO — Percepção e Classificação Temática Adaptativa

**MORFO v0.1.0 · sempre pronto, sempre incompleto.**

Projeto independente inicializado a partir do **código local do RIZOMA** com uma extensão de perfis temáticos; a interface original é mantida. Consulte `docs/MORFO-CONSTITUTION-001.md` e `docs/MORFO-ARCHITECTURE-001.md`.

## Compilar (Fedora Silverblue → toolbox)

```bash
cd /run/media/jpereiratrindade/labeco10T/dev/cpp/MORFO
sudo dnf install -y gcc-c++ cmake boost-devel sqlite-devel openssl-devel libpng-devel libjpeg-turbo-devel libtiff-devel python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./run.sh
```

O servidor escuta em `0.0.0.0:8091`. Neste computador, acesse `http://127.0.0.1:8091`; na rede local, use `http://IP-DO-COMPUTADOR:8091`. Para restringir ao próprio computador, execute `MORFO_HOST=127.0.0.1 ./run.sh`. Não alterar RIZOMA em produção. O perfil **Raízes · Matriz adaptativa EXP-003** cria resultados derivados com proveniência e status `NOT_VALIDATED`. Use `GET /api/v1/profiles` para identificar perfis existentes e planejados.

## Regressão do perfil raízes
O teste `morfo.roots.exp003.golden` compara todos os pixels de uma máscara preservada com a execução nova do detector original; `850×757`, `3.339` pixels e `30` componentes. Dados de fixture ficam em `tests/fixtures/private/`, fora do Git por padrão.

## Limitações
- O detector EXP-003 não é um classificador biológico confirmado. Mediana/MAD, filtros e regras geométricas localizam apenas candidatos.
- A extensão de temas **genéricos** e o PRE/REA/RIT permanecem trabalho de pesquisa futuro.
- A análise temática síncrona é limitada a 12 MP até a homologação da versão em mosaicos com contexto.
- O RIZOMA legado possui componentes próprios de análise e UI. Não confundir seus indicadores com resultados científicos validados do novo perfil.
- A cópia inicial mantém as licenças/copyright da origem (GPLv3). O código de raízes EXP-003 permanece intacto.
