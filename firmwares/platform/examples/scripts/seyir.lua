-- name: Seyir
-- author: PLATFORM
-- desc: A makam melody walks its seyir: from the durak up to the guclu and home
-- icon: wave

-- Pitches are in 53-tone koma (1 koma = 12/53 semitone) -- engine.note takes
-- fractional notes. A seyir is the path a makam melody takes: it opens around
-- the durak (tonic), climbs to the guclu (dominant), lingers above it, then
-- descends and settles on the durak with a cadence. Each phase biases a
-- random walk over the scale; a drone holds the durak.

local MAKAM = {
  {name = "Rast", koma = {0, 9, 17, 22, 31, 40, 48, 53, 62, 70},
   notes = {"Rast", "Dugah", "Segah", "Cargah", "Neva", "Huseyni", "Evic", "Gerdaniye", "Muhayyer", "Tiz segah"},
   guclu = 5, root = 55},
  {name = "Hicaz", koma = {0, 5, 17, 22, 31, 40, 48, 53, 58, 70},
   notes = {"Dugah", "Dik kurdi", "Nim hicaz", "Neva", "Huseyni", "Evic", "Gerdaniye", "Muhayyer", "Sunbule", "Tiz hicaz"},
   guclu = 4, root = 57},
  {name = "Ussak", koma = {0, 8, 13, 22, 31, 35, 44, 53, 61, 66},
   notes = {"Dugah", "Segah", "Cargah", "Neva", "Huseyni", "Acem", "Gerdaniye", "Muhayyer", "Tiz segah", "Tiz cargah"},
   guclu = 4, root = 57},
}
local PHASES = {"zemin", "guclu", "meyan", "karar"}   -- open, dominant, upper, cadence

local deg = 1
local phase = 1
local steps = 0
local path = {}         -- the last 48 degrees
local playing = true

local function mk() return MAKAM[params:get("makam")] end

local function noteOf(d)
  local m = mk()
  return m.root + m.koma[d] * 12 / 53
end

-- the next degree for the phase: a step mostly, a leap sometimes
local function walk()
  local m = mk()
  local top = #m.koma
  local target
  if phase == 1 then target = math.random(1, m.guclu)
  elseif phase == 2 then target = m.guclu + math.random(-1, 1)
  elseif phase == 3 then target = math.random(m.guclu, top)
  else target = 1 end
  local d = deg
  if target > deg then d = deg + 1 elseif target < deg then d = deg - 1
  else d = deg + (math.random() < 0.5 and -1 or 1) end
  if math.random() < 0.12 then d = d + (math.random() < 0.5 and -2 or 2) end
  d = math.max(1, math.min(top, d))
  -- phase changes: by time in the phase and by reaching its goal
  steps = steps + 1
  if phase == 1 and (d >= m.guclu or steps > 10) then phase, steps = 2, 0
  elseif phase == 2 and steps > 6 then phase, steps = 3, 0
  elseif phase == 3 and steps > 8 then phase, steps = 4, 0
  elseif phase == 4 and d == 1 then phase, steps = 1, 0 end
  return d
end

