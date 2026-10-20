# MORFO — Percepção e Classificação Temática Adaptativa

**MORFO v0.1.0 · sempre pronto, sempre incompleto.**

Projeto independente focado em aquisição, processamento de imagens e percepção temática adaptativa (inicializado a partir do código do **RIZOMA** com extensão de perfis temáticos e governança estrita de proveniência).

Consulte os documentos constitucionais e arquiteturais em [`docs/`](docs/):
- [`docs/MORFO-CONSTITUTION-001.md`](docs/MORFO-CONSTITUTION-001.md)
- [`docs/MORFO-ARCHITECTURE-001.md`](docs/MORFO-ARCHITECTURE-001.md)
- [`docs/MORFO-DEV-ROI-001-API.md`](docs/MORFO-DEV-ROI-001-API.md)

---

## 1. Compilação e Requisitos (Fedora Silverblue → toolbox)

Instale as dependências necessárias no container de compilação ou sistema:

```bash
cd /run/media/jpereiratrindade/labeco10T/dev/cpp/MORFO
sudo dnf install -y gcc-c++ cmake boost-devel sqlite-devel openssl-devel \
                    libpng-devel libjpeg-turbo-devel libtiff-devel python3
```

Compile em modo Release e execute a suíte de testes:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

---

## 2. Configuração de Ambiente e Autenticação SMTP

O MORFO utiliza autenticação segura sem senha (*passwordless* OTP) via envio de código de 6 dígitos por e-mail (SMTP TLS), com HMAC-SHA256 para tokens de sessão, rate limiting e registro de trilha de auditoria.

### Configurar o `.env`

Copie o template fornecido e defina suas credenciais:

```bash
cp .env.example .env
chmod 600 .env
```

Edite o arquivo `.env`:

```ini
# Configuração de Rede
MORFO_HOST=0.0.0.0
MORFO_PORT=8091
MORFO_DATA_DIR=./var

# Envio de E-mail (Gmail SMTP ou corporativo)
# Para o Gmail, gere uma "Senha de App" de 16 caracteres em:
# https://myaccount.google.com/apppasswords
MORFO_SMTP_HOST=smtp.gmail.com
MORFO_SMTP_PORT=587
MORFO_SMTP_USER=seu-email@gmail.com
MORFO_SMTP_PASS=xxxx xxxx xxxx xxxx
MORFO_SMTP_FROM=seu-email@gmail.com

# Segredo criptográfico fixo para HMAC das sessões (opcional, gerado aleatoriamente se ausente)
MORFO_AUTH_SECRET=

# Modo de desenvolvimento local (se 1, exibe o código OTP no terminal/console sem disparar e-mail real)
MORFO_DEV_AUTH=0
```

> **Nota de Segurança:** O arquivo `.env` está explicitamente ignorado no `.gitignore` para prevenir commits acidentais de credenciais.

---

## 3. Como Executar o Servidor

O launcher [`run.sh`](run.sh) compila automaticamente qualquer alteração pendente e carrega as variáveis do arquivo `.env` (se presente).

### Método A: Execução Padrão (utilizando `.env`)
```bash
./run.sh
```

### Método B: Informando a senha diretamente na linha de comando
Caso prefira não persistir a senha de e-mail no arquivo `.env`:
```bash
MORFO_SMTP_PASS="sua_senha_de_app" ./run.sh
```

### Método C: Modo de Desenvolvimento Local (sem envio real de e-mail)
Para desenvolvimento, demonstrações locais ou execução offline:
```bash
MORFO_DEV_AUTH=1 ./run.sh
```
*(O código OTP gerado é impresso diretamente no terminal e logado na interface web).*

### Acesso Web
- No próprio computador: `http://127.0.0.1:8091`
- Na rede local / LAN (LABECO): `http://<IP-DO-COMPUTADOR>:8091`
- Para restringir exclusivamente ao localhost: `MORFO_HOST=127.0.0.1 ./run.sh`

---

## 4. Gerenciamento de Usuários via Linha de Comando (CLI)

O binário `morfo-server` inclui comandos administrativos diretos para gerenciar os pesquisadores e permissões antes ou após iniciar o servidor:

```bash
# Listar todos os usuários cadastrados
./build/morfo-server --list-users

# Cadastrar novo usuário (roles disponíveis: admin, pesquisador, leitor)
./build/morfo-server --add-user pesquisador@embrapa.br --name "Nome do Pesquisador" --role pesquisador

# Cadastrar um novo administrador
./build/morfo-server --add-user admin@labeco.org --name "Admin LABECO" --role admin

# Ativar ou desativar acesso de um usuário
./build/morfo-server --deactivate-user usuario@embrapa.br
./build/morfo-server --activate-user usuario@embrapa.br
```

---

## 5. Funcionalidades da Interface Web

1. **Acesso Seguro (Passwordless OTP)**:
   - Login por código numérico de uso único enviado por e-mail com tempo de expiração e limitação de tentativas (*rate limiting*).
2. **Painel de Administração e Auditoria**:
   - Gestão de usuários, alteração de perfis (`admin`, `pesquisador`, `leitor`) e histórico auditável de eventos de autenticação (IPs de origem, timestamps, status).
3. **Inspeção e Pirâmide de Imagens Gigapixel**:
   - Visualização em alta resolução por ladrilhos (tiles 256×256) com suporte a formatos PNG, JPEG e TIFF científicos.
4. **Perfis de Segmentação Temática**:
   - Perfil adaptativo **Raízes (EXP-003)** com proveniência e status `NOT_VALIDATED`.
   - Consulta programática de perfis disponíveis via `GET /api/v1/profiles`.
5. **Refinamento Regional Interativo**:
   - Definição de ROIs (Regions of Interest) e ajuste fino de limiares.
   - Histórico encadeado de refinamentos com pré-visualização e descarte não destrutivo.
   - Gerenciamento de presets de parâmetros.
   - Exportação em lote de imagens originais, máscaras derivadas e métricas consolidadas em pacotes ZIP.

---

## 6. Testes Automatizados e Regressão

A suíte de testes cobre desde primitivas criptográficas e ciclo de vida de autenticação até regressão pixel a pixel de segmentação:

```bash
ctest --test-dir build --output-on-failure
```

- `morfo.auth-smtp`: Testes unitários de hashing HMAC-SHA256, tokens de sessão, rate limiter, ativação/desativação de usuários e auditoria.
- `rizoma.http-smoke`: Testes de integração HTTP ponta a ponta, upload de arquivos, pirâmides de tiles, APIs de autenticação e presets.
- `morfo.roots.exp003.golden`: Teste de regressão determinística pixel-a-pixel contra fixtures protegidas em `tests/fixtures/private/`.

---

## 7. Limitações e Governança Científica

- O detector EXP-003 localiza candidatos e indicadores de imagem através de filtros espaciais, mediana/MAD e regras morfológicas. Ele **não** constitui um classificador biológico confirmado.
- Os resultados derivados recebem o status `NOT_VALIDATED` até homologação por especialistas.
- O limite atual de ingestão síncrona monobloco é de 12 MP; imagens maiores são processadas por mosaicos com pirâmide multiescala.
- Código legado sob licença GPLv3 preservado integralmente conforme especificações do projeto de origem.
