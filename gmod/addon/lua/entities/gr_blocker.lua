-- One of RDR2's solid things right around the player (a trunk, a post, a rock, a crate) as an
-- invisible box the player's movement stops at, moved every tick by gr/near.lua. Server only,
-- like the ground (gr_chunk): the player's movement runs there.

AddCSLuaFile()

ENT.Type = "anim"
ENT.Base = "base_anim"
ENT.PrintName = "RDR2 obstacle"
ENT.Spawnable = false
ENT.AdminOnly = true
ENT.PhysgunDisabled = true

function ENT:CanTool() return false end

function ENT:Draw() end

if CLIENT then return end

function ENT:Initialize()
    -- An oriented box from its collision bounds, with no physics object: the player's
    -- movement and traces stop at it, and moving it every tick costs nothing.
    self:SetModel("models/hunter/blocks/cube025x025x025.mdl")
    self:SetMoveType(MOVETYPE_NONE)
    self:SetSolid(SOLID_OBB)
    self:SetNoDraw(true)
    self:DrawShadow(false)
end

function ENT:UpdateTransmitState()
    return TRANSMIT_NEVER
end

function ENT:GravGunPickupAllowed()
    return false
end
