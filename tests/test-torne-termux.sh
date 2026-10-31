#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
launcher=$root/packaging/termux/torne-termux
fail_test()
{
    printf 'torne-termux test: %s\n' "$1" >&2
    exit 1
}

[ -x "$launcher" ] || fail_test "launcher is not executable: $launcher"

test_root=$(mktemp -d "${TMPDIR:-/tmp}/torne-termux-test.XXXXXX")
cleanup()
{
    if [ -n "${signal_latch-}" ]; then
        rm -f "$signal_latch" "$signal_tmp"
    fi
    find "$test_root" -depth -delete
}
trap cleanup EXIT

fake_bin=$test_root/bin
mkdir "$fake_bin"
cat > "$fake_bin/termux-usb" <<'EOF'
#!/bin/sh
set -eu
[ "$#" -eq 3 ] && [ "$1" = -e ] || exit 91
callback=$2
usb=$3
case "$usb" in
    */usb-one) exec 7<"$usb"; fd=7; name=one ;;
    *) exit 92 ;;
esac
if [ -n "${FAKE_TERMUX_PID_DIR-}" ]; then
    printf '%s\n' "$$" > "$FAKE_TERMUX_PID_DIR/$name"
fi
if [ "${FAKE_FAIL_USB-}" = "$usb" ]; then
    exit 17
fi
# An asynchronous POSIX-shell child inherits ignored INT/QUIT.  Reset the
# dispositions before exec so the callback participates in the signal matrix
# like the foreground callback process in Termux.
perl -e '$SIG{INT} = "DEFAULT"; $SIG{TERM} = "DEFAULT"; $SIG{HUP} = "DEFAULT"; exec @ARGV or die "exec: $!"' \
    -- sh "$callback" "$fd" &
callback_pid=$!
if [ -n "${FAKE_CALLBACK_PID_DIR-}" ]; then
    printf '%s\n' "$callback_pid" > "$FAKE_CALLBACK_PID_DIR/$name"
fi
forward_signal()
{
    signal=$1
    kill "-$signal" "$callback_pid" 2>/dev/null || :
    if [ "${FAKE_TERMUX_EXIT_ON_SIGNAL-0}" -eq 1 ]; then
        case "$signal" in
            INT) exit 130 ;;
            TERM) exit 143 ;;
            HUP) exit 129 ;;
        esac
    fi
}
trap 'forward_signal INT' INT
trap 'forward_signal TERM' TERM
trap 'forward_signal HUP' HUP
if [ "${FAKE_EARLY_EXIT_USB-}" = "$usb" ]; then
    ready_file=${FAKE_TORNED_READY:-$FAKE_TORNED_ENDPOINT}
    while [ ! -e "$ready_file" ]; do
        sleep 0.1
    done
    exit 0
fi
callback_status=0
wait "$callback_pid" || callback_status=$?
trap - INT TERM HUP
exit "$callback_status"
EOF
chmod 0755 "$fake_bin/termux-usb"
fast_bin=$test_root/fast-bin
mkdir "$fast_bin"
cat > "$fast_bin/sleep" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 0755 "$fast_bin/sleep"
cat > "$fake_bin/torned" <<'EOF'
#!/bin/sh
set -eu
if [ "${FAKE_TORNED_VERSION-}" = 1 ]; then
    printf 'torned 0.1.0\n'
    exit 0
fi
printf '%s\n' "$#" > "$FAKE_TORNED_LOG"
printf '%s\n' "$$" > "$FAKE_TORNED_PID"
for argument do
    printf '[%s]\n' "$argument" >> "$FAKE_TORNED_LOG"
done
previous=
for argument do
    if [ "$previous" = --fd ]; then
        [ -e "/proc/self/fd/$argument" ] || exit 93
    fi
    previous=$argument
