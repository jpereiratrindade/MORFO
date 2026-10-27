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
sed "s|@MORFO_ROOT@|$escaped_root|g" "$template" > "$unit.tmp"
chmod 0644 "$unit.tmp"
mv "$unit.tmp" "$unit"

systemctl --user daemon-reload
systemctl --user enable --now morfo.service
echo "[MORFO] Serviço de usuário instalado e ativo: $unit"
systemctl --user --no-pager --full status morfo.service
