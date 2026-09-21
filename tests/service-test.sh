#!/bin/sh
#
# Install the packages and start the service, under a real systemd.
#
# ## Why this exists
#
# The suites test the library.  Dockerfile builds the .deb.  Nothing installed
# the package and started the service, and that is where the jhttpd deploy of
# 2026-09-20 broke -- twice, in ways every other check passed:
#
#   - the unit said ExecStart=/usr/local/bin/jhttpd, where `make install` puts
#     it, while the package installs /usr/bin/jhttpd.  "No such file or
#     directory", on a machine that had just been handed six live sites.
#
#   - the unit said `User=root`, which reads as harmless explicitness on a
#     service that already runs as root.  It makes systemd take its
#     change-identity path, which strips CAP_SETUID from the effective set --
#     leaving it in the *bounding* set, so `systemctl show` still lists it --
#     and jhttpd could not drop privilege:
#
#         jhttpd: sys exception: setuid() failed: Operation not permitted
#
# `--test` cannot reach either: it validates a config against the filesystem,
# and neither the unit's paths nor the sandbox it will run under are in the
# config.  The suites cannot reach either: they never see a unit file.
#
# So: a container running systemd, the real .deb installed into it, and the
# real unit started.
#
# ## Usage
#
#     tests/service-test.sh                 build the .debs, then test them
#     tests/service-test.sh --debs DIR      test .debs already built
#
# Needs docker, and --privileged, because systemd wants cgroups.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
debs=""
name=jlib-service-test

while [ $# -gt 0 ]; do
    case "$1" in
        --debs) debs=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }
ok()   { printf '  ok     %s\n' "$*"; }
bad()  { printf '  FAIL   %s\n' "$*"; failures=$((failures + 1)); }

failures=0

cleanup() { docker rm -f "$name" >/dev/null 2>&1 || true; }
trap cleanup EXIT

# ---------------------------------------------------------------- the packages

if [ -z "$debs" ]; then
    debs=$(mktemp -d)

    say "building the packages"

    docker build -q -t jlib-deb "$here" >/dev/null
    docker run --rm -v "$debs":/out jlib-deb sh -c '
        cd /src/jlib
        DEB_BUILD_OPTIONS=nocheck dpkg-buildpackage -us -uc -b >/tmp/b.log 2>&1 \
            || { tail -20 /tmp/b.log; exit 1; }
        cp /src/jlib_*.deb /src/jhttpd_*.deb /out/'
fi

say "packages: $(ls "$debs" | tr '\n' ' ')"

# ------------------------------------------------------------------- the boot

docker build -q -f "$here/Dockerfile.service" -t jlib-systemd "$here" >/dev/null

docker rm -f "$name" >/dev/null 2>&1 || true
docker run -d --name "$name" --privileged --cgroupns=host \
    -v /sys/fs/cgroup:/sys/fs/cgroup:rw \
    -v "$debs":/debs:ro \
    jlib-systemd >/dev/null

# systemd needs a moment, and polling beats a fixed sleep on a loaded machine.
i=0
while [ "$i" -lt 60 ]; do
    state=$(docker exec "$name" systemctl is-system-running 2>/dev/null || true)

    case "$state" in running|degraded) break ;; esac

    i=$((i + 1))
    sleep 0.5
done

say "systemd: ${state:-did not come up}"

run() { docker exec "$name" sh -c "$1"; }

# ---------------------------------------------------------------- the install

say ""
say "installing:"

# The image drops /var/lib/apt/lists to stay small, so apt has nothing to
# resolve jlib's dependencies against until this runs.
run 'apt-get update -qq >/dev/null 2>&1' || true

installed=no

if run 'apt-get install -y -qq /debs/jlib_*.deb /debs/jhttpd_*.deb >/tmp/i.log 2>&1'; then
    ok "the packages install"
    installed=yes
else
    bad "the packages install"
    run 'tail -20 /tmp/i.log' || true
fi

# The X11 question: a headless machine installing a web server should not
# acquire libgl1.  This container has no X11, so apt would have had to fetch
# it -- which makes a clean install the assertion.
#
# **Gated on the install having happened.**  Without the gate this passes when
# nothing was installed at all, which is not a fact about the packaging; the
# first run of this script did exactly that and read as a pass.
if [ "$installed" = no ]; then
    bad "no X11 stack was pulled in (not tested: nothing installed)"
elif run 'dpkg -l libgl1 2>/dev/null | grep -q "^ii"'; then
    bad "installing jhttpd pulled in libgl1"
else
    ok "no X11 stack was pulled in"
fi

# --------------------------------------------------------------- a config

run 'mkdir -p /srv/www && echo hello > /srv/www/index.html'
run 'cat > /etc/jhttpd.conf <<EOF
error_log   /var/log/jhttpd/error.log notice;
user        www-data www-data;
daemon;
pid         /run/jhttpd.pid;
http {
    listen      :8080;
    root        /srv/www;
    access_log  /var/log/jhttpd/access.log;
}
EOF'

say ""
say "starting the service:"

if run 'systemctl start jhttpd'; then
    ok "systemctl start succeeds"
else
    bad "systemctl start succeeds"
    run 'systemctl status jhttpd --no-pager || true'
    run 'journalctl -u jhttpd -n 20 --no-pager || true'
fi

if run 'systemctl is-active jhttpd >/dev/null'; then
    ok "the service is active"
else
    bad "the service is active"
fi

