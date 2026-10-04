#!/bin/bash
set -euo pipefail
SCRATCH=/mnt/data/aosp/out/pc-logs/apex-test
APEX=$SCRATCH/apex
USER_HOME=$SCRATCH/work/home
R=$SCRATCH/R
S=$SCRATCH/S
U=$SCRATCH/U
step=${1:?step name required}
shift
mode=${READONLY_LAYOUT:-0}
user_dir=${FLATPAK_USER_DIR_OVERRIDE:-/U}
system_dir=${FLATPAK_SYSTEM_DIR_OVERRIDE:-/S}
args=(
  --die-with-parent --new-session --clearenv
  --dir /apex
  --ro-bind "$APEX" /apex/com.matonos.flatpak
  --dir /apex/com.android.conscrypt
  --ro-bind /etc/ssl/certs /apex/com.android.conscrypt/cacerts
  --dir /etc
  --dir /etc/ssl
  --symlink /apex/com.android.conscrypt/cacerts /etc/ssl/certs
  --dir /var
  --symlink /tmp /var/tmp
  --ro-bind "$SCRATCH/etc/passwd" /etc/passwd
  --ro-bind "$SCRATCH/etc/group" /etc/group
  --ro-bind "$SCRATCH/etc/resolv.conf" /etc/resolv.conf
  --ro-bind "$APEX/etc/flatpak" /etc/flatpak
  --proc /proc --dev /dev --tmpfs /tmp
  --dir /home
  --bind "$USER_HOME" /home/linuxd
  --dir /run/user
  --bind "$SCRATCH/work/run" /run/user/1000
  --setenv PATH /apex/com.matonos.flatpak/bin
  --setenv HOME /home/linuxd
  --setenv USER linuxd
  --setenv LOGNAME linuxd
  --setenv XDG_CONFIG_HOME /home/linuxd/.config
  --setenv XDG_CACHE_HOME /home/linuxd/.cache
  --setenv XDG_DATA_HOME /home/linuxd/.local/share
  --setenv XDG_RUNTIME_DIR /run/user/1000
  --setenv FLATPAK_CONFIG_DIR /apex/com.matonos.flatpak/etc/flatpak
  --setenv FLATPAK_SYSTEM_DIR "$system_dir"
  --setenv FLATPAK_USER_DIR "$user_dir"
  --setenv FLATPAK_DOWNLOAD_TMPDIR /tmp
  --setenv SSL_CERT_DIR /apex/com.android.conscrypt/cacerts
  --setenv SSL_CERT_FILE /apex/com.android.conscrypt/cacerts/ca-certificates.crt
  --setenv CURL_CA_BUNDLE /apex/com.android.conscrypt/cacerts/ca-certificates.crt
  --chdir /
)
if [[ $mode == 1 ]]; then
  args+=(--ro-bind "$S" /S --ro-bind "$U" /U)
else
  args+=(--bind "$R" /R --bind "$S" /S --bind "$U" /U)
fi
printf '%q ' "$@" > "$SCRATCH/logs/$step.command"
printf '\n' >> "$SCRATCH/logs/$step.command"
set +e
strace -f -qq -s 1024 -e trace=file,network -o "$SCRATCH/traces/$step.trace" \
  /usr/bin/bwrap "${args[@]}" -- "$@" \
  >"$SCRATCH/logs/$step.out" 2>"$SCRATCH/logs/$step.err"
status=$?
set -e
printf '%s\n' "$status" > "$SCRATCH/logs/$step.status"
cat "$SCRATCH/logs/$step.out"
cat "$SCRATCH/logs/$step.err" >&2
exit "$status"
