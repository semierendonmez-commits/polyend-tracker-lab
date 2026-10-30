-- name: Datum
-- author: PLATFORM
-- desc: Data as sound, after Ikeda: a bit register, sine pulses, clicks, a barcode
-- icon: grid

-- A 48-bit register (the pads) is read one bit per step: a 1 clicks. A 16-bit
-- LFSR picks the sine pulses' frequencies and mutates the register once per
-- cycle. Everything is white on black; the only colour is the cursor.

local reg = {}                 -- 48 bits, 0 / 1
local pos = 0
local lfsr = 0xACE1
local hist = {}                -- the last 96 steps, for the barcode
local ones = 0                 -- ones in the last 16 steps
local frame = 0
local rates = {1 / 8, 1 / 16, 1 / 32}

local function shift()
  -- Galois LFSR x^16 + x^14 + x^13 + x^11 + 1
  local lsb = lfsr & 1
  lfsr = lfsr >> 1
  if lsb == 1 then lfsr = lfsr ~ 0xB400 end
  return lfsr
end

local function hex(v, n)
  return string.format("%0" .. n .. "X", v)
end

function init()
  screen.fps(30)
  for i = 1, 48 do reg[i] = (i % 3 == 1 or i % 7 == 0) and 1 or 0 end
  params:add_option("rate", "Rate", {"1/32", "1/64", "1/128"}, 2, "DATA")
  params:add_number("length", "Length", 8, 48, 48, nil, "bits", "DATA")
  params:add{id = "density", name = "Density", min = 0, max = 1, default = 0.85, page = "DATA"}
  params:add{id = "mutate", name = "Mutation", min = 0, max = 1, default = 0.15, page = "DATA"}
  params:add{id = "pitch", name = "Pulse pitch", min = 1000, max = 16000, default = 9600, unit = "Hz", page = "TONE"}
  params:add{id = "click", name = "Click decay", min = 0.001, max = 0.04, default = 0.004, unit = "s", page = "TONE",
             action = function(v) engine.set(1, "decay", v) end}
  params:add{id = "pulse", name = "Pulse level", min = 0, max = 1, default = 0.35, page = "TONE",
             action = function(v) engine.set(2, "amp", v * 0.5) end}
  params:add{id = "sub", name = "Sub level", min = 0, max = 1, default = 0.4, page = "TONE",
             action = function(v) engine.set(3, "amp", v * 0.7) end}
  -- how many ones went by: an envelope for anything on the MOD page
  mod.source("BITS", {fn = function() return ones / 16 end})
  engine.voice(1, {wave = "click", env_mode = "perc", attack = 0, decay = 0.004, amp = 0.9, ftype = "off"})
  engine.voice(2, {wave = "sine", env_mode = "perc", attack = 0, decay = 0.012, amp = 0.18, ftype = "off"})
  engine.voice(3, {wave = "sine", env_mode = "perc", attack = 0.001, decay = 0.09, amp = 0.28, ftype = "off"})
  engine.voice(4, {wave = "noise", env_mode = "perc", attack = 0, decay = 0.002, ftype = "hp", cutoff = 9000, amp = 0.25})
  engine.fx("reverb_level", 0)
  engine.fx("delay_level", 0)
  clock.run(function()
    while true do
      clock.sync(rates[params:get("rate")])
      local len = math.floor(params:get("length"))
      pos = pos % len + 1
      local bit = reg[pos]
      local r = shift()
      if bit == 1 and math.random() < params:get("density") then
        engine.note(1, 60, 1)
        if (r & 7) == 0 then engine.note(4, 60, 1) end
      end
      -- every 4th step: a sine pulse at a pitch the LFSR picks from 4 ratios
      if pos % 4 == 1 then
        local ratios = {1, 1.5, 2, 0.75}
        engine.hz(2, params:get("pitch") * ratios[(r >> 4) % 4 + 1], 1)
      end
      if pos == 1 then engine.hz(3, 55 * (1 + (r >> 8) % 3), 1) end
      -- once per cycle the register drifts
      if pos == len and math.random() < params:get("mutate") then
        local i = r % len + 1
        reg[i] = 1 - reg[i]
      end
      hist[#hist + 1] = bit
      if #hist > 96 then table.remove(hist, 1) end
      ones = 0
      for k = math.max(1, #hist - 15), #hist do ones = ones + hist[k] end
    end
  end)
end

function pad(n, vel, z)
  if z == 1 then reg[n] = 1 - reg[n] end
end

function key(name, z)
  if z ~= 1 then return end
  if name == "DELETE" then for i = 1, 48 do reg[i] = 0 end end
  if name == "INSERT" then for i = 1, 48 do reg[i] = math.random() < 0.4 and 1 or 0 end end
  if name == "F1" then params:delta("rate", 1) end
end

function enc(d, shift_)
  if shift_ then params:delta("pitch", d) else params:delta("density", d) end
end

function redraw()
  frame = frame + 1
  local len = math.floor(params:get("length"))
  -- header
  screen.font("title")
  screen.color(0xFFFFFF)
  screen.text(30, 34, "DATUM")
  screen.font("small")
  screen.level(10)
  screen.text(190, 44, string.format("lfsr 0x%s   pos %02d/%02d   %s   %.0f Hz", hex(lfsr, 4), pos, len,
    params:string("rate"), params:get("pitch")))
  -- the barcode: one bar per recent step (a 1 is white)
  local bars = {}
  for i, b in ipairs(hist) do
    if b == 1 then
      local x = 30 + (i - 1) * 7.6
      bars[#bars + 1] = x; bars[#bars + 1] = 74; bars[#bars + 1] = x + 4; bars[#bars + 1] = 214
    end
  end
  screen.color(0xFFFFFF)
  screen.rects(bars)
  -- the register as 48 cells, the read head in red
  local cells, offs = {}, {}
  for i = 1, 48 do
    local x, y = 30 + ((i - 1) % 24) * 30, 238 + ((i - 1) // 24) * 30
    local t = reg[i] == 1 and cells or offs
    t[#t + 1] = x; t[#t + 1] = y; t[#t + 1] = x + 24; t[#t + 1] = y + 24
  end
  screen.color(0xFFFFFF)
  screen.rects(cells)
  screen.color(0x262626)
  screen.rects(offs)
  local cx, cy = 30 + ((pos - 1) % 24) * 30, 238 + ((pos - 1) // 24) * 30
  screen.color(0xFF2A2A)
  screen.frame(cx - 3, cy - 3, 30, 30)
  -- a hex dump of the register, 3 words of 16 bits
  screen.font("tiny")
  screen.level(12)
  for w = 0, 2 do
    local v = 0
    for b = 1, 16 do v = v * 2 + reg[w * 16 + b] end
    screen.text(30 + w * 120, 316, "0x" .. hex(v, 4))
  end
  -- a scope of the last 64 LFSR states as a staircase
  local pts = {}
  local s = lfsr
  for i = 0, 63 do
    pts[#pts + 1] = 420 + i * 5.6
    pts[#pts + 1] = 384 - ((s >> (i % 12)) & 15) * 4
  end
  screen.level(8)
  screen.polyline(pts, 1)
  screen.level(5)
  screen.text(30, 352, string.format("ones %2d/16   density %.2f   mutation %.2f", ones, params:get("density"),
    params:get("mutate")))
  screen.text(30, 432, "pads: bits   Insert: random   Delete: clear   F1: rate   enc: density (Shift: pitch)")
  -- pads mirror the register, the head lit
  for i = 1, 48 do
    local x, y = (i - 1) % 12 + 1, (i - 1) // 12 + 1
    if i == pos then grid.led(x, y, 255, 40, 40)
    elseif i > len then grid.led(x, y, 2, 2, 2)
    elseif reg[i] == 1 then grid.led(x, y, 200, 200, 200)
    else grid.led(x, y, 10, 10, 10) end
  end
end
