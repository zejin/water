--
-- Basic tests
--
nx = tonumber(args[2]) or 200
t = tostring(args[3]) or "1"
s = tostring(args[4]) or "weak"

pond = {
  init = function(x,y) return 1, 0, 0 end,
  out = "pond.out",
  nx = nx
}

river = {
  init = function(x,y) return 1, 1, 0 end,
  out = "river.out",
  nx = nx
}

dam = {
  init = function(x,y)
    if (x-1)*(x-1) + (y-1)*(y-1) < 0.25 then
      return 1.5, 0, 0
    else
      return 1, 0, 0
    end
  end,
  out = "dam_break.out",
  nx = nx,
  t = t,
  s = s
}

wave = {
  init = function(x,y)
    return 1.0 + 0.2 * math.sin(math.pi * x), 1, 0
  end,
  out = "wave.out",
  frames = 100,
  nx = nx
}

simulate(_G[args[1]])