done
if [ "${FAKE_TORNED_WAIT-0}" -eq 1 ]; then
    record_signal()
    {
        signal=$1
        if (set -C; : > "$FAKE_TORNED_SIGNAL_LATCH") 2>/dev/null; then
            printf '%s\n' "$signal" > "$FAKE_TORNED_SIGNAL_TMP"
            mv "$FAKE_TORNED_SIGNAL_TMP" "$FAKE_TORNED_SIGNAL"
        fi
    }
    stop()
    {
        signal=$1
        # The process-group signal can arrive again while the first trap is
        # publishing its latched result.  Do not re-enter the critical section.
        trap '' INT TERM HUP
        record_signal "$signal"
        if [ "${FAKE_TORNED_STOP_DELAY-0}" != 0 ]; then
            sleep "$FAKE_TORNED_STOP_DELAY"
        fi
        rm -f "$FAKE_TORNED_ENDPOINT"
        exit $((128 + signal))
    }
    if [ "${FAKE_TORNED_IGNORE_SIGNALS-0}" -eq 1 ]; then
        trap '' INT TERM HUP
    else
        trap 'stop 2' INT
        trap 'stop 15' TERM
        trap 'stop 1' HUP
    fi
    # Publish readiness only after all signal traps are installed.
    : > "$FAKE_TORNED_ENDPOINT"
    if [ -n "${FAKE_TORNED_READY-}" ]; then
        : > "$FAKE_TORNED_READY"
    fi
    while :; do
        sleep 1
    done
fi
exit "${FAKE_TORNED_EXIT:-0}"
EOF
chmod 0755 "$fake_bin/torned"

usb_one=$test_root/usb-one
runtime=$test_root/runtime\ with\ spaces
: > "$usb_one"
log=$test_root/torned.log
pid_file=$test_root/torned.pid
termux_pid_dir=$test_root/termux-pids
callback_pid_dir=$test_root/callback-pids
endpoint=$test_root/fake-endpoint
signal_file=$test_root/torned-signal
signal_latch=$test_root/torned-signal.latch
signal_tmp=$test_root/torned-signal.tmp
ready_file=$test_root/fake-ready
mkdir "$termux_pid_dir" "$callback_pid_dir"

assert_equal()
{
    expected=$1
    actual=$2
    context=$3
    [ "$actual" = "$expected" ] || fail_test "$context: expected <$expected>, got <$actual>"
}

assert_file_exists()
{
    file=$1
    context=$2
    [ -e "$file" ] || fail_test "$context: missing $file"
}

assert_file_absent()
{
    file=$1
    context=$2
    [ ! -e "$file" ] || fail_test "$context: unexpected $file"
}

assert_log_line()
{
    line=$1
    context=$2
    grep -Fx "$line" "$log" >/dev/null || fail_test "$context: missing log line $line"
}

wait_for_file()
{
    file=$1
    attempts=0
    while [ ! -e "$file" ]; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 20 ]; then
            printf 'torne-termux test: state at timeout for %s:\n' "$file" >&2
            find "$test_root" -maxdepth 2 -print >&2
            ps -eo pid=,ppid=,pgid=,stat=,args= | awk -v root="$test_root" 'index($0, root) { print }' >&2
            fail_test "timed out waiting for $file"
        fi
        sleep 1
    done
}

wait_for_nonempty_file()
{
    file=$1
    attempts=0
    while [ ! -s "$file" ]; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 20 ]; then
            printf 'torne-termux test: state at timeout for non-empty %s:\n' "$file" >&2
            find "$test_root" -maxdepth 2 -print >&2
            ps -eo pid=,ppid=,pgid=,stat=,args= | awk -v root="$test_root" 'index($0, root) { print }' >&2
            fail_test "timed out waiting for non-empty $file"
        fi
        sleep 1
    done
}

process_group()
{
    awk '{print $5}' "/proc/$1/stat"
}

assert_pid_gone()
{
    pid=$1
    attempts=0
    while kill -0 "$pid" 2>/dev/null; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 20 ] || {
            fail_test "${context:-launcher signal}: process remained: $pid"
        }
        sleep 1
    done
}

launch_launcher()
{
    # POSIX shells ignore INT for asynchronous lists.  Reset it in an
    # external helper before setsid so the signal matrix reaches the launcher
    # as it would when invoked from an interactive Termux shell.
    perl -e '$SIG{INT} = "DEFAULT"; $SIG{TERM} = "DEFAULT"; $SIG{HUP} = "DEFAULT"; exec @ARGV or die "exec: $!"' \
        -- setsid sh "$launcher" "$@" &
    launcher_pid=$!
}

