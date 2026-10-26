// PLATFORM -- the Lua side of the script API (params, metro, util, music),
// loaded into every script state before the script itself (lua_host.cpp).
#pragma once

namespace plat {

constexpr const char kLuaPrelude[] = R"LUA(
local _plat = {}
-- params -------------------------------------------------------------------
local P = {list = {}, by = {}}
params = P
local function addp(p)
  assert(type(p.id) == "string", "params: id must be a string")
  if P.by[p.id] then error("params: duplicate id '" .. p.id .. "'", 3) end
  p.name = p.name or p.id
  P.list[#P.list + 1] = p
  P.by[p.id] = p
  p.value = p.default
  return p
end
function P:add_number(id, name, min, max, default, formatter)
  return addp{id = id, name = name, kind = "number", min = min or 0, max = max or 127,
              default = default or min or 0, step = 1, formatter = formatter}
end
function P:add_option(id, name, options, default)
  assert(type(options) == "table" and #options > 0, "params: options must be a non-empty list")
  return addp{id = id, name = name, kind = "option", options = options, min = 1, max = #options,
              default = default or 1, step = 1}
end
function P:add_control(id, name, min, max, default, step, units)
  return addp{id = id, name = name, kind = "control", min = min or 0, max = max or 1,
              default = default or min or 0, step = step or ((max or 1) - (min or 0)) / 100, units = units or ""}
end
function P:add_toggle(id, name, default)
  return addp{id = id, name = name, kind = "option", options = {"off", "on"}, min = 1, max = 2,
              default = default and 2 or 1, step = 1, toggle = true}
end
function P:add_trigger(id, name)
  return addp{id = id, name = name, kind = "trigger", min = 0, max = 0, default = 0, step = 0}
end
function P:add_separator(name)
  return addp{id = "__sep" .. (#P.list + 1), name = name or "", kind = "separator", min = 0, max = 0, default = 0}
end
function P:add(t)
  local k = t.type or "number"
  local p
  if k == "option" then p = P:add_option(t.id, t.name, t.options, t.default)
  elseif k == "control" then p = P:add_control(t.id, t.name, t.min, t.max, t.default, t.step, t.units)
  elseif k == "toggle" then p = P:add_toggle(t.id, t.name, t.default)
  elseif k == "trigger" then p = P:add_trigger(t.id, t.name)
  elseif k == "separator" then p = P:add_separator(t.name)
  else p = P:add_number(t.id, t.name, t.min, t.max, t.default, t.formatter) end
  p.action = t.action
  return p
end
local function getp(id)
  local p = P.by[id]
  if not p then error("params: no parameter '" .. tostring(id) .. "'", 3) end
  return p
end
local function clampv(p, v)
  if p.kind == "trigger" or p.kind == "separator" then return 0 end
  if v < p.min then v = p.min end
  if v > p.max then v = p.max end
  if p.kind ~= "control" then v = math.floor(v + 0.5) end
  return v
end
function P:set(id, v, silent)
  local p = getp(id)
  if p.kind == "trigger" then if p.action and not silent then p.action() end return end
  v = clampv(p, v)
  local changed = v ~= p.value
  p.value = v
  if p.action and not silent and changed then p.action(p.toggle and (v == 2) or v) end
end
function P:get(id)
  local p = getp(id)
  if p.toggle then return p.value == 2 end
  return p.value
end
function P:string(id)
  local p = getp(id)
  if p.kind == "option" then return tostring(p.options[p.value]) end
  if p.kind == "trigger" then return "press" end
  if p.kind == "separator" then return "" end
  if p.formatter then return p.formatter(p.value) end
  if p.kind == "control" then return string.format("%.3g%s", p.value, p.units or "") end
  return tostring(math.floor(p.value))
end
function P:delta(id, d)
  local p = getp(id)
  if p.kind == "trigger" then if d ~= 0 and p.action then p.action() end return end
  P:set(id, p.value + d * (p.step or 1))
end
function P:set_action(id, f) getp(id).action = f end
function P:lookup(id) return P.by[id] end
function P:bang()
  for _, p in ipairs(P.list) do
    if p.action and p.kind ~= "trigger" and p.kind ~= "separator" then p.action(p.toggle and (p.value == 2) or p.value) end
  end
end
function P:write()
  local out = {}
  for _, p in ipairs(P.list) do
    if p.kind ~= "trigger" and p.kind ~= "separator" then out[#out + 1] = p.id .. "=" .. tostring(p.value) end
  end
  return file.write("params.txt", table.concat(out, "\n") .. "\n")
end
function P:read()
  local s = file.read("params.txt")
  if not s then return false end
  for id, v in s:gmatch("([%w_%-%.]+)=([^\n]*)") do
    local p = P.by[id]
    local n = tonumber(v)
    if p and n then p.value = clampv(p, n) end
  end
  return true
end
function _plat.param_count() return #P.list end
function _plat.param_row(i)
  local p = P.list[i]
  if not p then return "", "" end
  return p.name, P:string(p.id), p.kind
end
function _plat.param_delta(i, d)
  local p = P.list[i]
  if p and p.kind ~= "separator" then P:delta(p.id, d) end
end
function _plat.after_init() P:read(); P:bang() end
function _plat.save() if #P.list > 0 then P:write() end end
-- metro (on the clock) -----------------------------------------------------
metro = {}
function metro.init(f, time, count)
  local m = {event = f, time = time or 1, count = count or -1, running = false}
  function m:start(t, c)
    if t then self.time = t end
    if c then self.count = c end
    if self.id then clock.cancel(self.id) end
    self.running = true
    self.id = clock.run(function()
      local n = 0
      while self.count < 0 or n < self.count do
        clock.sleep(self.time)
        n = n + 1
        if self.event then self.event(n) end
      end
      self.running = false
    end)
  end
  function m:stop()
    if self.id then clock.cancel(self.id) end
    self.id = nil
    self.running = false
  end
  return m
end
-- util ----------------------------------------------------------------------
util = {}
function util.clamp(x, a, b) if x < a then return a elseif x > b then return b end return x end
function util.linlin(a0, a1, b0, b1, x) return b0 + (x - a0) * (b1 - b0) / (a1 - a0) end
function util.round(x, q) q = q or 1 return math.floor(x / q + 0.5) * q end
function util.wrap(x, a, b) local r = b - a + 1 return ((x - a) % r) + a end
function util.time() return _plat_millis() / 1000 end
-- music ---------------------------------------------------------------------
music = {}
music.scales = {
  major = {0, 2, 4, 5, 7, 9, 11}, minor = {0, 2, 3, 5, 7, 8, 10}, dorian = {0, 2, 3, 5, 7, 9, 10},
  phrygian = {0, 1, 3, 5, 7, 8, 10}, lydian = {0, 2, 4, 6, 7, 9, 11}, mixolydian = {0, 2, 4, 5, 7, 9, 10},
  pentatonic = {0, 2, 4, 7, 9}, minor_pent = {0, 3, 5, 7, 10}, whole = {0, 2, 4, 6, 8, 10},
  chromatic = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, hijaz = {0, 1, 4, 5, 7, 8, 10},
}
function music.scale(root, name, count)
  local s = music.scales[name or "major"] or music.scales.major
  local out = {}
  count = count or #s
  for i = 0, count - 1 do
    out[#out + 1] = root + s[(i % #s) + 1] + 12 * (i // #s)
  end
  return out
end
function music.note_to_hz(n) return 440 * 2 ^ ((n - 69) / 12) end
local names = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}
function music.note_name(n) n = math.floor(n) return names[n % 12 + 1] .. tostring(n // 12 - 1) end
function music.euclid(steps, hits, rotate)
  local t = {}
  rotate = rotate or 0
  for i = 0, steps - 1 do
    local k = (i + rotate) % steps
    t[i + 1] = hits > 0 and ((k * hits) % steps) < hits
  end
  return t
end
return _plat
)LUA";

}  // namespace plat
