l = require "lsrvsock"

local s = assert(l.new("0.0.0.0", 63000))

while true do
	if s:isconnected("") then
		local buf = s:read("")
		if buf then
			s:write(buf)
		else
			print("disconnect", s:getpeername())
		end
	end
	l.sleep(50)
end