run_signal_test()
{
    signal_name=$1
    signal_number=$2
    expected_status=$((128 + signal_number))
    rm -f "$endpoint" "$signal_file" "$signal_latch" "$signal_tmp" \
        "$termux_pid_dir/one" "$callback_pid_dir/one" "$pid_file"
    PATH="$fake_bin:$PATH" FAKE_TORNED_LOG="$log" FAKE_TORNED_PID="$pid_file" \
        FAKE_TORNED_WAIT=1 FAKE_TORNED_ENDPOINT="$endpoint" FAKE_TORNED_SIGNAL="$signal_file" \
        FAKE_TORNED_SIGNAL_LATCH="$signal_latch" FAKE_TORNED_SIGNAL_TMP="$signal_tmp" \
        FAKE_TERMUX_PID_DIR="$termux_pid_dir" FAKE_CALLBACK_PID_DIR="$callback_pid_dir" \
        launch_launcher --usb-device "$usb_one" --instance termux.one
    wait_for_file "$endpoint"
    wait_for_nonempty_file "$termux_pid_dir/one"
    wait_for_nonempty_file "$callback_pid_dir/one"
    wait_for_nonempty_file "$pid_file"
    torned_pid=$(cat "$pid_file")
    termux_pid_one=$(cat "$termux_pid_dir/one")
    callback_pid_one=$(cat "$callback_pid_dir/one")

    launcher_pgid=$(process_group "$launcher_pid")
    child_pgid=$(process_group "$termux_pid_one")
    [ -n "$launcher_pgid" ] || fail_test "$signal_name topology: launcher PGID missing"
    [ -n "$child_pgid" ] || fail_test "$signal_name topology: child PGID missing"
    [ "$child_pgid" != "$launcher_pgid" ] || fail_test "$signal_name topology: supervisor and child share PGID"
    for pid in "$callback_pid_one" "$torned_pid"; do
        assert_equal "$child_pgid" "$(process_group "$pid")" "$signal_name topology for PID $pid"
    done

    kill "-$signal_name" "$launcher_pid"
    launcher_status=0
    wait "$launcher_pid" 2>/dev/null || launcher_status=$?
    assert_equal "$expected_status" "$launcher_status" "$signal_name launcher status"
    wait_for_nonempty_file "$signal_file"
    assert_equal "$signal_number" "$(cat "$signal_file")" "$signal_name torned signal identity"
    for pid in "$torned_pid" "$termux_pid_one" "$callback_pid_one"; do
        context="$signal_name cleanup"
        assert_pid_gone "$pid"
    done
    assert_file_absent "$endpoint" "$signal_name endpoint cleanup"
    rm -f "$signal_latch" "$signal_tmp"
    assert_file_absent "$signal_latch" "$signal_name signal latch cleanup"
    assert_file_absent "$signal_tmp" "$signal_name signal temp cleanup"
}

version=$(PATH="$fake_bin:$PATH" FAKE_TORNED_VERSION=1 sh "$launcher" --version)
assert_equal 'torne-termux 0.1.0' "$version" 'source launcher version'
PATH="$fake_bin:$PATH" sh "$launcher" --help >/dev/null

for arguments in \
    '--version --help' \
    '--help --usb-device' \
    '' \
    "--usb-device $usb_one" \
    '--instance termux.one' \
    "--usb-device $usb_one --usb-device $usb_one --instance termux.one" \
    "--usb-device $usb_one --instance termux.one --instance termux.two" \
    "--usb-device $usb_one --instance termux.one --runtime-dir a --runtime-dir b" \
    "--usb-device $usb_one --instance termux.one --fd 7" \
    "--usb-device $usb_one --instance termux.one --unknown"
do
    status=0
    # Each entry is a deliberately word-split argv string.
    # shellcheck disable=SC2086
    PATH="$fake_bin:$PATH" sh "$launcher" $arguments >/dev/null 2>&1 || status=$?
    [ "$status" -eq 2 ] || fail_test "invalid arguments accepted: $arguments"
