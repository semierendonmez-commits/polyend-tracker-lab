-- name: Hello
-- author: PLATFORM
-- desc: Drawing demo: shapes, text, encoder colour, pads play sines
-- icon: spark

local t = 0
local hue = 0.55
local hits = {}

local function hsv(h, s, v)
  local i = math.floor(h * 6) % 6
  local f = h * 6 - math.floor(h * 6)
  local p, q, u = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
  local r, g, b
  if i == 0 then r, g, b = v, u, p elseif i == 1 then r, g, b = q, v, p
  elseif i == 2 then r, g, b = p, v, u elseif i == 3 then r, g, b = p, q, v
  elseif i == 4 then r, g, b = u, p, v else r, g, b = v, p, q end
  return math.floor(r * 255), math.floor(g * 255), math.floor(b * 255)
end

function init()
  screen.fps(20)
  for v = 1, 8 do
    engine.voice(v, {wave = "sine", attack = 0.01, release = 0.6, amp = 0.35, send_reverb = 0.3})
  end
  engine.fx("reverb_level", 0.35)
  print("hello from Lua " .. _VERSION)
end

function enc(d)
  hue = (hue + d * 0.01) % 1
end

function key(name, z)
  if name == "ENC" and z == 1 then hue = math.random() end
end

function pad(n, vel, z)
  local v = (n - 1) % 8 + 1
  if z == 1 then
    engine.note(v, 48 + n, vel / 127)
    hits[#hits + 1] = {x = 60 + ((n - 1) % 12) * 62, y = 120 + ((n - 1) // 12) * 70, age = 0}
  else
    engine.off(v)
  end
end

function redraw()
  t = t + 1
  local r, g, b = hsv(hue, 0.7, 1)
  screen.color(r, g, b)
  screen.font("huge")
  screen.text_center(400, 60, "HELLO, PLATFORM")
  screen.font("small")
  screen.level(8)
  screen.text_center(400, 96, "turn the encoder - play the pads - hold Aux for the launcher")
  -- orbiting points
  local pts = {}
  for i = 0, 23 do
    local a = i / 24 * 2 * math.pi + t * 0.03
    pts[#pts + 1] = 400 + math.cos(a) * (140 + 30 * math.sin(t * 0.05 + i))
    pts[#pts + 1] = 280 + math.sin(a) * 90
  end
  screen.color(hsv((hue + 0.3) % 1, 0.6, 1))
  screen.points(pts, 4)
  -- a sine line
  local line = {}
  for x = 0, 40 do
    line[#line + 1] = 40 + x * 18
    line[#line + 1] = 420 + math.sin(x * 0.4 + t * 0.15) * 22
  end
  screen.color(r, g, b)
  screen.polyline(line, 2)
  -- pad hits
  for i = #hits, 1, -1 do
    local h = hits[i]
    h.age = h.age + 1
    screen.level(math.max(0, 15 - h.age))
    screen.ring(h.x, h.y, 10 + h.age * 3, 20)
    if h.age > 15 then table.remove(hits, i) end
  end
  screen.level(5)
  screen.frame(20, 20, 760, 440)
  -- pads mirror the colour
  for y = 1, 4 do
    for x = 1, 12 do
      local rr, gg, bb = hsv((hue + x / 24 + y / 16) % 1, 0.8, 0.25)
      grid.led(x, y, rr, gg, bb)
    end
  end
end
