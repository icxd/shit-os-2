-- shit os 2 -- what happens to a file after its last name is gone.
--
-- POSIX says an open file survives unlink and keeps working until the last
-- descriptor closes. This kernel used to free the inode with the name, so a
-- reader got whatever the heap handed out next; docs/roadmap.md records the
-- reproduction. These checks are what stop it coming back.

local ok, fail = 0, 0
local function check(name, cond, got)
  if cond then ok = ok + 1 else fail = fail + 1
    print(("  FAIL %s%s"):format(name, got and (" -> "..tostring(got)) or "")) end
end

-- Allocating and freeing enough files that a stale inode would certainly have
-- been handed back out by now.
local function churn(n)
  for i = 1, n do
    local h = io.open("/tmp/churn" .. i, "w")
    h:write("ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ")
    h:close()
  end
  for i = 1, n do os.remove("/tmp/churn" .. i) end
end

-- 1. Read through a descriptor whose name was removed underneath it.
do
  local f = io.open("/tmp/x", "w")
  f:write("CANARY-DATA-1234")
  f:close()

  local g = io.open("/tmp/x", "r")
  check("unlink of an open file succeeds", os.remove("/tmp/x"))
  check("the name is gone", io.open("/tmp/x", "r") == nil)
  churn(200)
  local got = g:read("a")
  g:close()
  check("an unlinked file still reads its own data", got == "CANARY-DATA-1234", got)
end

-- 2. Write to a descriptor whose name was removed, then read it back.
do
  local f = io.open("/tmp/y", "w+")
  os.remove("/tmp/y")
  f:write("written-after-unlink")
  f:seek("set", 0)
  churn(100)
  local got = f:read("a")
  f:close()
  check("an unlinked file is still writable", got == "written-after-unlink", got)
end

-- 3. tmpfile() relies on all of the above, so it is the real end-to-end check.
do
  local t = io.tmpfile()
  t:write("tmpfile-round-trip")
  churn(100)
  t:seek("set", 0)
  local got = t:read("a")
  t:close()
  check("tmpfile survives its own unlink", got == "tmpfile-round-trip", got)
end

-- 4. A file removed with nothing open really is gone.
do
  local f = io.open("/tmp/z", "w")
  f:write("transient")
  f:close()
  os.remove("/tmp/z")
  check("a closed file removed by name stays removed", io.open("/tmp/z", "r") == nil)
end

-- os.clock() here is uptime, not process time: a useful smoke signal, since
-- this suite went from 42 seconds to a fraction of one when the libc allocator
-- stopped scanning the whole heap on every free.
print(("%d passed, %d failed, %.2f s since boot"):format(ok, fail, os.clock()))
os.exit(fail == 0 and 0 or 1)
