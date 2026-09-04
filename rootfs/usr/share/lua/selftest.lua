local ok, fail = 0, 0
local function check(name, cond, got)
  if cond then ok = ok + 1 else fail = fail + 1
    print(("  FAIL %s%s"):format(name, got and (" -> "..tostring(got)) or "")) end
end

-- integers and floats
check("int arith", 6*7 == 42)
check("float div", 1/4 == 0.25)
check("int div", 7//2 == 3)
check("modulo", 7%3 == 1)
check("pow", 2^10 == 1024)
check("bigint", 9007199254740993 + 1 == 9007199254740994)

-- the libm, through Lua's math library
check("sqrt", math.abs(math.sqrt(2) - 1.4142135623730951) < 1e-15, math.sqrt(2))
check("pi", math.abs(math.pi - 3.141592653589793) < 1e-15)
check("sin", math.abs(math.sin(math.pi/6) - 0.5) < 1e-12, math.sin(math.pi/6))
check("cos", math.abs(math.cos(0) - 1.0) < 1e-15)
check("exp", math.abs(math.exp(1) - 2.718281828459045) < 1e-14, math.exp(1))
check("log", math.abs(math.log(math.exp(3)) - 3) < 1e-14, math.log(math.exp(3)))
check("log10", math.abs(math.log(1000, 10) - 3) < 1e-13, math.log(1000,10))
check("floor", math.floor(-2.5) == -3)
check("ceil", math.ceil(-2.5) == -2)
check("fmod", math.abs(math.fmod(7.5, 2) - 1.5) < 1e-15)
check("huge", math.huge > 1e308)
check("maxinteger", math.maxinteger + 1 == math.mininteger)

-- string formatting exercises our printf and strtod
check("format %d", ("%d"):format(42) == "42")
check("format %5.2f", ("%5.2f"):format(3.14159) == " 3.14", ("%5.2f"):format(3.14159))
check("format %x", ("%x"):format(255) == "ff")
check("format %s", ("%s|%s"):format("a","b") == "a|b")
check("format %g", ("%g"):format(0.0001) == "0.0001", ("%g"):format(0.0001))
check("tonumber dec", tonumber("3.5") == 3.5)
check("tonumber exp", tonumber("1e3") == 1000)
check("tonumber hex", tonumber("0x1p4") == 16, tonumber("0x1p4"))
check("tostring float", tostring(0.1) == "0.1", tostring(0.1))

-- strings
check("upper", ("hello"):upper() == "HELLO")
check("rep", ("ab"):rep(3) == "ababab")
check("sub", ("abcdef"):sub(2,4) == "bcd")
check("find", ("hello world"):find("wor") == 7)
check("gsub", ("a,b,c"):gsub(",", ";") == "a;b;c")
check("byte", ("A"):byte() == 65)
check("reverse", ("abc"):reverse() == "cba")
local m = ("key=value"):match("(%w+)=(%w+)")
check("match", m == "key")

-- tables and sorting (our qsort is not used by Lua, but table.sort is a workout)
local t = {}
for i = 1, 500 do t[i] = (i * 7919) % 1000 end
table.sort(t)
local sorted = true
for i = 2, #t do if t[i-1] > t[i] then sorted = false end end
check("table.sort", sorted)
check("table.concat", table.concat({1,2,3}, "-") == "1-2-3")
check("#table", #({1,2,3,4}) == 4)

-- closures, metatables, coroutines
local function counter() local n = 0; return function() n = n + 1; return n end end
local c = counter(); c(); c()
check("closure", c() == 3)

local mt = setmetatable({}, {__index = function() return "dflt" end,
                            __add = function(a,b) return "added" end})
check("__index", mt.missing == "dflt")
check("__add", (mt + mt) == "added")

local co = coroutine.create(function(a) local b = coroutine.yield(a+1); return b*2 end)
local _, v1 = coroutine.resume(co, 10)
local _, v2 = coroutine.resume(co, 5)
check("coroutine", v1 == 11 and v2 == 10, tostring(v1)..","..tostring(v2))

-- error handling: this is setjmp/longjmp
local okc, err = pcall(function() error("boom") end)
check("pcall catches", okc == false and tostring(err):find("boom") ~= nil, err)
local okd = pcall(function() local x = nil; return x.y end)
check("pcall runtime error", okd == false)
local deep
deep = function(n) if n == 0 then error("bottom") end; return deep(n-1) end
local oke, erre = pcall(deep, 200)
check("pcall unwinds deep", oke == false and tostring(erre):find("bottom") ~= nil)
check("xpcall handler", select(2, xpcall(function() error("x") end,
      function(e) return "handled" end)) == "handled")

-- garbage collector: exercises malloc/realloc/free hard
local before = collectgarbage("count")
local junk = {}
for i = 1, 20000 do junk[i] = {value = i, text = "item" .. i} end
junk = nil
collectgarbage("collect")
check("gc runs", collectgarbage("count") < before + 500,
      ("%.0f -> %.0f KB"):format(before, collectgarbage("count")))

-- file I/O through our stdio: fopen, fwrite, fseek, ftell, fread, fclose
local path = "/tmp/luatest.txt"
local f = assert(io.open(path, "w"))
f:write("line one\n", "line two\n", 12345, "\n")
f:close()

f = assert(io.open(path, "r"))
check("read line", f:read("l") == "line one")
local pos = f:seek()
check("ftell nonzero", pos == 9, pos)
check("read line 2", f:read("l") == "line two")
f:seek("set", 0)
check("rewind reads again", f:read("l") == "line one")
f:seek("end", 0)
check("seek end", f:seek() == 24, f:seek())
f:close()

f = assert(io.open(path, "r"))
local all = f:read("a")
check("read all", #all == 24, #all)
f:close()

f = assert(io.open(path, "a"))
f:write("appended\n")
f:close()
f = assert(io.open(path, "r"))
local grown = f:read("a")
check("append grew file", #grown == 33, #grown)
f:close()
os.remove(path)
check("os.remove", io.open(path, "r") == nil)

-- os library
check("os.time", type(os.time()) == "number")
check("os.clock", type(os.clock()) == "number")
check("os.date", type(os.date("%Y-%m-%d")) == "string", os.date("%Y-%m-%d"))
check("os.getenv PATH", os.getenv("PATH") == "/bin", tostring(os.getenv("PATH")))

-- floating point formatting, which is what found the %f/%e/%g gap
check("format %f", ("%f"):format(1.5) == "1.500000", ("%f"):format(1.5))
check("format %.0f", ("%.0f"):format(2.5) == "3", ("%.0f"):format(2.5))
check("format %e", ("%e"):format(1234.5) == "1.234500e+03", ("%e"):format(1234.5))
check("format %.3g", ("%.3g"):format(0.000123456) == "0.000123", ("%.3g"):format(0.000123456))
check("format %g big", ("%g"):format(1e20) == "1e+20", ("%g"):format(1e20))
check("format neg", ("%.2f"):format(-3.456) == "-3.46", ("%.2f"):format(-3.456))
check("tostring int float", tostring(2.0) == "2.0", tostring(2.0))
check("tostring 1/3", tostring(1/3) == "0.33333333333333", tostring(1/3))
check("format inf", ("%f"):format(math.huge) == "inf", ("%f"):format(math.huge))

print(("%d passed, %d failed"):format(ok, fail))
os.exit(fail == 0 and 0 or 1)
