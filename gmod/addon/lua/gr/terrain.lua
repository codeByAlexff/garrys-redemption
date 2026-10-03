-- RDR2's ground in GMod (milestone 5): the GMod player walks, falls and lands on it with
-- GMod's own movement, and thrown proxies land on it.
--
-- The RDR2 plugin samples the ground around the player into chunks (rdr2/src/terrain_sync.h)
-- and keeps them in a table of slots in the shared memory. Here each slot becomes one
-- gr_chunk: a static mesh, placed through the floating origin.
--
-- gm_flatgrass has its own ground, so the GMod player is kept in its empty sky, around
-- HOME: the anchor is made there, and whenever the player strays too far from it the
-- floating origin moves (a rebase) and the player, the proxies GMod simulates and the
-- chunks move back by the same amount in one tick.
--
-- Shared for HOME, which the client's anchor needs. Everything else is server side.

local GR = GR
GR.Terrain = GR.Terrain or {}
local Terrain = GR.Terrain

-- gm_flatgrass, measured with traces: grass at z -12800, ceiling at 15360, walls at
-- +-15360, the spawn platform's top at -12288 under (0, 0). Around (0, 0, 0) nothing of
-- the map is near the chunks (they reach about 64 m, 3400 units, from the player).
Terrain.HOME = Vector(0, 0, 0)
Terrain.NEAR_HOME = 64

if CLIENT then
    -- gr_terrain_debug 1: the ground and walls GMod has, drawn over RDR2 through the
    -- overlay (ground green near the player, walls red), to see how well they fit RDR2's.
    -- The client reads the slots itself: the module image is shared with the server.
    local debug = CreateClientConVar("gr_terrain_debug", "0", false, false,
        "Garry's Redemption: 1 = draw RDR2's ground and walls as GMod has them, over RDR2")
    local GROUND_RANGE = 12 / 0.01905
    local GREEN, RED = Color(60, 255, 90, 160), Color(255, 50, 40, 255)
    local cache = {}

    hook.Add("HUDPaint", "GR.Terrain.Debug", function()
        local Native = GR.Native
        if not debug:GetBool() or not Native or not Native.ChunkMesh or not Native.Anchored() then return end
        local origin = Native.OriginSerial()
        local eye = LocalPlayer():EyePos()
        cam.Start3D()
        for slot = 1, 64 do
            local serial = Native.ChunkSerial(slot)
            local c = cache[slot]
            if serial == 0 then
                cache[slot] = nil
                c = nil
            elseif not c or c.serial ~= serial or c.origin ~= origin then
                local s, _, _, _, _, walls, x, y, z, _, _, verts = Native.ChunkMesh(slot)
                if s then
                    c = { serial = s, origin = origin, pos = Vector(x, y, z), verts = verts, walls = walls }
                    cache[slot] = c
                end
            end
            if c then
                local pos, verts = c.pos, c.verts
                local first_wall = #verts - c.walls * 6 + 1
                for i = 1, first_wall - 1, 3 do
                    local a = pos + verts[i]
                    if math.abs(a.x - eye.x) < GROUND_RANGE and math.abs(a.y - eye.y) < GROUND_RANGE then
                        local b = pos + verts[i + 1]
                        render.DrawLine(a, b, GREEN, false)
                        render.DrawLine(b, pos + verts[i + 2], GREEN, false)
                    end
                end
                for i = first_wall, #verts, 6 do
                    -- Each wall is the triangles (a, b, t) and (a, t, u): draw its outline.
                    local a, b, t, u = pos + verts[i], pos + verts[i + 1], pos + verts[i + 2], pos + verts[i + 5]
                    render.DrawLine(a, b, RED, false)
                    render.DrawLine(b, t, RED, false)
                    render.DrawLine(t, u, RED, false)
                    render.DrawLine(u, a, RED, false)
                    render.DrawLine(a, t, RED, false)
                end
            end
        end
        cam.End3D()
    end)
    return
end

local enabled = CreateConVar("gr_terrain", "1", FCVAR_ARCHIVE,
    "Garry's Redemption: 1 = RDR2's ground is built in GMod and the player stands on it")

local UNITS = 1 / 0.01905                              -- per metre (protocol/gr_units.h)
local SLOTS = 64                                       -- GR_MAX_CHUNKS
local BUILDS_PER_TICK = 4                              -- PhysicsFromMesh is a few ms at most
local REBASE_XY = 8000                                 -- units from HOME before a rebase
local REBASE_Z = 6000
local FELL_THROUGH = 4 * UNITS                         -- this far under a chunk's lowest point

