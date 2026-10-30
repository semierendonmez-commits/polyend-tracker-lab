-- name: Pinball
-- author: PLATFORM
-- desc: A ball on a table of bumpers; every hit walks a Markov chain of notes
-- icon: spark

-- The table runs on the clock (four physics steps per 16th note), so every
-- hit lands on the grid. A hit advances an 8-state Markov chain over the
-- scale: the next degree is drawn from the current state's row of weights.
-- Pads, rows 1-2: teach / forget transitions; rows 3-4: place bumpers.

local W, H = 470, 360          -- the table, top-left at (X0, Y0)
local X0, Y0 = 24, 44
local BR = 7                   -- ball radius
local DEG = 8
local scaleNames = {"minor_pent", "dorian", "hijaz", "lydian", "whole"}

local ball = {x = 60, y = 40, vx = 3.2, vy = 0}
local bumper = {}              -- bumper[row][col] = flash level (0 = on, nil = none)
local M = {}                   -- M[i][j]: weight of the step from degree i to j
local state = 1
local trail = {}
local hits, lastName = 0, "-"

local function bumperPos(col, row)
  return X0 + 30 + (col - 1) * (W - 60) / 11, Y0 + 130 + (row - 1) * 110
end

local function resetChain()
  for i = 1, DEG do
    M[i] = {}
    for j = 1, DEG do
      local d = math.abs(i - j)
      M[i][j] = (d == 0) and 0.25 or 1 / d   -- steps are likelier than leaps
    end
  end
end

-- the next degree: the row's weights, flattened towards uniform by temperature
local function nextState()
  local temp = params:get("temperature")
  local row, total = M[state], 0
  for j = 1, DEG do total = total + row[j] * (1 - temp) + temp end
  local r = math.random() * total
  for j = 1, DEG do
    r = r - (row[j] * (1 - temp) + temp)
    if r <= 0 then state = j return j end
  end
  state = DEG
  return DEG
end

local function noteOf(deg)
  local s = music.scale(params:get("root"), scaleNames[params:get("scale")], DEG)
  return s[deg]
end

local function play(kind, speed)
  local n = noteOf(nextState())
  local vel = math.min(1, 0.45 + speed * 0.07)
  if kind == "bumper" then engine.note(1, n + 12, vel)
  elseif kind == "wall" then engine.note(2, n, vel * 0.8)
  else engine.note(3, n - 12, vel) end   -- the floor (the flipper)
  lastName = music.note_name(n)
  hits = hits + 1
end

local function launch()
  ball.x, ball.y = 40 + math.random() * (W - 80), 30
  ball.vx, ball.vy = (math.random() < 0.5 and -1 or 1) * (2.5 + math.random() * 2), 0
end

-- one physics step; returns the kind of the first thing hit (or nil) and the speed
local function step()
  local g, bounce, sp = params:get("gravity"), params:get("bounce"), params:get("speed")
  ball.vy = ball.vy + g * sp
  ball.x = ball.x + ball.vx * sp
  ball.y = ball.y + ball.vy * sp
  local what
  if ball.x < BR then ball.x = BR; ball.vx = math.abs(ball.vx) * bounce; what = "wall" end
  if ball.x > W - BR then ball.x = W - BR; ball.vx = -math.abs(ball.vx) * bounce; what = "wall" end
  if ball.y < BR then ball.y = BR; ball.vy = math.abs(ball.vy) * bounce; what = "wall" end
  if ball.y > H - BR then
    -- the flipper: always kicks the ball back up, a little sideways
    ball.y = H - BR
    ball.vy = -(5.5 + math.random() * 3)
    ball.vx = ball.vx + (math.random() - 0.5) * 2
    what = "floor"
  end
  for row = 1, 2 do
    for col = 1, 12 do
      if bumper[row][col] then
        local bx, by = bumperPos(col, row)
        bx, by = bx - X0, by - Y0
        local dx, dy = ball.x - bx, ball.y - by
        local d2, rr = dx * dx + dy * dy, BR + 11
        if d2 < rr * rr and d2 > 0.01 then
          local d = math.sqrt(d2)
          local nx, ny = dx / d, dy / d
          local dot = ball.vx * nx + ball.vy * ny
          if dot < 0 then
            ball.vx = (ball.vx - 2 * dot * nx) * 1.08   -- reflect, with a pop
            ball.vy = (ball.vy - 2 * dot * ny) * 1.08
          end
          ball.x, ball.y = bx + nx * rr, by + ny * rr
          bumper[row][col] = 8
          what = "bumper"
        end
      end
    end
  end
  local v = math.sqrt(ball.vx * ball.vx + ball.vy * ball.vy)
  if v > 10 then ball.vx, ball.vy, v = ball.vx * 10 / v, ball.vy * 10 / v, 10 end
  return what, v