done

PATH="$fake_bin:$PATH" FAKE_TORNED_LOG="$log" FAKE_TORNED_PID="$pid_file" \
    FAKE_TERMUX_PID_DIR="$termux_pid_dir" FAKE_CALLBACK_PID_DIR="$callback_pid_dir" \
    sh "$launcher" --usb-device "$usb_one" --instance termux.one \
    --runtime-dir "$runtime"
assert_log_line '[--fd]' 'normal argument forwarding'
assert_log_line '[7]' 'normal fd forwarding'
assert_log_line '[--instance]' 'normal instance option forwarding'
assert_log_line '[termux.one]' 'normal instance forwarding'
assert_log_line '[--runtime-dir]' 'normal runtime option forwarding'
assert_log_line "[$runtime]" 'normal runtime directory forwarding'
torned_pid=$(cat "$pid_file")
if kill -0 "$torned_pid" 2>/dev/null; then
    fail_test 'normal exit: torned process remained after launcher exit'
fi

run_signal_test INT 2
run_signal_test TERM 15
run_signal_test HUP 1

rm -f "$endpoint" "$signal_file" "$signal_latch" "$signal_tmp" \
    "$ready_file" "$termux_pid_dir/one" "$callback_pid_dir/one" "$pid_file"
warning_file=$test_root/kill-warning
PATH="$fast_bin:$fake_bin:$PATH" FAKE_TORNED_LOG="$log" FAKE_TORNED_PID="$pid_file" \
    FAKE_TORNED_WAIT=1 FAKE_TORNED_IGNORE_SIGNALS=1 FAKE_TORNED_ENDPOINT="$endpoint" \
    FAKE_TORNED_SIGNAL="$signal_file" FAKE_TORNED_SIGNAL_LATCH="$signal_latch" \
    FAKE_TORNED_SIGNAL_TMP="$signal_tmp" FAKE_TERMUX_EXIT_ON_SIGNAL=1 \
    FAKE_TERMUX_PID_DIR="$termux_pid_dir" FAKE_CALLBACK_PID_DIR="$callback_pid_dir" \
    launch_launcher --usb-device "$usb_one" --instance termux.one \
    2>"$warning_file"
wait_for_file "$endpoint"
wait_for_nonempty_file "$termux_pid_dir/one"
wait_for_nonempty_file "$callback_pid_dir/one"
wait_for_nonempty_file "$pid_file"
kill -TERM "$launcher_pid"
torned_pid=$(cat "$pid_file")
termux_pid_one=$(cat "$termux_pid_dir/one")
callback_pid_one=$(cat "$callback_pid_dir/one")
launcher_status=0
wait "$launcher_pid" || launcher_status=$?
assert_equal 143 "$launcher_status" 'SIGKILL fallback direct child status'
grep -F 'using SIGKILL' "$warning_file" >/dev/null || fail_test 'SIGKILL fallback warning missing'
grep -F 'runtime endpoint removal are not guaranteed' "$warning_file" >/dev/null || \
    fail_test 'SIGKILL fallback cleanup warning incomplete'
assert_file_exists "$endpoint" 'SIGKILL fallback stale endpoint preservation'
for pid in "$torned_pid" "$termux_pid_one" "$callback_pid_one"; do
    context='SIGKILL fallback process cleanup'
    assert_pid_gone "$pid"
done
residue=$(ps -eo pid=,ppid=,pgid=,stat=,args= |
    awk -v root="$test_root" 'index($0, root) && $0 !~ /awk -v root=/ { print }')
[ -z "$residue" ] || fail_test "SIGKILL fallback process residue: $residue"
assert_file_absent "$signal_latch" 'SIGKILL fallback signal latch residue'
assert_file_absent "$signal_tmp" 'SIGKILL fallback signal temp residue'

rm -f "$endpoint" "$signal_file" "$signal_latch" "$signal_tmp" \
    "$ready_file" "$termux_pid_dir/one" "$callback_pid_dir/one" "$pid_file"
