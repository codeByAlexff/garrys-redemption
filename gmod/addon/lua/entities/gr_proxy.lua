-- An invisible stand-in for one RDR2 ped, animal, horse, vehicle or spawned object, sized
-- from its RDR2 model box.
-- gr/proxies.lua creates and moves these; the physgun and gravity gun pick them up like any
-- prop, and RDR2 makes the real one follow while GMod has it.

AddCSLuaFile()

ENT.Type = "anim"
ENT.Base = "base_anim"
ENT.PrintName = "RDR2 entity"
ENT.Spawnable = false
ENT.AdminOnly = true

-- Nothing of it is drawn: RDR2 draws the real entity.
function ENT:Draw() end

-- Works around: the physgun often did not pick up someone the player was aiming at (user:
-- "there are times I'm in front of an entity and the physgun does not grab them"). A person's
-- proxy is a slim prism (Points) so it does not catch on things, about a third of a metre
-- across, while RDR2 draws arms, a coat and a hat around it and the person moves between the
-- two games' frames. For a trace from the shooting position of a player holding the physgun
-- or the gravity gun, the proxy is its whole RDR2 box grown by a margin: GRAB_MARGIN units,
-- plus GRAB_PER_UNIT per unit of distance (about 2 degrees of aim), up to GRAB_MAX. Every
-- other trace (bullets, the player's movement, tools) gets the real shape: returning true
-- falls back to it (returning nothing made those traces miss the proxy altogether: seen).
-- Measured on a person's proxy 10 m away: the physgun takes it up to 4 degrees off its middle,
-- the crowbar's trace only dead on, as before.
local GRAB_WEAPONS = { weapon_physgun = true, weapon_physcannon = true }
local GRAB_MARGIN, GRAB_PER_UNIT, GRAB_MAX = 6, 0.035, 40

-- Only people: animals, horses, wagons and objects already are their whole box, and growing
-- theirs made the physgun catch a stagecoach's horses, or the bench or table next to someone
-- sitting, instead of what the player aimed at (seen: aimed at a coach, the beam hit a horse).
function ENT:TestCollision(start, delta, isbox)
    if isbox or self:GetNW2Int("gr_kind") ~= 1 then return true end
    local ply = player.GetAll()[1]
    if not IsValid(ply) then return true end
    local weapon = ply:GetActiveWeapon()
    if not IsValid(weapon) or not GRAB_WEAPONS[weapon:GetClass()] then return true end
    if start:DistToSqr(ply:GetShootPos()) > 64 * 64 then return true end
    local m = math.min(GRAB_MARGIN + start:Distance(self:WorldSpaceCenter()) * GRAB_PER_UNIT, GRAB_MAX)
    local grow = Vector(m, m, m)
    local hit, normal, fraction = util.IntersectRayWithOBB(start, delta, self:GetPos(), self:GetAngles(),
        self:OBBMins() - grow, self:OBBMaxs() + grow)
    if not hit then return true end
    return { HitPos = hit, Fraction = fraction, Normal = normal }
end

if CLIENT then return end

-- An upright eight-sided prism inside the box, for people: a box's corners would catch on
-- everything. The box itself for animals and horses.
local function Points(kind, mins, maxs)
    local points = {}
    if kind == 1 then
        local cx, cy = (mins.x + maxs.x) / 2, (mins.y + maxs.y) / 2
        -- RDR2's boxes for people are wide (arms out); a body is about 18 units across.
        local r = math.min((maxs.x - mins.x) / 2, (maxs.y - mins.y) / 2, 9)
        for i = 0, 7 do
            local a = math.rad(i * 45 + 22.5)
            local x, y = cx + math.cos(a) * r, cy + math.sin(a) * r
            points[#points + 1] = Vector(x, y, mins.z)
            points[#points + 1] = Vector(x, y, maxs.z)
        end
    else
        for _, x in ipairs({ mins.x, maxs.x }) do
            for _, y in ipairs({ mins.y, maxs.y }) do
                for _, z in ipairs({ mins.z, maxs.z }) do
                    points[#points + 1] = Vector(x, y, z)
                end
            end
        end
    end
    return points
end

local MASS = { [1] = 80, [2] = 60, [3] = 450, [4] = 800 }
local KG_PER_CUBIC_UNIT = 250 * 0.01905 ^ 3  -- an object weighs about a quarter of its box in water

function ENT:PhysicsCollide(data)
    if GR and GR.Proxies then GR.Proxies.Collide(self, data) end
end

-- kind is GR_ENT_*: 1 ped, 2 animal, 3 horse, 4 vehicle, 5 object. mins and maxs in the
-- entity's own space.
function ENT:Setup(handle, kind, mins, maxs)
    self.gr_handle = handle
    self.gr_kind = kind
    -- The client draws people as slim occluders (world_render.lua).
    self:SetNW2Int("gr_kind", kind)
    self:PhysicsInitConvex(Points(kind, mins, maxs))
    self:SetMoveType(MOVETYPE_VPHYSICS)
    self:SetSolid(SOLID_VPHYSICS)
    self:EnableCustomCollisions(true)
    -- proxies.lua decides whether a vehicle collides with it (the ShouldCollide hook).
    self:SetCustomCollisionCheck(true)
    self:SetCollisionBounds(mins, maxs)
    -- Traces only test what reaches these bounds: grown so the grab margin above can be hit,
    -- without growing the collision bounds (world_render draws them as occluders).
    local grow = Vector(GRAB_MAX, GRAB_MAX, GRAB_MAX)
    self:SetSurroundingBounds(mins - grow, maxs + grow)
    self:DrawShadow(false)
    -- Works around: GMod's fists (and other scripted melee) only hurt what has health above
    -- 0, so punches did nothing to a person. The number is never used: RDR2 has the real one.
    if kind <= 3 then
        self:SetMaxHealth(100)
        self:SetHealth(100)
    end
    local phys = self:GetPhysicsObject()
    if IsValid(phys) then
        local size = maxs - mins
        phys:SetMass(MASS[kind] or math.Clamp(size.x * size.y * size.z * KG_PER_CUBIC_UNIT, 2, 400))
        -- Following RDR2 until GMod takes it.
        phys:EnableMotion(false)
    end
end