Terrain.slots = Terrain.slots or {}    -- slot -> { serial, cx, cy, base_z, ent }
Terrain.by_key = Terrain.by_key or {}  -- chunk key -> the same record
Terrain.origin_serial = Terrain.origin_serial or -1

local function Key(cx, cy)
    return cx * 1000000 + cy
end

local function RemoveSlot(slot)
    local rec = Terrain.slots[slot]
    if not rec then return end
    if IsValid(rec.ent) then rec.ent:Remove() end
    if Terrain.by_key[rec.key] == rec then Terrain.by_key[rec.key] = nil end
    Terrain.slots[slot] = nil
end

function Terrain.RemoveAll()
    for slot in pairs(Terrain.slots) do RemoveSlot(slot) end
    Terrain.ready = false
end

local function Build(slot)
    local serial, cx, cy, base_z, holes, walls, x, y, z, mins, maxs, verts = GR.Native.ChunkMesh(slot)
    if not serial then return end
    RemoveSlot(slot)
    local rec = { serial = serial, cx = cx, cy = cy, base_z = base_z, holes = holes, walls = walls, key = Key(cx, cy) }
    Terrain.slots[slot] = rec
    Terrain.by_key[rec.key] = rec
    if #verts == 0 then return end  -- nothing found anywhere in it (yet)
    local ent = ents.Create("gr_chunk")
    if not IsValid(ent) then return end
    ent:SetPos(Vector(x, y, z))
    ent:Spawn()
    if ent:Build(verts, mins, maxs) then
        rec.ent = ent
    else
        ent:Remove()
        GR.Print("terrain: PhysicsFromMesh refused chunk (", cx, ", ", cy, ")")
    end
end

local function IsChunk(ent)
    return ent:GetClass() == "gr_chunk"
end

-- After an anchor: the player is where RDR2's ped stands, which should be on the ground but
-- can be a little in it (a ped GMod had left inside a slope) or over it. Inside, Source
-- calls the player stuck and it cannot move, so it is stood on the ground just above or
-- below it once that is built.
local SETTLE_REACH = 2 * UNITS

local function Settle(ply)
    local pos = ply:GetPos()
    local tr = util.TraceLine({
        start = pos + Vector(0, 0, SETTLE_REACH),
        endpos = pos - Vector(0, 0, SETTLE_REACH),
        filter = IsChunk,
        mask = MASK_PLAYERSOLID,
    })
    if tr.Hit and not tr.StartSolid then
        ply:SetPos(tr.HitPos + Vector(0, 0, 1))  -- just above: on the mesh itself is inside it
        GR.Print(string.format("terrain: stood the player on RDR2's ground, %.0f units %s", math.abs(tr.HitPos.z - pos.z),
            tr.HitPos.z > pos.z and "up" or "down"))
    end
end

-- Works around: a player whose box is even a hair inside the ground's mesh cannot move at
-- all (user report: "I can't move the character"; it stood 0.1 units deep after an anchor,
-- and a chunk rebuilt under the feet can do the same). It is lifted to the first free spot.
local function Unstick(ply)
    local pos = ply:GetPos()
    local function Inside(at)
        local tr = util.TraceEntity({ start = at, endpos = at, filter = ply }, ply)
        return tr.StartSolid and IsValid(tr.Entity) and tr.Entity:GetClass() == "gr_chunk"
    end
    if not Inside(pos) then return end
    for up = 1, 40 do
        local at = pos + Vector(0, 0, up)
        if not Inside(at) then
            ply:SetPos(at)
            Terrain.unsticks = (Terrain.unsticks or 0) + 1
            return
        end
    end
end

local function Reposition()
    for _, rec in pairs(Terrain.slots) do
        if IsValid(rec.ent) then rec.ent:SetPos(Vector(GR.Native.ChunkPos(rec.cx, rec.cy, rec.base_z))) end
    end
end