# **The CAP_SETUID case.**  A unit that cannot drop privilege fails here and
# nowhere else in this tree.
if run 'journalctl -u jhttpd --no-pager | grep -q "setuid() failed"'; then
    bad "the privilege drop is refused (setuid() failed)"
else
    ok "the privilege drop is not refused"
fi

if run 'ps -o user= -C jhttpd | grep -q www-data'; then
    ok "and it is running as www-data, not root"
else
    bad "and it is running as www-data, not root"
    run 'ps -o pid=,user=,comm= -C jhttpd || true'
fi

# **The Type= / daemon; agreement** (#334).  Neither half can see the other,
# so ExecStartPre passes --forking and --test refuses a config without it.
# Checked here because the failure it prevents is a start that hangs until
# TimeoutStartSec and then kills a server that was working.
if run 'sed -i "/^daemon;/d" /etc/jhttpd.conf && ! jhttpd --config /etc/jhttpd.conf --test --forking >/tmp/f.log 2>&1'; then
    if run 'grep -q "no daemon;" /tmp/f.log'; then
        ok "a config without daemon; is refused under --forking, and says why"
    else
        bad "a config without daemon; is refused under --forking, and says why"
        run 'cat /tmp/f.log'
    fi
else
    bad "a config without daemon; is refused under --forking"
fi

run 'sed -i "/^user /a daemon;" /etc/jhttpd.conf'

if run 'jhttpd --config /etc/jhttpd.conf --test --forking >/dev/null 2>&1'; then
    ok "and accepted once it is there"
else
    bad "and accepted once it is there"
fi

say ""
say "serving:"

if run 'curl -fsS -o /dev/null http://127.0.0.1:8080/index.html'; then
    ok "a request is answered"
else
    bad "a request is answered"
fi

if run 'grep -q "GET /index.html" /var/log/jhttpd/access.log'; then
    ok "and written to the access log"
else
    bad "and written to the access log"
fi

if run 'grep -q "configured -- resuming normal operations" /var/log/jhttpd/error.log'; then
    ok "the lifecycle notice names the build"
else
    bad "the lifecycle notice names the build"
fi

say ""
say "reload and rotation:"

# **A reload must not kill logging**, which is what it did.
#
# The logs are created by root at startup, because they live where an
# unprivileged user cannot create them; jhttpd then drops to www-data, whose
# group gets r-- from a 0644 umask. The first reopen -- which is SIGHUP, which
# is this reload -- then fails, both logs close, and the server serves on with
# no record at all.
#
# Rotation masked it completely: logrotate's `create 0640 www-data www-data`
# makes a file the dropped user owns, so the reopen after a *rotation* works.
# Only a reload before any rotation reaches it, which is why a live deploy
# that tested rotation looked fine.
run 'systemctl reload jhttpd'
sleep 1

if run 'grep -q "reloaded /etc/jhttpd.conf" /var/log/jhttpd/error.log'; then
    ok "systemctl reload reaches SIGHUP and is recorded"
else
    bad "systemctl reload reaches SIGHUP and is recorded"
    run 'ls -l /var/log/jhttpd/ || true'
fi

# **Counted before and after, not grepped for.**
#
# The first version of this asked whether "GET /index.html" appeared in the
# access log -- which the request made *before* the reload had already put
# there. It passed with the fix removed, asserting a fact about the past.
before=$(run 'wc -l < /var/log/jhttpd/access.log' 2>/dev/null | tr -d ' ')

run 'curl -fsS -o /dev/null http://127.0.0.1:8080/index.html || true'
sleep 1

after=$(run 'wc -l < /var/log/jhttpd/access.log' 2>/dev/null | tr -d ' ')

if [ "${after:-0}" -gt "${before:-0}" ]; then
    ok "and a request after the reload is still logged ($before -> $after)"
else
    bad "and a request after the reload is still logged ($before -> $after)"
    run 'ls -l /var/log/jhttpd/; ls -l /proc/$(pgrep -x jhttpd)/fd 2>/dev/null | grep jhttpd || echo "  no log fds open"'
fi

# **The owner field, not the whole line.**
#
# `ls -l | grep www-data` matched the *group*, which is www-data even when
# root owns the file -- so it passed on exactly the `root www-data 0644` that
# breaks the reopen. stat asks for the one field that matters.
owner=$(run 'stat -c %U /var/log/jhttpd/access.log' 2>/dev/null | tr -d ' \r')

if [ "$owner" = "www-data" ]; then
    ok "the logs belong to the user jhttpd became (owner $owner)"
else
    bad "the logs belong to the user jhttpd became (owner ${owner:-?})"
    run 'ls -l /var/log/jhttpd/'
fi

# **The log-directory case.**  Rotation creates a new file and SIGHUPs; the
# reopen happens after the privilege drop, so a directory www-data cannot
# traverse makes logging stop silently.  Forced here rather than waited for.
run 'logrotate -f /etc/logrotate.d/jhttpd'
run 'curl -fsS -o /dev/null http://127.0.0.1:8080/index.html || true'
sleep 1

if run 'grep -q "GET /index.html" /var/log/jhttpd/access.log'; then
    ok "logging survives a rotation"
else
    bad "logging survives a rotation"
    run 'ls -la /var/log/jhttpd/ || true'
fi

say ""
say "stopping:"

if run 'systemctl stop jhttpd && ! systemctl is-active jhttpd >/dev/null'; then
    ok "systemctl stop stops it"
else
    bad "systemctl stop stops it"
fi

say ""

if [ "$failures" -gt 0 ]; then
    say "service-test: $failures failed"
    exit 1
fi

say "service-test: all checks passed"
