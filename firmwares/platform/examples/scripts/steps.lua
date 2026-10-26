-- name: Steps
-- author: PLATFORM
-- desc: 4-track, 12-step sequencer on the pads: kick, snare, hat, bass
-- icon: seq

local STEPS = 12
local tracks = {
  {name = "KICK",  pat = {1,0,0,0,0,0,1,0,0,0,0,0}, color = {255, 90, 60}},
  {name = "SNARE", pat = {0,0,0,1,0,0,0,0,0,1,0,0}, color = {255, 200, 60}},
  {name = "HAT",   pat = {1,1,1,1,1,1,1,1,1,1,1,1}, color = {80, 220, 255}},
  {name = "BASS",  pat = {1,0,1,0,0,1,0,1,0,0,1,0}, color = {170, 120, 255}},
}
local pos = 0
local sel = 1
local bassNotes = {0, 0, 3, 0, 0, 7, 0, 5, 0, 0, 10, 0}

function init()
  engine.voice(1, {wave = "sine", env_mode = "perc", decay = 0.35, pitch_env = 24, pitch_decay = 0.04, amp = 0.9, drive = 0.2})
  engine.voice(2, {wave = "noise", env_mode = "perc", decay = 0.16, ftype = "bp", cutoff = 1800, res = 1.2, amp = 0.6, send_reverb = 0.25})
  engine.voice(3, {wave = "noise", env_mode = "perc", decay = 0.04, ftype = "hp", cutoff = 7000, amp = 0.25})
  engine.voice(4, {wave = "saw", env_mode = "perc", decay = 0.22, cutoff = 400, res = 3, fenv = 3, amp = 0.45, send_delay = 0.2})
  params:add_number("root", "bass root", 24, 48, 33, function(v) return music.note_name(v) end)
  params:add_control("swing", "swing", 0, 0.4, 0, 0.01)
  params:add_control("hat_level", "hat level", 0, 1, 0.25, 0.01)
  params:set_action("hat_level", function(v) engine.set(3, "amp", v) end)
  params:add_toggle("run_stopped", "run when stopped", false)
  clock.run(function()
    while true do
      local swing = params:get("swing")
      clock.sync(1 / 4, (pos % 2 == 1) and swing / 4 or 0)
      if clock.playing() or params:get("run_stopped") then
        pos = pos % STEPS + 1
        for i, t in ipairs(tracks) do
          if t.pat[pos] == 1 then
            if i == 4 then
              engine.note(4, params:get("root") + bassNotes[pos], 0.9)
            else
              engine.note(i, i == 1 and 36 or 60, i == 3 and (pos % 2 == 1 and 1 or 0.6) or 1)
            end
            if i == 1 then midi.note_on(36, 110, 10) end
          end
        end
        screen.dirty()
      end
    end
  end)
end

function clock.transport.start() pos = 0 end
function clock.transport.stop() pos = 0 end

function pad(n, vel, z)
  if z == 0 then return end
  local row, col = (n - 1) // 12 + 1, (n - 1) % 12 + 1
  local t = tracks[row]
  t.pat[col] = 1 - t.pat[col]
  sel = row
end

function enc(d, shift)
  if shift then
    params:delta("swing", d)
  else
    clock.set_tempo(clock.get_tempo() + d)
  end
end

function key(name, z)
  if z ~= 1 then return end
  if name == "F1" or name == "F2" or name == "F3" or name == "F4" then sel = tonumber(name:sub(2)) end
  if name == "DELETE" then for i = 1, STEPS do tracks[sel].pat[i] = 0 end end
  if name == "ENC" then if clock.playing() then clock.stop() else clock.start() end end
end

function redraw()
  screen.font("title")
  screen.color(0xE8ECF0)
  screen.text(30, 40, "STEPS")
  screen.font("small")
  screen.level(9)
  screen.text(150, 46, string.format("%.0f BPM   swing %.0f%%   %s", clock.get_tempo(), params:get("swing") * 100,
    clock.playing() and "PLAYING" or "press Play"))
  for r, t in ipairs(tracks) do
    local y = 80 + (r - 1) * 86
    screen.color(t.color[1], t.color[2], t.color[3])
    screen.font("body")
    screen.text(30, y + 34, t.name)
    if r == sel then screen.rect(18, y + 8, 4, 50) end
    for c = 1, STEPS do
      local x = 130 + (c - 1) * 54
      if t.pat[c] == 1 then
        screen.color(t.color[1], t.color[2], t.color[3])
        screen.rect(x, y + 10, 46, 46)
      else
        screen.level(3)
        screen.frame(x, y + 10, 46, 46)
      end
      if c == pos then
        screen.color(0xFFFFFF)
        screen.rect(x, y + 60, 46, 4)
      end
    end
  end
  screen.level(6)
  screen.font("tiny")
  screen.text(30, 456, "pads: toggle steps   enc: tempo (shift: swing)   push: play/stop   F1-F4: track   Delete: clear track")
  -- pad LEDs
  for r, t in ipairs(tracks) do
    for c = 1, STEPS do
      local on = t.pat[c] == 1
      local k = (c == pos) and 1 or (on and 0.5 or 0.04)
      grid.led(c, r, math.floor(t.color[1] * k), math.floor(t.color[2] * k), math.floor(t.color[3] * k))
    end
  end
end