PATH="$fake_bin:$PATH" FAKE_TORNED_LOG="$log" FAKE_TORNED_PID="$pid_file" \
    FAKE_TORNED_WAIT=1 FAKE_TORNED_STOP_DELAY=0.5 FAKE_TORNED_ENDPOINT="$endpoint" \
    FAKE_TORNED_READY="$ready_file" \
    FAKE_TORNED_SIGNAL="$signal_file" FAKE_TORNED_SIGNAL_LATCH="$signal_latch" \
    FAKE_TORNED_SIGNAL_TMP="$signal_tmp" FAKE_EARLY_EXIT_USB="$usb_one" \
    FAKE_TERMUX_PID_DIR="$termux_pid_dir" FAKE_CALLBACK_PID_DIR="$callback_pid_dir" \
    launch_launcher --usb-device "$usb_one" --instance termux.one
wait_for_file "$ready_file"
wait_for_nonempty_file "$termux_pid_dir/one"
wait_for_nonempty_file "$callback_pid_dir/one"
wait_for_nonempty_file "$pid_file"
torned_pid=$(cat "$pid_file")
termux_pid_one=$(cat "$termux_pid_dir/one")
callback_pid_one=$(cat "$callback_pid_dir/one")
launcher_status=0
wait "$launcher_pid" || launcher_status=$?
assert_equal 0 "$launcher_status" 'delayed normal cleanup launcher status'
wait_for_nonempty_file "$signal_file"
assert_equal 15 "$(cat "$signal_file")" 'delayed normal cleanup signal identity'
for pid in "$torned_pid" "$termux_pid_one" "$callback_pid_one"; do
    context='delayed normal cleanup'
    assert_pid_gone "$pid"
done
assert_file_absent "$endpoint" 'delayed normal cleanup endpoint'
rm -f "$signal_latch" "$signal_tmp"
assert_file_absent "$signal_latch" 'delayed normal cleanup signal latch'
assert_file_absent "$signal_tmp" 'delayed normal cleanup signal temp'

rm -f "$endpoint" "$termux_pid_dir/one" "$callback_pid_dir/one" "$pid_file"
PATH="$fake_bin:$PATH" FAKE_FAIL_USB="$usb_one" \
    FAKE_TERMUX_PID_DIR="$termux_pid_dir" FAKE_CALLBACK_PID_DIR="$callback_pid_dir" \
    launch_launcher --usb-device "$usb_one" --instance termux.one
wait_for_nonempty_file "$termux_pid_dir/one"
launcher_status=0
wait "$launcher_pid" || launcher_status=$?
assert_equal 17 "$launcher_status" 'open failure launcher status'
termux_pid_one=$(cat "$termux_pid_dir/one")
assert_pid_gone "$termux_pid_one"
assert_file_absent "$pid_file" 'open failure torned residue'
assert_file_absent "$endpoint" 'open failure endpoint residue'

status=0
PATH="$fake_bin:$PATH" FAKE_TORNED_LOG="$log" FAKE_TORNED_PID="$pid_file" \
    FAKE_TORNED_EXIT=23 sh "$launcher" --usb-device "$usb_one" --instance termux.one || status=$?
assert_equal 23 "$status" 'torned exit status preserved'

exec 7<"$usb_one"
status=0
TORNE_TERMUX_STAGE=1 TORNE_TERMUX_INSTANCE=termux.one PATH="$fake_bin:$PATH" \
    sh "$launcher" 999999 >/dev/null 2>&1 || status=$?
[ "$status" -eq 2 ] || fail_test 'invalid callback FD accepted'
status=0
TORNE_TERMUX_STAGE=1 TORNE_TERMUX_INSTANCE=termux.one PATH="$fake_bin:$PATH" \
    sh "$launcher" 7 8 >/dev/null 2>&1 || status=$?
[ "$status" -eq 2 ] || fail_test 'multiple callback FDs accepted'
exec 7<&-

status=0
TORNE_TERMUX_STAGE=1 PATH="$fake_bin:$PATH" sh "$launcher" 7 >/dev/null 2>&1 || status=$?
[ "$status" -eq 2 ] || fail_test 'callback without instance accepted'

printf '%s\n' 'torne-termux offline tests: PASS'
