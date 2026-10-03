-- Toolgun tools on RDR2 entities (milestone 8), server side.
--
-- GMod's own tools work on the proxies as on any prop and make GMod's own constraints. Every
-- tick this lists the ones between proxies (or a proxy and the world) for RDR2
-- (rdr2/src/constraint_sync.h), which makes each in RDR2 and undoes it when it leaves the
-- list: undo, the remover and cleanup need nothing of their own.
--   weld       -> the entity attached to the other (or frozen, welded to the world)
--   no-collide -> the two pass through each other
--   rope       -> a real RDR2 rope between the same two points
--   duplicator -> a copied proxy pastes as a new RDR2 entity of the same model, where it is
--                 pasted, facing the way it faced (constraints between them are not pasted)
--   thruster   -> while a gmod_thruster welded to a proxy fires, GMod takes the proxy (as the
--                 physgun does), its own thruster moves it, and RDR2 follows. Pushing RDR2's
--                 entity directly did nothing: placed objects ignored forces and velocities.
--   balloon, wheel, hoist, winch, elastic, hydraulic, muscle
--              -> things only GMod can simulate. While one of them pulls on a proxy (a balloon
--                 or a turning wheel roped or axled to it, a hoist to the world), GMod takes
--                 the proxy, its own physics moves it, and RDR2 follows: a wagon rolls on
--                 GMod's wheels, a person or a horse rises under balloons or hangs from a
--                 hoist (as a ragdoll, world_sync.h). A plain rope on a person, horse or
--                 animal does the same, so they can be tied up, hung or dragged.
-- The remover deletes the real entity (spawn.lua). Proxies welded together are taken by the
-- physgun together (proxies.lua), and RDR2 moves the welded ones with the one held.

local GR = GR
GR.Tools = GR.Tools or {}
local Tools = GR.Tools

local enabled = CreateConVar("gr_tools", "1", FCVAR_ARCHIVE,
    "Garry's Redemption: 1 = welds, ropes, no-collides and thrusters on RDR2 entities happen in RDR2")

local CON_WELD, CON_NOCOLLIDE, CON_ROPE = 1, 2, 3

-- Each constraint entity (or thruster) gets an id for as long as it exists. Weak keys: a
-- removed entity's id goes with it, and ids are never handed out twice.
Tools.ids = Tools.ids or setmetatable({}, { __mode = "k" })
Tools.next_id = Tools.next_id or 1
Tools.listed = Tools.listed or 0

local function Id(ent)
    local id = Tools.ids[ent]
    if not id then
        id = Tools.next_id
        Tools.next_id = id + 1
        Tools.ids[ent] = id
    end
    return id
end

local function Handle(ent)
    if IsValid(ent) and ent:GetClass() == "gr_proxy" then return ent.gr_handle end
end

-- RDR2's ground in GMod (gr_chunk) counts as the world.
local function IsGround(ent)
    return IsValid(ent) and ent:GetClass() == "gr_chunk"
end

local function IsWorld(ent)
    return ent ~= nil and ent.IsWorld and (ent:IsWorld() or IsGround(ent))
end

local ZERO = Vector(0, 0, 0)

