local ffi = require'ffi'

local dirname = string.sub(debug.getinfo(1).source, 2, string.len('/commandt.lua') * -1)
local library_path = dirname .. '../build/commandt.so'

local native = ffi.load(library_path)

ffi.cdef[[
  float calculate_match(const char *haystack, const char *needle, bool case_sensitive);
]]

local commandt = {}

function commandt.score(haystack, needle, case_sensitive)
  return native.calculate_match(haystack, needle, case_sensitive)
end

return commandt
