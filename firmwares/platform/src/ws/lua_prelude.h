// PLATFORM -- the Lua side of the script API (params, metro, util, music),
// loaded into every script state before the script itself (lua_host.cpp).
#pragma once

namespace plat {

constexpr const char kLuaPrelude[] = R"LUA(
local _plat = {}
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
-- params (LABKIT registry: PARAM pads, MAP, macros, mod routes, MIDI learn) ----
local P = {list = {}, by = {}}
params = P
local reg_add, reg_get, reg_set, reg_str, reg_nudge = _plat_param_add, _plat_param_get, _plat_param_set,
  _plat_param_string, _plat_param_nudge
local function addp(p)
  assert(type(p.id) == "string" and #p.id > 0, "params: id must be a string")
  if P.by[p.id] then error("params: duplicate id '" .. p.id .. "'", 3) end
  p.name = p.name or p.id
  if p.kind ~= "trigger" and p.kind ~= "separator" then
    p.rid = reg_add(p)            -- the registry param (error when full)
  end
  P.list[#P.list + 1] = p
  P.by[p.id] = p
  p.value = nil                   -- actions run once after init (bang)
  return p
end
function P:add_number(id, name, min, max, default, formatter, unit, page)
  return addp{id = id, name = name, kind = "number", min = min or 0, max = max or 127,
              default = default or min or 0, formatter = formatter, unit = unit, page = page}
end
function P:add_option(id, name, options, default, page)
  assert(type(options) == "table" and #options > 0, "params: options must be a non-empty list")
  return addp{id = id, name = name, kind = "option", options = options, min = 1, max = #options,
              default = default or 1, page = page}
end
function P:add_control(id, name, min, max, default, step, unit, page)
  return addp{id = id, name = name, kind = "control", min = min or 0, max = max or 1,
              default = default or min or 0, step = step, unit = unit, page = page}
end
function P:add_toggle(id, name, default, page)
  return addp{id = id, name = name, kind = "option", options = {"off", "on"}, min = 1, max = 2,
              default = default and 2 or 1, toggle = true, page = page}
end
function P:add_trigger(id, name) return addp{id = id, name = name, kind = "trigger"} end
function P:add_separator(name) return addp{id = "__sep" .. (#P.list + 1), name = name or "", kind = "separator"} end
-- norns-style: params:add{id=, name=, min=, max=, default=, unit=, page=, type=, step=, options=, action=, formatter=}
function P:add(t)
  local k = t.type or (t.options and "option") or "control"
  local p
  if k == "option" then p = P:add_option(t.id, t.name, t.options, t.default, t.page)
  elseif k == "toggle" then p = P:add_toggle(t.id, t.name, t.default, t.page)
  elseif k == "trigger" then p = P:add_trigger(t.id, t.name)
  elseif k == "separator" then p = P:add_separator(t.name)
  elseif k == "number" then p = P:add_number(t.id, t.name, t.min, t.max, t.default, t.formatter, t.unit, t.page)
  else p = P:add_control(t.id, t.name, t.min, t.max, t.default, t.step, t.unit, t.page) end
  p.action = t.action
  if t.formatter then p.formatter = t.formatter end
  return p
end
local function getp(id)
  local p = P.by[id]
  if not p then error("params: no parameter '" .. tostring(id) .. "'", 3) end
  return p
end
local function out(p, v)
  if p.toggle then return v == 2 end
  return v
end
-- the value with modulation (what the sound should use)
function P:get(id)
  local p = getp(id)
  if not p.rid then return nil end
  return out(p, reg_get(p.rid))
end
-- the value the user set (without modulation)
function P:get_base(id) local p = getp(id) return p.rid and out(p, reg_get(p.rid, true)) end
function P:set(id, v, silent)
  local p = getp(id)
  if p.kind == "trigger" then if p.action and not silent then p.action() end return end
  if not p.rid then return end
  if type(v) == "boolean" then v = v and 2 or 1 end
  reg_set(p.rid, v)
  if silent then p.value = reg_get(p.rid) end
end
function P:delta(id, d)
  local p = getp(id)
  if p.kind == "trigger" then if d ~= 0 and p.action then p.action() end return end
  if p.rid then reg_nudge(p.rid, d) end
end
function P:string(id)
  local p = getp(id)
  if not p.rid then return p.kind == "trigger" and "press" or "" end
  if p.formatter then return p.formatter(out(p, reg_get(p.rid))) end
  return reg_str(p.rid)
end
function P:set_action(id, f) getp(id).action = f end
function P:lookup(id) return P.by[id] end
function P:bang()
  for _, p in ipairs(P.list) do
    if p.rid then
      p.value = reg_get(p.rid)
      if p.action then p.action(out(p, p.value)) end
    end
  end
end
-- base values + macros + routes + MIDI learn of this script (Data/<script>/labkit.lkc)
function P:write() return _plat_kit_save() end
function P:read() return _plat_kit_load() end
-- every frame: values changed by pads, MAP, macros, routes, MIDI -> actions
local sources = {}
function _plat.poll()
  for _, s in ipairs(sources) do
    if s.fn then
      local ok, v = pcall(s.fn)
      if ok and type(v) == "number" then _plat_mod_set(s.slot, v) end
    end
  end
  for _, p in ipairs(P.list) do
    if p.rid then
      local v = reg_get(p.rid)
      if v ~= p.value then
        p.value = v
        if p.action then p.action(out(p, v)) end
      end
    end
  end
end
function _plat.after_init() _plat_kit_load(); P:bang() end
function _plat.save() _plat_kit_save() end
-- mod: sources the script feeds, routes, macros -----------------------------------
mod = {}
-- mod.source("ENV" [, {bipolar = true, fn = function() return x end}]) -> source;
-- source:set(v) with 0..1 (or -1..1 when bipolar), or fn is polled every frame
function mod.source(name, opt)
  opt = opt or {}
  local slot, id = _plat_mod_source(name, opt.bipolar and true or false)
  local s = {name = name, slot = slot, id = id, fn = opt.fn}
  function s:set(v) _plat_mod_set(self.slot, v) end
  sources[#sources + 1] = s
  return s
end
function mod.set(s, v) _plat_mod_set(s.slot, v) end
local function srcid(s)
  if type(s) == "table" then return s.id end
  local id = _plat_mod_find(s)
  if not id then error("mod: no source '" .. tostring(s) .. "'", 3) end
  return id
end
local function parid(p)
  local q = P.by[p]
  if not q or not q.rid then error("mod: no parameter '" .. tostring(p) .. "'", 3) end
  return q.rid
end
-- source: a mod.source, or "LFO1".."LFO4", "RND1", "RND2", "M1".."M8"; depth -1..1
function mod.route(src, param, depth) return _plat_mod_route(srcid(src), parid(param), depth or 0.5) end
function mod.unroute(src, param) return _plat_mod_unroute(srcid(src), parid(param)) end
function mod.clear() _plat_mod_clear() end
function mod.routes() return _plat_mod_count() end
-- macros 1..8: value 0..1 (get, or set when v is given), names, targets
function mod.macro(i, v) if v ~= nil then _plat_macro_set(i, v) end return _plat_macro_get(i) end
function mod.macro_name(i, name) _plat_macro_name(i, name) end
function mod.macro_target(i, param, depth) return _plat_mod_route(srcid("M" .. i), parid(param), depth or 1.0) end
-- LFOs 1..4: field "shape" (sine tri saw square sh smooth), "rate" (Hz), "sync" (0 free, 1..), "phase"
function mod.lfo(i, field, v) return _plat_lfo(i, field, v) end
return _plat
)LUA";

}  // namespace plat
