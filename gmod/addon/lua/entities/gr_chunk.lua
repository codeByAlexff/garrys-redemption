-- One square of RDR2's ground as a static physics mesh, built by gr/terrain.lua from a
-- terrain chunk. Invisible (RDR2 draws the ground) and server only: the player's movement
-- and the physics run in the server realm.

AddCSLuaFile()

ENT.Type = "anim"
ENT.Base = "base_anim"
ENT.PrintName = "RDR2 ground"
ENT.Spawnable = false
ENT.AdminOnly = true
-- Sandbox reads these: the physgun would unfreeze the ground.
ENT.PhysgunDisabled = true

-- The ground takes tools the way GMod's world does: things are pasted, placed, welded and
-- roped onto it (the duplicator's paste was refused on it: seen), but nothing may change or
-- remove the ground itself, and the duplicator's right click may not copy it.
local NOT_ON_GROUND = { remover = true, colour = true, material = true, paint = true, physprop = true,
    nocollide = true, trails = true, inflator = true, eyeposer = true, faceposer = true, fingerposer = true }
function ENT:CanTool(_, _, mode, _, button)
    if NOT_ON_GROUND[mode] then return false end
    if mode == "duplicator" and button ~= 1 then return false end
    return true
end

function ENT:Draw() end

if CLIENT then return end

function ENT:UpdateTransmitState()
    return TRANSMIT_NEVER
end

function ENT:GravGunPickupAllowed()
    return false
end

-- verts: Vectors relative to the entity, three per triangle; mins and maxs bound them.
function ENT:Build(verts, mins, maxs)
    if not self:PhysicsFromMesh(verts) then return false end
    self:SetSolid(SOLID_VPHYSICS)
    self:SetMoveType(MOVETYPE_VPHYSICS)
    -- Without it the player's movement and traces treat a mesh like this as spongy or
    -- miss it (wiki: Entity:PhysicsFromMesh).
    self:EnableCustomCollisions(true)
    self:SetCollisionBounds(mins - Vector(1, 1, 1), maxs + Vector(1, 1, 1))
    self:DrawShadow(false)
    local phys = self:GetPhysicsObject()
    if IsValid(phys) then phys:EnableMotion(false) end
    return true
end