end

function init()
  screen.fps(25)
  params:add{id = "gravity", name = "Gravity", min = 0.05, max = 0.6, default = 0.22, page = "TABLE"}
  params:add{id = "bounce", name = "Bounce", min = 0.5, max = 1, default = 0.92, page = "TABLE"}
  params:add{id = "speed", name = "Speed", min = 0.4, max = 2, default = 1, page = "TABLE"}
  params:add{id = "temperature", name = "Temperature", min = 0, max = 1, default = 0.2, page = "CHAIN"}
  params:add_number("root", "Root", 36, 60, 48, function(v) return music.note_name(v) end, nil, "CHAIN")
  params:add_option("scale", "Scale", {"minor pent", "dorian", "hijaz", "lydian", "whole"}, 1, "CHAIN")
  params:add{id = "decay", name = "Decay", min = 0.05, max = 1.5, default = 0.35, unit = "s", page = "SOUND",
             action = function(v) for i = 1, 3 do engine.set(i, "decay", v * (i == 3 and 1.6 or 1)) end end}
  params:add{id = "bright", name = "Brightness", min = 300, max = 8000, default = 2400, unit = "Hz", page = "SOUND",
             action = function(v) for i = 1, 3 do engine.set(i, "cutoff", v) end end}
  -- the ball as two modulation sources: route them on the MOD page (Shift + Master)
  mod.source("BALL_X", {fn = function() return ball.x / W end})
  mod.source("BALL_Y", {fn = function() return 1 - ball.y / H end})
  engine.voice(1, {wave = "fm", fm_ratio = 3.01, fm_index = 1.5, env_mode = "perc", decay = 0.35, amp = 0.5,
                   send_reverb = 0.35, send_delay = 0.2, pan = 0.3})
  engine.voice(2, {wave = "tri", env_mode = "perc", decay = 0.35, amp = 0.45, send_reverb = 0.3, pan = -0.3})
  engine.voice(3, {wave = "saw", env_mode = "perc", decay = 0.5, cutoff = 900, res = 2, fenv = 2, amp = 0.4})
  engine.fx("reverb_size", 0.7)
  engine.fx("delay_time", 0.375)
  resetChain()
  for row = 1, 2 do bumper[row] = {} end
  for _, c in ipairs({3, 6, 9}) do bumper[1][c] = 0 end
  for _, c in ipairs({2, 5, 8, 11}) do bumper[2][c] = 0 end
  clock.run(function()
    while true do
      clock.sync(1 / 16)
      local first, fv
      for _ = 1, 4 do
        local what, v = step()
        if what and not first then first, fv = what, v end
      end
      if first then play(first, fv) end
      trail[#trail + 1] = {ball.x, ball.y}
      if #trail > 24 then table.remove(trail, 1) end
    end
  end)
end

function pad(n, vel, z)
  if z ~= 1 then return end
  local col, row = (n - 1) % 12 + 1, (n - 1) // 12 + 1
  if row == 1 and col <= DEG then
    -- teach: the current state likes this degree more; play it now
    M[state][col] = M[state][col] + 1
    state = col
    engine.note(1, noteOf(col) + 12, vel / 127)
  elseif row == 2 and col <= DEG then
    for i = 1, DEG do M[i][col] = M[i][col] * 0.5 end   -- forget: every way into it halves
  elseif row == 1 and col == 12 then launch()
  elseif row == 2 and col == 12 then resetChain()
  elseif row >= 3 then
    local r = row - 2
    if bumper[r][col] then bumper[r][col] = nil else bumper[r][col] = 0 end
  end
end

function key(name, z)
  if z ~= 1 then return end
  if name == "INSERT" then launch() end
  if name == "DELETE" then resetChain() end
end

function enc(d) params:delta("temperature", d) end

function redraw()
  -- table
  screen.level(3)
  screen.frame(X0, Y0, W, H)
  -- bumpers: two batches of dots (lit ones bigger)
  local on, lit = {}, {}
  for row = 1, 2 do
    for col = 1, 12 do
      local f = bumper[row][col]
      if f then
        local x, y = bumperPos(col, row)
        local t = (f > 0) and lit or on
        t[#t + 1] = x
        t[#t + 1] = y
        if f > 0 then bumper[row][col] = f - 1 end
      end
    end
  end
  screen.color(0x3A6E8C)
  screen.points(on, 18)
  screen.color(0xFFB347)
  screen.points(lit, 26)
  -- trail and ball
  if #trail > 1 then
    local pts = {}
    for _, p in ipairs(trail) do pts[#pts + 1] = X0 + p[1]; pts[#pts + 1] = Y0 + p[2] end
    screen.color(0x1F7A63)
    screen.polyline(pts, 2)
  end
  screen.color(0x39E6B4)
  screen.circle(X0 + ball.x, Y0 + ball.y, BR)
  -- the chain: an 8 x 8 heat map, the current row framed
  local mx, my, cs = 540, 70, 26
  local bins = {{}, {}, {}, {}}
  for i = 1, DEG do
    local rowMax = 0
    for j = 1, DEG do rowMax = math.max(rowMax, M[i][j]) end
    for j = 1, DEG do
      local k = math.min(4, 1 + math.floor(M[i][j] / rowMax * 3.99))
      local b = bins[k]
      local x, y = mx + (j - 1) * cs, my + (i - 1) * cs
      b[#b + 1] = x; b[#b + 1] = y; b[#b + 1] = x + cs - 3; b[#b + 1] = y + cs - 3
    end
  end
  local shades = {0x14202A, 0x1E4A5C, 0x2F8FA0, 0x8FF0E0}
  for k = 1, 4 do screen.color(shades[k]); screen.rects(bins[k]) end
  screen.color(0xFFB347)
  screen.frame(mx - 2, my + (state - 1) * cs - 2, DEG * cs + 1, cs + 1)
  screen.font("small")
  screen.level(9)
  screen.text(mx, my - 26, "MARKOV  from -> to")
  screen.font("large")
  screen.color(0xE8ECF0)
  screen.text(mx, my + DEG * cs + 16, lastName)
  screen.font("small")
  screen.level(7)
  screen.text(mx + 90, my + DEG * cs + 22, string.format("hits %d   temp %.2f", hits, params:get("temperature")))
  screen.level(5)
  screen.font("tiny")
  screen.text(24, 432, "pads row 1: teach a degree   row 2: forget it   rows 3-4: bumpers   Insert: launch   Delete: reset chain")
  -- pads
  local row = M[state]
  local rowMax = 0
  for j = 1, DEG do rowMax = math.max(rowMax, row[j]) end
  for j = 1, 12 do
    if j <= DEG then
      local k = row[j] / rowMax
      if j == state then grid.led(j, 1, 255, 179, 71) else grid.led(j, 1, 20, math.floor(40 + 200 * k), math.floor(30 + 150 * k)) end
      grid.led(j, 2, 40, 18, 18)
    else
      grid.led(j, 1, 0, 0, 0)
      grid.led(j, 2, 0, 0, 0)
    end
    for r = 1, 2 do
      local f = bumper[r][j]
      if f then grid.led(j, r + 2, f > 0 and 255 or 40, f > 0 and 179 or 110, f > 0 and 71 or 140) else grid.led(j, r + 2, 4, 6, 8) end
    end
  end
  grid.led(12, 1, 57, 230, 180)
  grid.led(12, 2, 255, 77, 94)
end