-- Floating origin: keeps the player near HOME, away from gm_flatgrass's own brushes and
-- inside Source's coordinate range.
local function Rebase(ply)
    if Terrain.respawning then return end
    local p = ply:GetPos()
    local off = p - Terrain.HOME
    if math.abs(off.x) < REBASE_XY and math.abs(off.y) < REBASE_XY and math.abs(off.z) < REBASE_Z then return end
    local d = Vector(math.Round(off.x), math.Round(off.y), math.Round(off.z))
    -- In a GMod vehicle the player goes where the vehicle goes: the vehicle is moved.
    local vehicle = ply:GetVehicle()
    if IsValid(vehicle) then
        -- Works around: a jeep fell through RDR2's ground to the empty map's floor, far below
        -- HOME, with the player in it. The player could not be moved back, the origin was
        -- moved anyway, tick after tick, and RDR2's player ended up 900 km off the map (user
        -- report). A vehicle that has fallen that far is left: the player gets out at HOME.
        if math.abs(off.z) >= REBASE_Z then
            ply:ExitVehicle()
            vehicle:Remove()
            ply:SetPos(Terrain.last_ground or Terrain.HOME)
            ply:SetLocalVelocity(vector_origin)
            return
        end
        vehicle:SetPos(vehicle:GetPos() - d)
        local phys = vehicle:GetPhysicsObject()
        if IsValid(phys) then phys:SetPos(vehicle:GetPos()) end
    else
        ply:SetPos(p - d)
    end
    -- The origin only moves if the player did.
    if ply:GetPos():DistToSqr(p - d) > 200 * 200 and not IsValid(vehicle) then
        ply:SetPos(p)
        return
    end
    GR.Native.Rebase(d.x, d.y, d.z, p.x, p.y, p.z)
    for _, ent in pairs(GR.Proxies and GR.Proxies.by_handle or {}) do
        if IsValid(ent) and ent.gr_owned then ent:SetPos(ent:GetPos() - d) end
    end
    if Terrain.last_ground then Terrain.last_ground = Terrain.last_ground - d end
    if Terrain.died_at then Terrain.died_at = Terrain.died_at - d end
    if Terrain.hold_z then Terrain.hold_z = Terrain.hold_z - d.z end
    Terrain.rebases = (Terrain.rebases or 0) + 1
end

function Terrain.Tick()
    local Native = GR.Native
    local on = enabled:GetBool()
    Native.SetTerrain(on)
    -- Switched either way: anchor afresh, at HOME with the terrain or where the player is
    -- without it, so GMod's heights and RDR2's agree again.
    if on ~= Terrain.was_on then
        if Terrain.was_on ~= nil or Native.Anchored() then Native.Unanchor() end
        Terrain.was_on = on
        if not on then Terrain.RemoveAll() end
    end
    if not on then return end

    local ply = player.GetAll()[1]
    if not IsValid(ply) then return end
    local anchored = Native.Anchored()
    if anchored and not Terrain.was_anchored then Terrain.settle = true end
    Terrain.was_anchored = anchored
    if anchored then Rebase(ply) end

    local serial = Native.OriginSerial()
    if serial ~= Terrain.origin_serial then
        Terrain.origin_serial = serial
        Reposition()
    end

    -- Positions need the anchor; without one the slots are left as they are.
    if anchored then
        local builds = 0
        for slot = 1, SLOTS do
            local s = Native.ChunkSerial(slot)
            local rec = Terrain.slots[slot]
            if s == 0 then
                if rec then RemoveSlot(slot) end
            elseif (not rec or rec.serial ~= s or (rec.ent ~= nil and not IsValid(rec.ent))) and builds < BUILDS_PER_TICK then
                Build(slot)
                builds = builds + 1
            end
        end
    end

    local pos = ply:GetPos()
    local cx, cy = Native.ChunkOf(pos.x, pos.y)
    local under = Terrain.by_key[Key(cx, cy)]
    Terrain.under = under
    Terrain.ready = anchored and under ~= nil and IsValid(under.ent)
    if Terrain.ready and Terrain.settle then
        Terrain.settle = false
        Settle(ply)
    end
    if Terrain.ready and ply:GetMoveType() ~= MOVETYPE_NOCLIP then Unstick(ply) end
    if Terrain.ready and ply:IsOnGround() then Terrain.last_ground = pos end
end

