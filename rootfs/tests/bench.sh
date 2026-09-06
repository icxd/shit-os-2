# shit os 2 -- what a frame costs.
#
# Not a pass/fail check: the number depends on the host, and a threshold that
# holds on one machine fails on another. It is here because a regression in
# compositing shows up as this number moving, and nobody reads a number that
# is not printed. The invariant that *is* asserted -- that printing one line
# asks to repaint one row -- lives in tools/check-ui.sh, where it is exact.
wsys &
server=$!
sleep 1
wsysbench 100
kill $server 2>/dev/null
sleep 1