-- One GMod constraint, as GR.Native.Constraint arguments, or nothing if it does not join
-- two RDR2 entities (or one and the world).
local function Add(Native, con)
    local cent = con.Constraint
    local e1, e2 = con.Ent1, con.Ent2
    local h1, h2 = Handle(e1), Handle(e2)
    local p1, p2 = con.LPos1 or ZERO, con.LPos2 or ZERO
    if not h1 and h2 then
        -- The proxy first; for a rope the world's point goes second.
        e1, e2, h1, h2, p1, p2 = e2, e1, h2, h1, p2, p1
    end
    if not h1 then return end
    if IsGround(e2) then p2 = e2:LocalToWorld(p2) end  -- a rope's world end is a world position
    if not h2 and not IsWorld(e2) then return end  -- welded to a GMod prop (a thruster's body)
    local id = Id(cent)
    if con.Type == "Weld" then
        return Native.Constraint(id, CON_WELD, h1, h2 or 0, 0, 0, 0, 0, 0, 0, 0)
    elseif con.Type == "NoCollide" and h2 then
        return Native.Constraint(id, CON_NOCOLLIDE, h1, h2, 0, 0, 0, 0, 0, 0, 0)
    elseif con.Type == "Rope" then
        local length = (con.length or 0) + (con.addlength or 0)
        return Native.Constraint(id, CON_ROPE, h1, h2 or 0, p1.x, p1.y, p1.z, p2.x, p2.y, p2.z, length)
    end
end

-- The proxy a firing thruster is welded to: GMod's to move while it fires. Kept from being
-- given back (it would settle between two pushes) by marking it as just released.
local function Thruster(thr)
    if not thr:GetOn() then return end
    for _, con in ipairs(constraint.FindConstraints(thr, "Weld")) do
        local other = con.Ent1 == thr and con.Ent2 or con.Ent1
        if Handle(other) and GR.Proxies.Take then
            if not other.gr_owned then GR.Proxies.Take(other, false) end
            other.gr_released = CurTime()
            other.gr_slow = 0
            local phys = other:GetPhysicsObject()
            if IsValid(phys) then phys:Wake() end
            return
        end
    end
end

-- GMod keeps `proxy` for now (see Thruster).
local function Keep(proxy)
    if not GR.Proxies.Take then return end
    if not proxy.gr_owned then GR.Proxies.Take(proxy, false) end
    proxy.gr_released = CurTime()
    proxy.gr_slow = 0
    proxy.gr_apart = 0
    local phys = proxy:GetPhysicsObject()
    if IsValid(phys) and phys:IsMotionEnabled() then phys:Wake() end
end

-- Constraints that pull without holding rigid, and the ones among them whose length a key
-- changes (a hoist, a winch).
local PULLS = { Rope = true, Elastic = true, Winch = true, Hydraulic = true, Muscle = true, Pulley = true,
    Slider = true, Axis = true, Motor = true, Ballsocket = true, AdvBallsocket = true }
local LIFTS = { Elastic = true, Winch = true, Hydraulic = true, Muscle = true }

-- Whether `con` on `proxy` is something only GMod's physics can act out right now.
local function Pulled(proxy, con)
    if not PULLS[con.Type] then return false end
    local other = con.Ent1 == proxy and con.Ent2 or con.Ent1
    if IsWorld(other) or Handle(other) then
        -- A hoist to the world or to another RDR2 entity; or a rope on a living thing, which
        -- RDR2's own rope does not hold back while it walks.
        return LIFTS[con.Type] or (con.Type == "Rope" and (proxy.gr_kind or 0) <= 3)
    end
    -- One of GMod's own things on the other end: a balloon (never at rest), a wheel while it
    -- turns, a prop while it moves. At rest it pulls nothing and RDR2 keeps its entity.
    local phys = IsValid(other) and other:GetPhysicsObject()
    return IsValid(phys) and phys:IsMotionEnabled() and not phys:IsAsleep()
end

function Tools.Tick()
    local Native = GR.Native
    if not Native or not Native.Constraint then return end
    Native.ClearConstraints()
    if not enabled:GetBool() then return end
    local seen = {}
    local count = 0
    for _, proxy in pairs(GR.Proxies.by_handle) do
        if IsValid(proxy) and proxy.Constraints then
            local keep = false
            for _, con in ipairs(constraint.GetTable(proxy)) do
                keep = keep or Pulled(proxy, con)
                local cent = con.Constraint
                if IsValid(cent) and not seen[cent] then
                    seen[cent] = true
                    if Add(Native, con) then count = count + 1 end
                end
            end
            if keep then Keep(proxy) end
        end
    end
    for _, thr in ipairs(ents.FindByClass("gmod_thruster")) do Thruster(thr) end
    Tools.listed = count
end

-- The proxies welded to `ent`, and the ones welded to those (not ropes: a rope leaves them free).
function Tools.Welded(ent)
    local out, todo, seen = {}, { ent }, { [ent] = true }
    while #todo > 0 do
        local e = table.remove(todo)
        for _, con in ipairs(constraint.FindConstraints(e, "Weld")) do
            for _, other in ipairs({ con.Ent1, con.Ent2 }) do
                if IsValid(other) and not seen[other] and other:GetClass() == "gr_proxy" then
                    seen[other] = true
                    out[#out + 1] = other
                    todo[#todo + 1] = other
                end
            end
        end
    end
    return out
end

-- Pasting a proxy spawns its model in RDR2 (spawn.lua) and makes no GMod entity: the proxy
-- comes with the real one. The duplicator copies the proxy's Lua table, gr_model included.
duplicator.RegisterEntityClass("gr_proxy", function(ply, data)
    if not data.gr_model or not GR.Spawn or not GR.Spawn.At or not data.Pos then return end
    local facing = (data.Angle or angle_zero):Forward()
    facing.z = 0
    if facing:LengthSqr() < 1e-6 then facing = Vector(1, 0, 0) end
    -- Peds stand on what is under them; others are placed from slightly above it.
    GR.Spawn.At(ply, data.gr_model, string.format("RDR2 model %08X", data.gr_model), data.Pos + Vector(0, 0, 4),
        facing)
end, "Data")

concommand.Add("gr_tools_status", function(ply)
    if IsValid(ply) and not ply:IsListenServerHost() then return end
    GR.Print(string.format("tools: %s, %d constraints between RDR2 entities listed for RDR2",
        enabled:GetBool() and "on" or "off", Tools.listed))
end, nil, "Garry's Redemption: show the welds, ropes, no-collides and thrusters sent to RDR2")

hook.Add("Tick", "GR.Tools", Tools.Tick)
