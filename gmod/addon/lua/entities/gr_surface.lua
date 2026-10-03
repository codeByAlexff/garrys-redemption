-- A small solid square standing on one of RDR2's surfaces (a tree trunk, a wall, a rock) that
-- one of GMod's fast things is about to hit, so it bounces, sticks or blows up there. Placed
-- and recycled by gr/surfaces.lua. Server only, never sent to the client, never drawn: RDR2
-- draws the real surface.

AddCSLuaFile()

ENT.Type = "anim"
ENT.Base = "base_anim"
ENT.PrintName = "RDR2 surface"
ENT.Spawnable = false
ENT.AdminOnly = true
ENT.PhysgunDisabled = true

function ENT:CanTool() return false end

function ENT:Draw() end

if CLIENT then return end

-- Half the square's side and its depth behind the surface, in units.
ENT.HALF = 16
ENT.DEPTH = 24

function ENT:Initialize()
    self:SetModel("models/hunter/blocks/cube025x025x025.mdl")
    -- The box's front face (+x) is the surface; the rest is inside the tree or wall.
    self:PhysicsInitBox(Vector(-self.DEPTH, -self.HALF, -self.HALF), Vector(0, self.HALF, self.HALF))
    self:SetSolid(SOLID_VPHYSICS)
    self:SetMoveType(MOVETYPE_VPHYSICS)
    self:SetNoDraw(true)
    self:DrawShadow(false)
    local phys = self:GetPhysicsObject()
    if IsValid(phys) then
        phys:EnableMotion(false)
        phys:SetMaterial("wood")
    end
end

function ENT:UpdateTransmitState()
    return TRANSMIT_NEVER
end

function ENT:GravGunPickupAllowed()
    return false
end
