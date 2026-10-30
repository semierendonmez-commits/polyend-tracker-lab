-- name: Euclid
-- author: PLATFORM
-- desc: Euclidean drum machine: 4 rings, hits / steps / rotation per track
-- icon: euclid

local tr = {
  {steps = 16, hits = 4, rot = 0, note = 36, c = {255, 80, 70}},
  {steps = 12, hits = 3, rot = 2, note = 50, c = {255, 190, 60}},
  {steps = 16, hits = 7, rot = 0, note = 70, c = {70, 220, 255}},
  {steps = 9,  hits = 4, rot = 1, note = 41, c = {180, 110, 255}},
}
local sel, field = 1, 1   -- field: 1 hits 2 steps 3 rotate
local tick = 0
local pat = {}

local function rebuild(i)
  local t = tr[i]
  pat[i] = music.euclid(t.steps, t.hits, t.rot)
end

function init()
  engine.voice(1, {wave = "sine", env_mode = "perc", decay = 0.4, pitch_env = 30, pitch_decay = 0.03, amp = 0.9})
  engine.voice(2, {wave = "noise", env_mode = "perc", decay = 0.18, ftype = "bp", cutoff = 2200, res = 1.5, amp = 0.55, send_reverb = 0.3})
  engine.voice(3, {wave = "click", env_mode = "perc", decay = 0.05, ftype = "hp", cutoff = 6000, amp = 0.5, send_delay = 0.3})
  engine.voice(4, {wave = "fm", env_mode = "perc", decay = 0.25, fm_ratio = 3.5, fm_index = 4, amp = 0.35, send_delay = 0.2})
  engine.fx("delay_time", 0.33)
  for i = 1, 4 do rebuild(i) end
  params:add_toggle("follow", "only while playing", false, "EUCLID")
  params:add{id = "accent", name = "accent", min = 0.3, max = 1, default = 0.75, page = "EUCLID"}
  clock.run(function()
    while true do
      clock.sync(1 / 4)
      if clock.playing() or not params:get("follow") then
        tick = tick + 1
        for i, t in ipairs(tr) do
          local s = (tick - 1) % t.steps + 1
          if pat[i][s] then engine.note(i, t.note, (s == 1) and 1 or params:get("accent")) end
        end
        screen.dirty()
      end
    end
  end)
end

function clock.transport.start() tick = 0 end

function pad(n, vel, z)
  if z == 0 then return end
  local row, col = (n - 1) // 12 + 1, (n - 1) % 12 + 1
  sel = row
  tr[row].hits = math.min(col, tr[row].steps)
  rebuild(row)
end

function enc(d, shift)
  local t = tr[sel]
  if field == 1 then t.hits = util.clamp(t.hits + d, 0, t.steps)
  elseif field == 2 then t.steps = util.clamp(t.steps + d, 1, 32); t.hits = math.min(t.hits, t.steps)
  else t.rot = (t.rot + d) % t.steps end
  rebuild(sel)
end

function key(name, z)
  if z ~= 1 then return end
  if name == "UP" then sel = (sel - 2) % 4 + 1 end
  if name == "DOWN" then sel = sel % 4 + 1 end
  if name == "LEFT" then field = (field - 2) % 3 + 1 end
  if name == "RIGHT" then field = field % 3 + 1 end
  if name == "F1" then field = 1 elseif name == "F2" then field = 2 elseif name == "F3" then field = 3 end
end

function redraw()
  screen.font("title")
  screen.level(12)
  screen.text(24, 36, "EUCLID")
  local fields = {"hits", "steps", "rotate"}
  for i, t in ipairs(tr) do
    local cx, cy = 110 + (i - 1) * 190, 220
    local s = (tick - 1) % t.steps + 1
    screen.color(t.c[1] // 4, t.c[2] // 4, t.c[3] // 4)
    screen.ring(cx, cy, 70, 40)
    local on, off = {}, {}
    for k = 1, t.steps do
      local a = (k - 1) / t.steps * 2 * math.pi - math.pi / 2
      local list = pat[i][k] and on or off
      list[#list + 1] = cx + math.cos(a) * 70
      list[#list + 1] = cy + math.sin(a) * 70
    end
    screen.color(t.c[1], t.c[2], t.c[3])
    screen.points(on, 7)
    screen.level(4)
    screen.points(off, 3)
    local a = (s - 1) / t.steps * 2 * math.pi - math.pi / 2
    screen.color(0xFFFFFF)
    screen.line(cx, cy, cx + math.cos(a) * 58, cy + math.sin(a) * 58)
    screen.font("body")
    screen.color(t.c[1], t.c[2], t.c[3])
    screen.text_center(cx, 330, string.format("%d / %d  r%d", t.hits, t.steps, t.rot))
    if i == sel then
      screen.rect(cx - 70, 350, 140, 4)
      screen.font("small")
      screen.level(12)
      screen.text_center(cx, 372, "enc: " .. fields[field])
    end
  end
  screen.font("tiny")
  screen.level(6)
  screen.text(24, 456, "pads: row = track, column = hits   Up/Down: track   Left/Right or F1-F3: field   enc: change")
  for i, t in ipairs(tr) do
    local s = (tick - 1) % t.steps + 1
    for c = 1, 12 do
      local k = 0.04
      if c <= t.hits then k = 0.35 end
      if c == ((s - 1) % 12) + 1 and pat[i][s] then k = 1 end
      grid.led(c, i, math.floor(t.c[1] * k), math.floor(t.c[2] * k), math.floor(t.c[3] * k))
    end
  end
end
