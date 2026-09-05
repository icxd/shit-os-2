# shit os 2 -- the window server, end to end.
#
# A screenshot proves the desktop looks right to a human once. This proves the
# parts a human cannot see stayed connected: that a client which knows only a
# path can reach the server, that it is given a window, and that the buffer it
# is handed is the size it asked for. Every one of those is a FIFO, a syscall
# and a shared mapping working together.

ok=0
fail=0
check() {
    if [ "$2" = "$3" ]; then
        ok=$((ok + 1))
    else
        fail=$((fail + 1))
        echo "  FAIL $1: expected [$3], got [$2]"
    fi
}

# The server takes the screen, so anything it prints goes to serial, not here.
wsys &
server=$!
sleep 1

check "the server is running" "$(kill -0 $server 2>/dev/null && echo yes || echo no)" "yes"
check "the connect fifo exists" "$(test -p /tmp/wsys/connect && echo yes || echo no)" "yes"

wsysdemo probe 200 150 &
client=$!
sleep 2

# 200 x 150 pixels, four bytes each. If the client had been given a buffer of
# the wrong shape it would still draw -- into the wrong memory.
check "the window buffer exists" "$(test -f /tmp/wsys/win1.px && echo yes || echo no)" "yes"
check "and is exactly the size asked for" "$(wc -c < /tmp/wsys/win1.px 2>/dev/null)" "120000"

# The client made its own channels before announcing itself; both should still
# be there while it lives.
check "the client channel exists" \
    "$(test -p /tmp/wsys/$client.to-server && echo yes || echo no)" "yes"

check "the client is still running" "$(kill -0 $client 2>/dev/null && echo yes || echo no)" "yes"

# Closing the client must take its window with it, buffer and all.
kill $client 2>/dev/null
sleep 2
check "the buffer goes when the client does" \
    "$(test -f /tmp/wsys/win1.px && echo present || echo gone)" "gone"

# And the server must survive a client dying, which is the whole reason it
# ignores SIGPIPE.
check "the server outlives its client" "$(kill -0 $server 2>/dev/null && echo yes || echo no)" "yes"

kill $server 2>/dev/null
sleep 1

echo "$ok passed, $fail failed"
[ "$fail" -eq 0 ]
