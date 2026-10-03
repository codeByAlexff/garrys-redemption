-- Proxies, server side: one invisible gr_proxy per RDR2 ped, animal and horse near the
-- player. Each follows its RDR2 entity until GMod takes it (the physgun or gravity gun
-- picks it up, or punts it). From then on GMod's physics moves it and RDR2 makes the real
-- one follow as a ragdoll. Dropped, it flies until it settles, then RDR2 has it again.
-- Frozen with the physgun, GMod keeps it, and RDR2 holds the ragdoll in place.
--
-- The module was loaded in this realm too (gmsv_gr_win64.dll, the same build, which hands
-- this Lua state to the client's image), so GR.Native here reads the same link.

local GR = GR
GR.Proxies = GR.Proxies or {}
local Proxies = GR.Proxies
Proxies.by_handle = Proxies.by_handle or {}

local SETTLE_SPEED = 40   -- units/s: slower than this ...
local SETTLE_TIME = 0.5   -- ... for this long, and a dropped proxy is RDR2's again
local MAX_FREE_TIME = 8   -- s: a dropped proxy that never settles goes back anyway
-- Works around: until RDR2's world is in GMod (milestone 5), RDR2's ped can hit what GMod
-- does not have (a wagon, a wall) while its proxy flies on. Apart by more than this for
-- longer than that, and RDR2 finishes the fall on its own.
local SPLIT_DISTANCE = 60 -- units, about 1.1 m
local SPLIT_TIME = 0.25
local KNOCK_SPEED = 150   -- units/s: a GMod-driven proxy hitting a still one this fast takes it along
local ENTF_DEAD, ENTF_RAGDOLL, ENTF_SPAWNED = 1, 2, 16
local KIND_VEHICLE = 4
local KIND_OBJECT = 5
local DRIVE_HELD, DRIVE_FROZEN = 1, 2

local tick = 0

-- The physgun's reach while linked (user: "extend the length of the phys gun grab"). GMod's
-- own is 4096 units, 78 m: short in RDR2's open country. It is not saved, so GMod's comes
-- back by itself in a game without RDR2.
local range = CreateConVar("gr_physgun_range", "16384", FCVAR_ARCHIVE,
    "Garry's Redemption: physgun reach in units while linked to RDR2 (GMod's own: 4096)")
local physgun_maxrange = GetConVar("physgun_maxrange")

local Take

-- Proxies welded to a taken one go with it: an idle proxy has its motion off, and GMod's
-- weld would hold the taken one to it like a nail. RDR2 moves the welded entities with the
-- one attached to (constraint_sync.h), so only the taken one is driven there.
local function TakeWelded(ent)
    if not GR.Tools or not GR.Tools.Welded then return end
    for _, other in ipairs(GR.Tools.Welded(ent)) do
        if not other.gr_owned then Take(other, false) end
    end
end

function Take(ent, held)
    local first = not ent.gr_owned
    if not ent.gr_owned then
        ent.gr_owned = true
        GR.Print("proxies: GMod takes RDR2 entity ", ent.gr_handle)
        ent:CollisionRulesChanged()  -- solid to vehicles from now on (RunOver)
    end
    ent.gr_held = held
    ent.gr_released = CurTime()
    ent.gr_slow = 0
    local phys = ent:GetPhysicsObject()
    if IsValid(phys) then
        phys:EnableMotion(true)
        phys:Wake()
    end
    if first then TakeWelded(ent) end
end
Proxies.Take = Take

-- Let go of, but still GMod's: it flies on, or stays put if the physgun froze it. Motion
-- is left alone because freezing with the physgun also drops the prop, and turning motion
-- back on here would unfreeze it (seen in-game: right click did nothing).
local function Drop(ent)
    ent.gr_held = false
    ent.gr_released = CurTime()
    ent.gr_slow = 0
    ent.gr_apart = 0
end

local function GiveBack(ent)
    ent.gr_owned = false
    ent.gr_held = false
    local phys = ent:GetPhysicsObject()
    if IsValid(phys) then phys:EnableMotion(false) end
    ent:CollisionRulesChanged()
    GR.Print("proxies: RDR2 has entity ", ent.gr_handle, " again")
end

local function Create(handle, kind, mins, maxs)
    local ent = ents.Create("gr_proxy")
    if not IsValid(ent) then return nil end
    ent:Spawn()
    ent:Setup(handle, kind, mins, maxs)
    Proxies.by_handle[handle] = ent
    return ent
end

-- Works around: a plugin reload (CTRL+R in RDR2), its pause menu or a short handover drop
-- the link or the anchor for a moment, and every proxy used to be removed with it, taking
-- GMod's welds and ropes along (RDR2 then undid them too). Proxies now wait, as they are,
-- for this long before they go.
local KEEP_WHILE_AWAY = 30
-- Not listed by RDR2 for this long and a proxy goes: one frame without it (the first frames
-- of a reloaded plugin, a hiccup) is not enough.
local UNSEEN_GRACE = 1
local UNSEEN_GRACE_KEPT = 10

-- What GMod made around a proxy (a weld, a rope, the spawn itself) is lost with it, so those
-- wait longer: a reloaded plugin took more than a second to list them again (seen).
local function Grace(ent)
    if ent.gr_spawned or (ent.Constraints and #ent.Constraints > 0) then return UNSEEN_GRACE_KEPT end
    return UNSEEN_GRACE
end

function Proxies.Tick()
    local Native = GR.Native
    if not Native.Anchored() then
        Proxies.away_since = Proxies.away_since or CurTime()
        if CurTime() - Proxies.away_since > KEEP_WHILE_AWAY then Proxies.RemoveAll() end
        Native.ClearDriven()
        return
    end
    Proxies.away_since = nil
    if physgun_maxrange and physgun_maxrange:GetInt() ~= range:GetInt() then
        RunConsoleCommand("physgun_maxrange", tostring(range:GetInt()))
    end
    tick = tick + 1

    for i = 1, Native.EntityCount() do
        local handle, kind, flags, model, x, y, z, pitch, yaw, roll, _, _, _, mnx, mny, mnz, mxx, mxy, mxz =
            Native.Entity(i)
        if not handle then break end
        local ent = Proxies.by_handle[handle]
        if not IsValid(ent) then
            ent = Create(handle, kind, Vector(mnx, mny, mnz), Vector(mxx, mxy, mxz))
            if ent and bit.band(flags, ENTF_SPAWNED) ~= 0 then
                ent.gr_spawned = true
                if GR.Spawn and GR.Spawn.ProxyCreated then GR.Spawn.ProxyCreated(ent) end
            elseif ent and kind == KIND_OBJECT then
                -- RDR2's own crates and fences: solid to GMod's props, not to the player, whom
                -- RDR2's rays already stop at them (PlayerSync::Collide). Works around: a player
                -- put back inside one of their boxes fell through the ground over and over.
                ent:SetCollisionGroup(COLLISION_GROUP_WEAPON)
            end
        end
        if ent then
            ent.gr_seen = tick
            ent.gr_seen_time = CurTime()
            ent.gr_model = model -- for the duplicator (tools.lua)
            ent.gr_rdr2_pos = Vector(x, y, z)
            if not ent.gr_owned then
                ent:SetPos(Vector(x, y, z))
                -- Standing people stay upright; a body lying on the ground lies down. Vehicles
                -- and objects lie as they lie.
                if kind >= KIND_VEHICLE or bit.band(flags, ENTF_DEAD + ENTF_RAGDOLL) ~= 0 then
                    ent:SetAngles(Angle(pitch, yaw, roll))
                else
                    ent:SetAngles(Angle(0, yaw, 0))
                end
            end
        end
    end

    Native.ClearDriven()
    for handle, ent in pairs(Proxies.by_handle) do
        if not IsValid(ent) then
            Proxies.by_handle[handle] = nil
        elseif ent.gr_seen ~= tick and not ent.gr_owned and CurTime() - (ent.gr_seen_time or 0) > Grace(ent) then
            -- Out of range, despawned, or GMod no longer has the player. Only the proxy
            -- goes (spawn.lua: gr_dropping keeps the real one).
            ent.gr_dropping = true
            ent:Remove()
            Proxies.by_handle[handle] = nil
        elseif ent.gr_owned then
            local phys = ent:GetPhysicsObject()
            local frozen = IsValid(phys) and not phys:IsMotionEnabled()
            if not ent.gr_held and not frozen then
                local slow = IsValid(phys) and phys:GetVelocity():Length() < SETTLE_SPEED
                ent.gr_slow = slow and ent.gr_slow + FrameTime() or 0
                local apart = ent.gr_rdr2_pos and ent.gr_rdr2_pos:Distance(ent:GetPos()) > SPLIT_DISTANCE
                ent.gr_apart = apart and (ent.gr_apart or 0) + FrameTime() or 0
                if ent.gr_slow > SETTLE_TIME or ent.gr_apart > SPLIT_TIME
                    or CurTime() - ent.gr_released > MAX_FREE_TIME then
                    GiveBack(ent)
                end
            end
            if ent.gr_owned then
                local pos, ang = ent:GetPos(), ent:GetAngles()
                local vel = IsValid(phys) and phys:GetVelocity() or vector_origin
                local flags = (ent.gr_held and DRIVE_HELD or 0) + (frozen and DRIVE_FROZEN or 0)
                Native.Drive(handle, flags, pos.x, pos.y, pos.z, ang.pitch, ang.yaw, ang.roll, vel.x, vel.y, vel.z)
            end
        end
    end
end

local function IsProxy(ent)
    return IsValid(ent) and ent:GetClass() == "gr_proxy"
end

-- Called by gr_proxy's PhysicsCollide. A still proxy is frozen, so it would stop a thrown
-- one like a wall: instead it is taken too and sent on with part of the blow.
function Proxies.Collide(ent, data)
    local other = data.HitEntity
    -- One of GMod's own things (a thrown prop, a vehicle) hits a still vehicle or object of
    -- RDR2's: it is taken and goes with the blow, instead of standing like a wall.
    if not ent.gr_owned and IsValid(other) and not IsProxy(other) and not other:IsPlayer()
        and other:GetClass() ~= "gr_chunk" and data.Speed >= KNOCK_SPEED then
        local push = data.TheirOldVelocity * 0.6
        timer.Simple(0, function()
            if not IsValid(ent) or ent.gr_owned then return end
            Take(ent, false)
            local phys = ent:GetPhysicsObject()
            if IsValid(phys) then phys:SetVelocity(push) end
        end)
        return
    end
    if not ent.gr_owned or not IsProxy(other) or other.gr_owned or data.Speed < KNOCK_SPEED then return end
    local push = data.OurOldVelocity * 0.6
    -- Physics state cannot change inside a collision callback.
    timer.Simple(0, function()
        if not IsValid(other) or other.gr_owned then return end
        Take(other, false)
        local phys = other:GetPhysicsObject()
        if IsValid(phys) then phys:SetVelocity(push) end
    end)
end

-- Running RDR2's people and animals over with GMod's vehicles.
--
-- Works around: a still proxy does not move, so a jeep that drove into a person hit a wall
-- and was thrown away itself (user report). A person, horse or animal that still follows
-- RDR2 is therefore not solid to GMod's vehicles at all; each tick every moving vehicle is
-- checked for the ones it has reached, and those are taken by GMod and sent off with the
-- vehicle's speed, hurt by RDR2 in proportion (weapons.lua's HIT event). Once GMod's, the
-- proxy is solid again and the car pushes the ragdoll like any prop.
local RUN_OVER_SPEED = 120   -- units/s: slower than this a vehicle only nudges
local RUN_OVER_DAMAGE = 0.25 -- RDR2 health per unit/s of the vehicle

local function IsVehicle(ent)
    return IsValid(ent) and ent:IsVehicle()
end

hook.Add("ShouldCollide", "GR.Proxies.RunOver", function(a, b)
    local proxy, other = a, b
    if not IsProxy(proxy) then proxy, other = b, a end
    if IsProxy(proxy) and not proxy.gr_owned and (proxy.gr_kind or 0) <= 3 then
        if IsVehicle(other) then return false end
        -- Works around the user's "wagons and stagecoaches bugging and going flying in the air
        -- with no physics": an RDR2 wagon's box takes in its own harnessed horses and the
        -- people on its seats, so the moment GMod took it, it started inside their boxes
        -- and was thrown out of them, up into the air, and RDR2 copied the flight. People
        -- and animals that follow RDR2 are not solid to RDR2's wagons for GMod.
        if IsProxy(other) and other.gr_kind == KIND_VEHICLE then return false end
    end
end)

local function RunOver()
    if not GR.Native or not GR.Native.Anchored() then return end
    for _, veh in ipairs(ents.GetAll()) do
        if veh:IsVehicle() then
            local vel = veh:GetVelocity()
            local speed = vel:Length()
            if speed >= RUN_OVER_SPEED then
                local mins, maxs = veh:WorldSpaceAABB()
                for _, ent in ipairs(ents.FindInBox(mins, maxs)) do
                    if IsProxy(ent) and not ent.gr_owned and (ent.gr_kind or 0) <= 3 and ent.gr_handle then
                        Take(ent, false)
                        local phys = ent:GetPhysicsObject()
                        if IsValid(phys) then phys:SetVelocity(vel * 1.1 + Vector(0, 0, speed * 0.25)) end
                        local pos = ent:GetPos()
                        local dir = vel / speed
                        if GR.Native.Event then
                            GR.Native.Event(2, 0, ent.gr_handle, speed * RUN_OVER_DAMAGE, 0, pos.x, pos.y, pos.z, pos.x, pos.y,
                                pos.z, dir.x, dir.y, dir.z, math.min(speed, 1500))
                        end
                    end
                end
            end
        end
    end
end
hook.Add("Tick", "GR.Proxies.RunOver", RunOver)

hook.Add("OnPhysgunPickup", "GR.Proxies", function(_, ent)
    if IsProxy(ent) then Take(ent, true) end
end)
hook.Add("PhysgunDrop", "GR.Proxies", function(_, ent)
    if IsProxy(ent) and ent.gr_owned then Drop(ent) end
end)
hook.Add("GravGunOnPickedUp", "GR.Proxies", function(_, ent)
    if IsProxy(ent) then Take(ent, true) end
end)
hook.Add("GravGunOnDropped", "GR.Proxies", function(_, ent)
    if IsProxy(ent) and ent.gr_owned then Drop(ent) end
end)
-- A punt hits a proxy that is still following RDR2: take it before the impulse lands.
hook.Add("GravGunPunt", "GR.Proxies", function(_, ent)
    if IsProxy(ent) then Take(ent, false) end
end)

function Proxies.RemoveAll()
    for handle, ent in pairs(Proxies.by_handle) do
        if IsValid(ent) then
            ent.gr_dropping = true
            ent:Remove()
        end
        Proxies.by_handle[handle] = nil
    end
end

function Proxies.Start()
    if game.IsDedicated() then return end
    local ok, err = pcall(require, "gr")
    if not ok or not GR.Native then
        GR.Print("proxies off: the server copy of the module did not load (", tostring(err), ")")
        return
    end
    hook.Add("Tick", "GR.Proxies", Proxies.Tick)
    GR.Print("proxies: module loaded in the server realm.")
end

concommand.Add("gr_proxies", function(ply)
    if IsValid(ply) and not ply:IsListenServerHost() then return end
    local owned = 0
    local count = 0
    for _, ent in pairs(Proxies.by_handle) do
        if IsValid(ent) then
            count = count + 1
            if ent.gr_owned then owned = owned + 1 end
        end
    end
    GR.Print("proxies: ", count, " following RDR2 entities, ", owned, " of them GMod's")
    for handle, ent in pairs(Proxies.by_handle) do
        if IsValid(ent) then
            local p = ent:GetPos()
            GR.Print(string.format("  %d kind %d at (%.0f, %.0f, %.0f)%s%s", handle, ent.gr_kind or 0, p.x, p.y, p.z,
                ent.gr_owned and " GMod's" or "", ent.gr_held and ", held" or ""))
        end
    end
end, nil, "Garry's Redemption: list the proxies of RDR2 entities")

Proxies.Start()