function init()
  screen.fps(20)
  params:add_option("makam", "Makam", {"Rast", "Hicaz", "Ussak"}, 1, "SEYIR")
  params:add{id = "pace", name = "Pace", min = 0.25, max = 2, default = 1, unit = "x", page = "SEYIR"}
  params:add{id = "ornament", name = "Ornament", min = 0, max = 1, default = 0.35, page = "SEYIR"}
  params:add{id = "drone", name = "Drone", min = 0, max = 1, default = 0.5, page = "SEYIR",
             action = function(v) engine.set(3, "amp", v * 0.3); engine.set(4, "amp", v * 0.16) end}
  params:add{id = "breath", name = "Breath", min = 0, max = 1, default = 0.4, page = "TONE",
             action = function(v) engine.set(2, "amp", v * 0.12) end}
  params:add{id = "tone", name = "Tone", min = 400, max = 6000, default = 2200, unit = "Hz", page = "TONE",
             action = function(v) engine.set(1, "cutoff", v); engine.set(2, "cutoff", v * 1.5) end}
  params:add{id = "space", name = "Space", min = 0, max = 1, default = 0.6, page = "TONE",
             action = function(v) engine.fx("reverb_level", 0.15 + v * 0.5); engine.fx("reverb_size", 0.6 + v * 0.38) end}
  params:set_action("makam", function() deg, phase, steps = 1, 1, 0 end)
  -- where the melody is (0 durak .. 1 top) and the seyir phase, as mod sources
  mod.source("PITCH", {fn = function() return (deg - 1) / (#mk().koma - 1) end})
  mod.source("PHASE", {fn = function() return (phase - 1) / 3 end})
  -- 1 the ney-ish line, 2 its breath, 3-4 the drone (durak, and the fifth below)
  engine.voice(1, {wave = "tri", attack = 0.06, decay = 0.4, sustain = 0.7, release = 0.35, cutoff = 2200, res = 0.8,
                   glide = 0.05, amp = 0.5, send_reverb = 0.5, send_delay = 0.15})
  engine.voice(2, {wave = "noise", ftype = "bp", cutoff = 3300, res = 2, attack = 0.08, release = 0.3, amp = 0.05,
                   send_reverb = 0.5})
  engine.voice(3, {wave = "sine", attack = 2, sustain = 1, release = 3, amp = 0.15, send_reverb = 0.3})
  engine.voice(4, {wave = "fm", fm_ratio = 1.5, fm_index = 0.3, attack = 3, sustain = 1, release = 3, amp = 0.08,
                   send_reverb = 0.3})
  engine.fx("delay_time", 0.5)
  engine.fx("delay_feedback", 0.3)
  clock.run(function()
    local lastDrone = -1
    while true do
      if not playing then clock.sync(1 / 2)
      else
        local m = mk()
        if lastDrone ~= params:get("makam") then
          lastDrone = params:get("makam")
          engine.note(3, m.root - 12, 1)
          engine.note(4, m.root - 12 + m.koma[m.guclu] * 12 / 53 - 12, 1)
        end
        local nd = walk()
        -- an ornament: a quick upper neighbour before the note
        if math.random() < params:get("ornament") and nd < #m.koma then
          engine.note(1, noteOf(nd + 1), 0.6)
          clock.sync(1 / 16)
        end
        deg = nd
        engine.note(1, noteOf(deg), (deg == 1 or deg == m.guclu) and 0.9 or 0.7)
        engine.note(2, noteOf(deg) + 12, 0.5)
        path[#path + 1] = deg
        if #path > 48 then table.remove(path, 1) end
        -- durations: long on the durak and the guclu, the cadence slows down
        local d = ({1 / 4, 1 / 2, 1 / 2, 3 / 4, 1})[math.random(1, 5)]
        if deg == 1 or deg == m.guclu then d = d * 2 end
        if phase == 4 then d = d * 1.5 end
        clock.sync(d / params:get("pace"))
        engine.off(2)
        if phase == 1 and deg == 1 and steps == 0 then clock.sync(2) end   -- a breath at home
      end
    end
  end)
end

function pad(n, vel, z)
  local m = mk()
  local col, row = (n - 1) % 12 + 1, (n - 1) // 12 + 1
  if row == 4 and col <= #m.koma then
    if z == 1 then
      deg = col
      engine.note(1, noteOf(col), vel / 127)
      path[#path + 1] = deg
      if #path > 48 then table.remove(path, 1) end
    end
  elseif row == 3 and col <= 4 and z == 1 then
    phase, steps = col, 0   -- jump to a phase of the seyir
  end
end

function key(name, z)
  if z ~= 1 then return end
  if name == "INSERT" then
    playing = not playing
    if not playing then engine.off(1); engine.off(2) end
  end
  if name == "F1" then params:set("makam", 1) end
  if name == "F2" then params:set("makam", 2) end
  if name == "F3" then params:set("makam", 3) end
end

function enc(d, shift_)
  if shift_ then params:delta("ornament", d) else params:delta("pace", d) end
end

function redraw()
  local m = mk()
  local top = #m.koma
  screen.font("title")
  screen.color(0xE8D5A8)
  screen.text(30, 34, "SEYIR")
  screen.font("body")
  screen.color(0xFFB347)
  screen.text(180, 40, m.name)
  screen.font("small")
  screen.level(9)
  screen.text(300, 44, string.format("phase: %s   pace %.2fx   %s", PHASES[phase], params:get("pace"),
    playing and "playing" or "paused (Insert)"))
  -- the ladder: one line per degree, spaced by its koma
  local y0, y1 = 410, 80
  local function yOf(d) return y0 - (m.koma[d] / m.koma[top]) * (y0 - y1) end
  local lines = {}
  for d = 1, top do
    local y = yOf(d)
    lines[#lines + 1] = 170; lines[#lines + 1] = y; lines[#lines + 1] = 770; lines[#lines + 1] = y
  end
  screen.level(3)
  screen.lines(lines, 1)
  screen.font("tiny")
  for d = 1, top do
    local strong = d == 1 or d == m.guclu
    if strong then screen.color(0xFFB347) else screen.level(7) end
    screen.text_right(160, yOf(d) - 7, m.notes[d])
  end
  -- the path so far
  if #path > 1 then
    local pts = {}
    for i, d in ipairs(path) do
      pts[#pts + 1] = 180 + (i - 1) * 12.5
      pts[#pts + 1] = yOf(d)
    end
    screen.color(0x39E6B4)
    screen.polyline(pts, 2)
    screen.points(pts, 5)
    screen.color(0xFFFFFF)
    screen.circle(pts[#pts - 1], pts[#pts], 7)
  end
  -- the phase bar
  for p = 1, 4 do
    if p == phase then screen.color(0xFFB347) else screen.level(4) end
    screen.rect(180 + (p - 1) * 150, 420, 140, 6)
  end
  screen.level(5)
  screen.text(30, 440, "pads row 4: degrees   row 3: phase   Insert: play / pause   F1-F3: makam   enc: pace")
  -- pads: row 4 the degrees (durak and guclu amber), row 3 the phases
  for c = 1, 12 do
    if c <= top then
      if c == deg then grid.led(c, 4, 255, 255, 255)
      elseif c == 1 or c == m.guclu then grid.led(c, 4, 255, 150, 50)
      else grid.led(c, 4, 20, 80, 70) end
    else grid.led(c, 4, 0, 0, 0) end
    if c <= 4 then
      if c == phase then grid.led(c, 3, 255, 179, 71) else grid.led(c, 3, 30, 20, 10) end
    else grid.led(c, 3, 0, 0, 0) end
  end
end
