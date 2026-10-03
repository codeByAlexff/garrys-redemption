-- RDR2's trees, walls and rocks in the way of GMod's fast things (server side).
--
-- GMod's copy of RDR2's world is the ground and the map's walls near the player (terrain.lua)
-- and a ring of panels right around it (near.lua): grenades, the AR2's energy ball, rockets,
-- crossbow bolts and thrown props flew through every tree and house beyond that (user: "I
-- want the weapons to be able to collide with objects like trees, houses"). Each tick the
-- fastest-moving of them near the player list the stretch they are about to cover; RDR2 tests
-- it against its world (rdr2/src/probe_sync.h) and answers with the first surface, and a small
-- solid square (gr_surface) is stood on it there, just in time to be hit.
--
-- Bullets are not here: they are instant. weapons.lua sends each one that hit none of RDR2's
-- people to RDR2 as a harmless bullet, which RDR2 stops at its own trees and walls.

local GR = GR
GR.Surfaces = GR.Surfaces or {}
local Surfaces = GR.Surfaces

local enabled = CreateConVar("gr_surfaces", "1", FCVAR_ARCHIVE,
    "Garry's Redemption: 1 = GMod's grenades, balls, rockets and thrown things hit RDR2's trees, walls and rocks")

local MAX_PROBES = 16      -- GR_MAX_PROBES
local MIN_SPEED = 150      -- units/s: slower things are rolling or lying, the chunks hold them
local LOOKAHEAD = 0.25     -- s of travel tested ahead: the answer comes a tick or two later
local MIN_REACH = 48       -- units
local KEEP = 3             -- s a square stays after it was last asked for
local SAME_PLACE = 20      -- units: a hit this close to a standing square is that square
local POOL = 32
local FAR = 6000           -- units from the player: further things are not probed
-- What flies. Classes with a * are patterns.
local CLASSES = {
    "prop_combine_ball", "npc_grenade_frag", "grenade_ar2", "rpg_missile", "crossbow_bolt", "npc_satchel",
    "prop_physics*", "prop_ragdoll", "prop_vehicle_*", "gr_proxy", "gmod_*", "sent_*",
}

-- Matched by the patterns above but not things that fly: they follow the player.
local SKIP = { gr_surface = true, gmod_hands = true, prop_vehicle_prisoner_pod = true }

Surfaces.pool = Surfaces.pool or {}  -- { ent, until_t }
Surfaces.made = Surfaces.made or 0

local function Hide(slot)
    local ent = slot.ent
    if not IsValid(ent) then return end
    ent:SetNotSolid(true)
    local phys = ent:GetPhysicsObject()
    if IsValid(phys) then phys:EnableCollisions(false) end
    slot.until_t = nil
end

local function Place(pos, normal)
    local now = CurTime()
    local free, oldest
    for _, slot in ipairs(Surfaces.pool) do
        if not IsValid(slot.ent) then
            slot.ent = nil
            free = free or slot
        elseif slot.until_t then
            if slot.pos:DistToSqr(pos) < SAME_PLACE * SAME_PLACE then
                slot.until_t = now + KEEP
                return
            end
            if not oldest or slot.until_t < oldest.until_t then oldest = slot end
        else
            free = free or slot
        end
    end
    if not free and #Surfaces.pool < POOL then
        free = {}
        Surfaces.pool[#Surfaces.pool + 1] = free
    end
    local slot = free or oldest
    if not IsValid(slot.ent) then
        local ent = ents.Create("gr_surface")
        if not IsValid(ent) then return end
        ent:Spawn()
        slot.ent = ent
    end
    local ent = slot.ent
    ent:SetPos(pos)
    ent:SetAngles(normal:Angle())
    ent:SetNotSolid(false)
    local phys = ent:GetPhysicsObject()
    if IsValid(phys) then
        phys:EnableCollisions(true)
        phys:SetPos(pos)
        phys:SetAngles(normal:Angle())
        phys:EnableMotion(false)
    end
    slot.pos = pos
    slot.until_t = now + KEEP
    Surfaces.made = Surfaces.made + 1
end

local function Moving(ply)
    local list = {}
    local eye = ply:GetPos()
    for _, pattern in ipairs(CLASSES) do
        for _, ent in ipairs(ents.FindByClass(pattern)) do
            if not ent:IsPlayer() and not ent:IsWeapon() and not SKIP[ent:GetClass()] then
                local vel = ent:GetVelocity()
                local speed = vel:Length()
                if speed >= MIN_SPEED then
                    local d = ent:GetPos():DistToSqr(eye)
                    if d < FAR * FAR then list[#list + 1] = { ent = ent, vel = vel, speed = speed, d = d } end
                end
            end
        end
    end
    -- Nearest first: those are the ones the player sees.
    table.sort(list, function(a, b) return a.d < b.d end)
    return list
end

local last_origin

hook.Add("Tick", "GR.Surfaces", function()
    local Native = GR.Native
    if not Native or not Native.Probe then return end
    Native.ClearProbes()
    -- The origin moved: every square is somewhere else now.
    local origin = Native.OriginSerial and Native.OriginSerial()
    if origin ~= last_origin then
        last_origin = origin
        for _, slot in ipairs(Surfaces.pool) do Hide(slot) end
    end
    local now = CurTime()
    for _, slot in ipairs(Surfaces.pool) do
        if slot.until_t and now > slot.until_t then Hide(slot) end
    end
    local ply = player.GetAll()[1]
    if not enabled:GetBool() or not IsValid(ply) or not Native.Anchored() then return end

    local list = Moving(ply)
    for i = 1, math.min(#list, MAX_PROBES) do
        local m = list[i]
        local id = m.ent:EntIndex()
        -- The answer to last tick's question about this one.
        local flags, x, y, z, nx, ny, nz = Native.ProbeHit(id)
        if flags then Place(Vector(x, y, z), Vector(nx, ny, nz)) end
        local from = m.ent:WorldSpaceCenter()
        local reach = math.max(m.speed * LOOKAHEAD, MIN_REACH) + m.ent:BoundingRadius()
        Native.Probe(id, from.x, from.y, from.z, from.x + m.vel.x / m.speed * reach, from.y + m.vel.y / m.speed * reach,
            from.z + m.vel.z / m.speed * reach)
    end
end)

concommand.Add("gr_surfaces_status", function(ply)
    if IsValid(ply) and not ply:IsListenServerHost() then return end
    local up = 0
    for _, slot in ipairs(Surfaces.pool) do
        if slot.until_t then up = up + 1 end
    end
    GR.Print(string.format("surfaces: %s, %d squares standing on RDR2's surfaces now, %d placed since the start",
        enabled:GetBool() and "on" or "off", up, Surfaces.made))
end, nil, "Garry's Redemption: show the squares standing on RDR2's trees and walls for GMod's projectiles")
