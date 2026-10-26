-- name: Drift
-- author: PLATFORM
-- desc: Generative ambient: slow FM and sine clouds, grains, long reverb
-- icon: drone

local blobs = {}
local scale = {}

local function rebuild()
  local names = {"minor_pent", "dorian", "lydian", "hijaz", "whole"}
  scale = music.scale(params:get("root"), names[params:get("scale")], 15)
end

function init()
  params:add_number("root", "root", 36, 60, 45, function(v) return music.note_name(v) end)
  params:add_option("scale", "scale", {"minor pent", "dorian", "lydian", "hijaz", "whole"}, 1)
  params:add_control("density", "density", 0.1, 1, 0.45, 0.05)
  params:add_control("space", "space", 0, 1, 0.8, 0.05)
  params:set_action("root", rebuild)
  params:set_action("scale", rebuild)
  params:set_action("space", function(v)
    engine.fx("reverb_size", 0.6 + v * 0.4)
    engine.fx("reverb_level", 0.2 + v * 0.5)
  end)
  rebuild()
  for v = 1, 6 do
    engine.voice(v, {wave = (v % 2 == 0) and "fm" or "sine", fm_ratio = 2.01, fm_index = 0.8,
      attack = 2.5, decay = 3, sustain = 0.6, release = 6, amp = 0.16,
      pan = (v - 3.5) / 3, send_reverb = 0.7, send_delay = 0.25, cutoff = 2500})
  end
  engine.voice(7, {wave = "grain", grain_size = 0.12, grain_density = 14, amp = 0.12, attack = 4, release = 8,
    send_reverb = 0.9})
  engine.fx("delay_time", 0.75)
  engine.fx("delay_feedback", 0.55)
  engine.fx("delay_level", 0.25)
  for v = 1, 7 do
    clock.run(function()
      clock.sleep(v * 0.7)
      while true do
        local n = scale[math.random(#scale)]
        engine.note(v, n + (v == 7 and 12 or 0), 0.6 + math.random() * 0.4)
        blobs[#blobs + 1] = {x = 60 + math.random() * 680, y = 80 + (1 - (n - 30) / 50) * 300, r = 4, v = v}
        clock.sleep(2 + math.random() * 6 / params:get("density"))
        engine.off(v)
        clock.sleep(1 + math.random() * 4 / params:get("density"))
      end
    end)
  end
  screen.fps(15)
end

function enc(d)
  params:delta("density", d)
end

function pad(n, vel, z)
  if z == 1 then
    local v = 8 + (n - 1) % 6
    engine.voice(v, {wave = "sine", attack = 0.8, release = 5, amp = 0.2, send_reverb = 0.8})
    engine.note(v, scale[(n - 1) % #scale + 1] + 12, vel / 127)
  else
    engine.off(8 + (n - 1) % 6)
  end
end

function redraw()
  screen.font("title")
  screen.level(10)
  screen.text(30, 40, "DRIFT")
  screen.font("small")
  screen.level(5)
  screen.text(140, 46, string.format("%s %s   density %.2f   space %.2f", music.note_name(params:get("root")),
    params:string("scale"), params:get("density"), params:get("space")))
  for i = #blobs, 1, -1 do
    local b = blobs[i]
    b.r = b.r + 1.2
    b.y = b.y - 0.3
    local k = math.max(0, 1 - b.r / 160)
    screen.color(math.floor(60 * k), math.floor((140 + b.v * 12) * k), math.floor(200 * k))
    screen.ring(b.x, b.y, b.r, 28)
    if k <= 0 then table.remove(blobs, i) end
  end
  for v = 1, 7 do
    local l = engine.level(v)
    screen.color(math.floor(40 + 200 * l), math.floor(120 + 100 * l), 255)
    screen.rect(30 + (v - 1) * 24, 440 - math.floor(l * 60), 16, math.floor(l * 60) + 2)
  end
  for i = 1, 48 do
    local x, y = (i - 1) % 12 + 1, (i - 1) // 12 + 1
    local l = engine.level((i - 1) % 7 + 1)
    grid.led(x, y, 0, math.floor(20 + 80 * l), math.floor(40 + 120 * l))
  end
end