hook.Add("FinishMove", "GR.Terrain", function(ply, mv)
    if not enabled:GetBool() or not GR.Native or not GR.Native.Terrain then return end
    if not game.SinglePlayer() and not ply:IsListenServerHost() then return end
    local Native = GR.Native
    if not Native.Anchored() then
        -- About to anchor: wait at HOME, still.
        if Native.CanAnchor() then
            mv:SetOrigin(Terrain.HOME)
            mv:SetVelocity(vector_origin)
        end
        return
    end
    if ply:GetMoveType() == MOVETYPE_NOCLIP then return end

    local origin = mv:GetOrigin()
    if not Terrain.ready then
        -- The ground under the player is not built yet: it may not fall.
        Terrain.hold_z = Terrain.hold_z or origin.z
        if origin.z < Terrain.hold_z then
            origin.z = Terrain.hold_z
            mv:SetOrigin(origin)
        end
        local vel = mv:GetVelocity()
        if vel.z < 0 then
            vel.z = 0
            mv:SetVelocity(vel)
        end
        return
    end
    Terrain.hold_z = nil

    -- Through a hole (no ground found in a cell) or between two chunks: back to where it
    -- last stood. The chunk under the player is looked up here, not taken from the last
    -- Tick: after a teleport that one is somewhere else (seen: a false rescue).
    local cx, cy = Native.ChunkOf(origin.x, origin.y)
    local under = Terrain.by_key[Key(cx, cy)]
    local ent = under and under.ent
    if IsValid(ent) and origin.z < ent:GetPos().z - FELL_THROUGH then
        -- Works around: after a plugin reload RDR2 stood its ped 5.5 m too low and GMod
        -- anchored there, inside the hill; "where it last stood" was under the ground too and
        -- the rescue repeated forever. Then, or with nowhere known, the player goes on top of
        -- the built ground under it.
        local function ChunkBelow(from, reach)
            local tr = util.TraceLine({
                start = from, endpos = from - Vector(0, 0, reach),
                filter = function(e) return e:GetClass() == "gr_chunk" end, ignoreworld = true,
            })
            return tr.Hit and tr.HitPos + Vector(0, 0, 2)
        end
        local last = Terrain.last_ground
        local again = last and not ChunkBelow(last + Vector(0, 0, 40), 80)
        local to = not again and last
        if not to then
            to = ChunkBelow(Vector(origin.x, origin.y, ent:GetPos().z + 8000), 16000) or last
        end
        if to then
            mv:SetOrigin(to)
            mv:SetVelocity(vector_origin)
            Terrain.rescues = (Terrain.rescues or 0) + 1
            GR.Print("terrain: the player fell through the ground; put back ", again and "on top of it" or "where it last stood")
        end
    end
end)

hook.Add("PlayerDeath", "GR.Terrain", function(ply)
    if GR.Native and GR.Native.Anchored() then Terrain.died_at = ply:GetPos() end
end)

-- Where a dead player comes back (user request: "respawn nearby where they are at"): on the
-- built ground under the spot it died, so a death in the air or in water comes back on the
-- ground below; where it last stood if there is none built there.
function Terrain.RespawnPos()
    local died = Terrain.died_at
    Terrain.died_at = nil
    if died then
        local tr = util.TraceLine({
            start = died + Vector(0, 0, 64), endpos = died - Vector(0, 0, 16000),
            filter = function(e) return e:GetClass() == "gr_chunk" end, ignoreworld = true,
        })
        if tr.Hit and tr.HitNormal.z > 0.5 then return tr.HitPos + Vector(0, 0, 2) end
    end
    return Terrain.last_ground or Terrain.HOME
end

-- GMod respawns a dead player at gm_flatgrass's spawn, which the floating origin would take
-- for a walk to a spot far under RDR2's ground (and move RDR2's ped there). With the terrain
-- on, the player comes back where it died instead.
hook.Add("PlayerSpawn", "GR.Terrain", function(ply)
    if not enabled:GetBool() or not GR.Native or not GR.Native.Anchored() then return end
    Terrain.respawning = true
    local at = Terrain.RespawnPos()
    timer.Simple(0, function()
        Terrain.respawning = false
        if IsValid(ply) then ply:SetPos(at) end
    end)
end)

concommand.Add("gr_terrain_status", function(ply)
    if IsValid(ply) and not ply:IsListenServerHost() then return end
    local count, built, holes, walls = 0, 0, 0, 0
    for _, rec in pairs(Terrain.slots) do
        count = count + 1
        if IsValid(rec.ent) then built = built + 1 end
        holes = holes + (rec.holes or 0)
        walls = walls + (rec.walls or 0)
    end
    GR.Print(string.format("terrain: %s, %d chunks (%d built, %d walls, %d samples without ground), ground under the "
        .. "player %s, %d rebases, %d rescues", enabled:GetBool() and "on" or "off", count, built, walls, holes,
        Terrain.ready and "built" or "NOT built", Terrain.rebases or 0, Terrain.rescues or 0))
    local p = player.GetAll()[1]
    if IsValid(p) then
        local pos = p:GetPos()
        local cx, cy = GR.Native.ChunkOf(pos.x, pos.y)
        GR.Print(string.format("  player at (%.0f, %.0f, %.0f), chunk (%d, %d), on ground %s", pos.x, pos.y, pos.z,
            cx, cy, tostring(p:IsOnGround())))
    end
end, nil, "Garry's Redemption: show the RDR2 ground built in GMod")

function Terrain.Start()
    if game.IsDedicated() or not GR.Native or not GR.Native.ChunkMesh then
        GR.Print("terrain off: the module in the server realm is missing or too old")
        return
    end
    hook.Add("Tick", "GR.Terrain", Terrain.Tick)
end

Terrain.Start()
