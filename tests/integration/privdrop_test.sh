#!/usr/bin/env bash
# Privilege drop: a daemon started as root must switch to run.user before it does any
# work, must not be able to regain root, and must refuse to run as root at all when no
# run.user is configured.
#
#   sudo tests/integration/privdrop_test.sh          # run.user = the account that ran sudo
#
# Uses of-pay (it needs no database) with a throwaway config, run directory and queue
# names under /tmp, so your real run/ directory and its ownership are never touched.
set -uo pipefail
cd "$(dirname "$0")/../.."

if [[ $EUID -ne 0 ]]; then
    echo "run with sudo: this test starts a daemon as root" >&2
    exit 1
fi
CONF=${CONF:-conf/orderforge.conf}
BIN=${BIN:-build/debug}
USER_NAME=${RUN_USER:-${SUDO_USER:-}}
[[ -n $USER_NAME && $USER_NAME != root ]] || { echo "set RUN_USER=<non-root account>" >&2; exit 1; }
UID_EXPECT=$(id -u "$USER_NAME")
GID_EXPECT=$(id -g "$USER_NAME")

WORK=$(mktemp -d /tmp/of-privdrop.XXXXXX)
chmod 755 "$WORK"
trap 'kill $PAY 2>/dev/null; rm -rf "$WORK"; rm -f /dev/mqueue/of-privdrop-*' EXIT
PASS=0
FAIL=0
check() {
    local desc=$1; shift
    if "$@"; then PASS=$((PASS + 1)); printf '  ok    %s\n' "$desc"
    else FAIL=$((FAIL + 1)); printf '  FAIL  %s\n' "$desc"; fi
}
is() { [[ "$1" == "$2" ]]; }

make_conf() { # make_conf FILE [run.user]
    {
        grep -E '^pay\.hmac_key' "$CONF"
        echo "run.dir = $WORK/run"
        echo "log.dir = $WORK/log"
        echo "report.dir = $WORK/reports"
        echo "pay.request_queue = /of-privdrop-req"
        echo "pay.reply_queue = /of-privdrop-rep"
        [[ -n ${2:-} ]] && echo "run.user = $2"
    } > "$1"
    chmod 600 "$1"
}

echo "privilege drop (started as root, run.user = $USER_NAME)"
make_conf "$WORK/with-user.conf" "$USER_NAME"
"$BIN/of-pay" -c "$WORK/with-user.conf" 2> "$WORK/pay.err" &
PAY=$!
sleep 1
check "of-pay started" kill -0 "$PAY"
UIDS=$(awk '/^Uid:/ {print $2, $3, $4, $5}' "/proc/$PAY/status")
GIDS=$(awk '/^Gid:/ {print $2, $3, $4, $5}' "/proc/$PAY/status")
check "real, effective, saved and filesystem uid are all $UID_EXPECT ($UIDS)" \
    is "$UIDS" "$UID_EXPECT $UID_EXPECT $UID_EXPECT $UID_EXPECT"
check "all gids are $GID_EXPECT ($GIDS)" is "$GIDS" "$GID_EXPECT $GID_EXPECT $GID_EXPECT $GID_EXPECT"
GROUPS_LINE=$(awk '/^Groups:/ {$1=""; print}' "/proc/$PAY/status" | xargs)
check "supplementary groups are the account's own, not root's ($GROUPS_LINE)" \
    test "$(echo " $GROUPS_LINE " | grep -c ' 0 ')" -eq 0
check "its pid file is owned by $USER_NAME" is "$(stat -c %U "$WORK/run/of-pay.pid")" "$USER_NAME"
check "its log is owned by $USER_NAME" is "$(stat -c %U "$WORK/log/of-pay.log")" "$USER_NAME"
check "core dumps are off (not dumpable)" is "$(stat -c %U "/proc/$PAY/mem")" "root"
kill -TERM "$PAY"
wait "$PAY" 2>/dev/null

echo "refusing to run as root without run.user"
make_conf "$WORK/no-user.conf"
"$BIN/of-pay" -c "$WORK/no-user.conf" > "$WORK/out.txt" 2>&1
RC=$?
check "of-pay exits with an error" test "$RC" -ne 0
check "and says why" grep -q "refuses to run as root" "$WORK/out.txt"

echo
echo "privilege drop: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
