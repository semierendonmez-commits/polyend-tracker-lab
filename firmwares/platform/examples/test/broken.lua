-- name: Broken
-- author: PLATFORM tests
-- desc: Fails on purpose after a moment, again and again: shows the error page
-- icon: spark

local n = 0
function init()
  print("broken.lua: from its 20th frame on, every redraw indexes a nil value")
  screen.fps(20)
end

local function inner()
  return undefined_table.field   -- runtime error: attempt to index a nil value
end

function redraw()
  n = n + 1
  screen.font("large")
  screen.level(15)
  screen.text_center(400, 200, "frame " .. n)
  if n >= 20 then inner() end   -- the crash-safe runner logs 5, stops at the 6th
end
