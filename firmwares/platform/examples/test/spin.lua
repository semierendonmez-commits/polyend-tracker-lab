-- name: Spin
-- author: PLATFORM tests
-- desc: An endless loop on pad 1: the instruction limit stops it
-- icon: spark

function pad(n, vel, z)
  if z == 1 and n == 1 then
    local x = 0
    while true do x = x + 1 end
  end
end

function redraw()
  screen.level(15)
  screen.text_center(400, 200, "press pad 1 to hang this script")
end
