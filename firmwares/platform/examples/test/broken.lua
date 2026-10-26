-- name: Broken
-- author: PLATFORM tests
-- desc: Fails on purpose after a moment: shows the error page
-- icon: spark

local n = 0
function init()
  print("broken.lua: this script calls a missing function on its 20th frame")
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
  if n == 20 then inner() end
end
