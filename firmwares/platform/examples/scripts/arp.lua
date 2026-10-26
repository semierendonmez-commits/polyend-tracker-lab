-- name: Arp
-- author: PLATFORM
-- desc: MIDI arpeggiator: hold pads or MIDI notes, sends MIDI and plays a voice
-- icon: arp

local held = {}      -- note -> velocity
local order = {}
local idx = 0
local last = nil
local modes = {"up", "down", "updown", "random", "played"}
local rates = {1, 1/2, 1/3, 1/4, 1/6, 1/8}
local rateNames = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"}
local flash = 0

local function sorted()
  local t = {}
  if modes[params:get("mode")] == "played" then
    for _, n in ipairs(order) do t[#t + 1] = n end
  else
    for n in pairs(held) do t[#t + 1] = n end
    table.sort(t)
  end
  local out = {}
  for o = 0, params:get("octaves") - 1 do
    for _, n in ipairs(t) do out[#out + 1] = n + 12 * o end
  end
  return out
end

local function noteOn(n, v)
  if not held[n] then order[#order + 1] = n end
  held[n] = v
end
local function noteOff(n)
  held[n] = nil
  for i, m in ipairs(order) do if m == n then table.remove(order, i) break end end
end

function init()
  params:add_option("mode", "mode", modes, 1)
  params:add_option("rate", "rate", rateNames, 4)
  params:add_number("octaves", "octaves", 1, 4, 2)
  params:add_control("gate", "gate", 0.05, 1, 0.5, 0.05)
  params:add_number("channel", "MIDI out channel", 1, 16, 1)
  params:add_toggle("latch", "latch", false)
  params:add_number("base", "pad base note", 24, 72, 48, function(v) return music.note_name(v) end)
  engine.voice(1, {wave = "square", pw = 0.3, env_mode = "perc", decay = 0.3, cutoff = 1800, fenv = 2, res = 2,
    amp = 0.35, send_delay = 0.3})
  engine.fx("delay_time", 0.25)
  engine.fx("delay_feedback", 0.45)
  clock.run(function()
    local dir = 1
    while true do
      local r = rates[params:get("rate")]
      clock.sync(r)
      local notes = sorted()
      if #notes > 0 then
        local m = modes[params:get("mode")]
        if m == "up" or m == "played" then idx = idx % #notes + 1
        elseif m == "down" then idx = (idx - 2) % #notes + 1
        elseif m == "updown" then
          idx = idx + dir
          if idx > #notes then idx = math.max(1, #notes - 1); dir = -1 end
          if idx < 1 then idx = math.min(2, #notes); dir = 1 end
        else idx = math.random(#notes) end
        local n = notes[idx]
        local ch = params:get("channel")
        midi.note_on(n, 100, ch)
        engine.note(1, n, 0.9)
        last = n
        flash = 4
        clock.sleep(r * params:get("gate") * clock.get_beat_sec())
        midi.note_off(n, 0, ch)
      else
        idx = 0
      end
    end
  end)
end

function midi.event(msg)
  local t, n, v = msg[1] & 0xF0, msg[2], msg[3]
  if t == 0x90 and v > 0 then noteOn(n, v)
  elseif t == 0x80 or (t == 0x90 and v == 0) then if not params:get("latch") then noteOff(n) end end
end

function pad(n, vel, z)
  local note = params:get("base") + (n - 1) % 12 + 12 * (3 - (n - 1) // 12)
  if z == 1 then
    if params:get("latch") and held[note] then noteOff(note) else noteOn(note, vel) end
  elseif not params:get("latch") then
    noteOff(note)
  end
end

function enc(d)
  params:delta("rate", d)
end

function key(name, z)
  if z ~= 1 then return end
  if name == "F1" then params:delta("mode", 1) end
  if name == "F2" then params:delta("octaves", 1) end
  if name == "F3" then params:set("latch", params:get("latch") and 1 or 2) end
  if name == "DELETE" then held = {}; order = {} end
end

function redraw()
  screen.font("title")
  screen.level(12)
  screen.text(24, 36, "ARP")
  screen.font("body")
  screen.color(0x39E6B4)
  screen.text(120, 36, string.format("%s  %s  x%d  gate %.2f  ch %d%s", params:string("mode"), params:string("rate"),
    params:get("octaves"), params:get("gate"), params:get("channel"), params:get("latch") and "  LATCH" or ""))
  local notes = sorted()
  for i, n in ipairs(notes) do
    local x = 40 + (i - 1) * 46
    if x < 760 then
      local h = (n - 24) * 3
      if i == idx then screen.color(0xFFB347) else screen.level(7) end
      screen.rect(x, 400 - h, 38, h)
      screen.font("tiny")
      screen.text_center(x + 19, 414, music.note_name(n))
    end
  end
  if #notes == 0 then
    screen.level(6)
    screen.font("body")
    screen.text_center(400, 240, "hold pads or play MIDI notes")
  end
  if last then
    screen.font("huge")
    screen.color(flash > 0 and 0xFFFFFF or 0x808890)
    screen.text_right(770, 90, music.note_name(last))
  end
  flash = math.max(0, flash - 1)
  screen.font("tiny")
  screen.level(6)
  screen.text(24, 456, "F1 mode  F2 octaves  F3 latch  enc: rate  Delete: release all  Shift+Aux: params")
  local base = params:get("base")
  for i = 1, 48 do
    local note = base + (i - 1) % 12 + 12 * (3 - (i - 1) // 12)
    local x, y = (i - 1) % 12 + 1, (i - 1) // 12 + 1
    local black = ({[1]=1,[3]=1,[6]=1,[8]=1,[10]=1})[note % 12]
    if held[note] then grid.led(x, y, 255, 179, 71)
    elseif note == last then grid.led(x, y, 57, 230, 180)
    else grid.led(x, y, black and 4 or 14, black and 6 or 20, black and 10 or 28) end
  end
end
