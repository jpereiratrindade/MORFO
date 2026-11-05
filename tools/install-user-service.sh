#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
template="$repo_root/deploy/systemd/morfo-user.service.in"
user_unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
unit="$user_unit_dir/morfo.service"

if [[ ! -x "$repo_root/build/morfo-server" ]]; then
  echo "[MORFO ERRO] Binário ausente: $repo_root/build/morfo-server" >&2
  echo "Compile antes de instalar o serviço." >&2
  exit 1
fi

mkdir -p "$user_unit_dir" "$repo_root/var"
escaped_root="${repo_root//\/\\}"
escaped_root="${escaped_root//&/\\&}"
escaped_root="${escaped_root//|/\\|}"

toolbox_container="${MORFO_TOOLBOX_CONTAINER:-}"
if [[ -z "$toolbox_container" && -f "$repo_root/.env" ]]; then
  toolbox_container="$(sed -nE 's/^[[:space:]]*MORFO_TOOLBOX_CONTAINER[[:space:]]*=[[:space:]]*"?([^"#[:space:]]+)"?.*$/\1/p' "$repo_root/.env" | tail -n1)"
fi

if [[ -n "$toolbox_container" ]]; then
  if [[ ! "$toolbox_container" =~ ^[a-zA-Z0-9_.-]+$ ]]; then
    echo "[MORFO ERRO] Nome de Toolbox inválido: '$toolbox_container'." >&2
    exit 1
  fi
  if ! command -v toolbox >/dev/null 2>&1; then
    echo "[MORFO ERRO] MORFO_TOOLBOX_CONTAINER foi definido, mas toolbox não está instalado." >&2
    exit 1
  fi
  if ! toolbox run --container "$toolbox_container" sh -c \
      "curl --version | sed -n '3p' | grep -Eq '(^| )smtps?( |$)'"; then
    echo "[MORFO ERRO] O container '$toolbox_container' não possui curl com suporte SMTP." >&2
    exit 1
  fi
  exec_start="/usr/bin/toolbox run --container $toolbox_container $repo_root/build/morfo-server"
  sandbox="# Toolbox/Podman requer acesso gravável ao armazenamento do usuário e ao /tmp do host."
  echo "[MORFO] Execução via Toolbox: $toolbox_container"
else
  exec_start="$repo_root/build/morfo-server"
  sandbox=$'PrivateTmp=true\nProtectSystem=strict\nProtectHome=read-only\nReadWritePaths='"$repo_root/var"
fi

escaped_exec_start="${exec_start//\/\\}"
escaped_exec_start="${escaped_exec_start//&/\\&}"
escaped_exec_start="${escaped_exec_start//|/\\|}"

sed -e "s|@MORFO_ROOT@|$escaped_root|g" \
    -e "s|@MORFO_EXEC_START@|$escaped_exec_start|g" \
    -e "/@MORFO_SANDBOX@/{r /dev/stdin" -e "d}" \
    "$template" > "$unit.tmp" <<< "$sandbox"
chmod 0644 "$unit.tmp"
mv "$unit.tmp" "$unit"

systemctl --user daemon-reload
systemctl --user enable morfo.service
systemctl --user restart morfo.service
echo "[MORFO] Serviço de usuário instalado e ativo: $unit"
systemctl --user --no-pager --full status morfo.service
