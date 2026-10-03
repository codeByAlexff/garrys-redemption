-- Fast flight: noclip with Shift held flies at `gr_noclip_fast_speed` while linked to RDR2.
--
-- GMod's own noclip triples its speed with Shift (500 to 1500 units/s, measured), which is
-- about 100 km/h: slow over RDR2's map (user: "pressing shift while flying makes you fly
-- faster"). Shared, so the client predicts the same move the server makes.

local GR = GR
GR.Fly = GR.Fly or {}

local fast = CreateConVar("gr_noclip_fast_speed", "4000", FCVAR_ARCHIVE + FCVAR_REPLICATED,
    "Garry's Redemption: units per second noclip flies at with Shift held (0: GMod's own)")

-- How quickly the speed follows the keys: most of the way in a fifth of a second.
local RESPONSE = 8

hook.Add("Move", "GR.Fly", function(ply, mv)
    if ply:GetMoveType() ~= MOVETYPE_NOCLIP or not mv:KeyDown(IN_SPEED) then return end
    local speed = fast:GetFloat()
    if speed <= 0 or not GR.Native or not GR.Native.Anchored or not GR.Native.Anchored() then return end
    -- The way GMod's noclip goes: along the view, pitch included.
    local ang = mv:GetMoveAngles()
    local wish = ang:Forward() * mv:GetForwardSpeed() + ang:Right() * mv:GetSideSpeed() + ang:Up() * mv:GetUpSpeed()
    local target = wish:LengthSqr() > 1 and wish:GetNormalized() * speed or Vector(0, 0, 0)
    local dt = FrameTime()
    local vel = LerpVector(math.min(dt * RESPONSE, 1), mv:GetVelocity(), target)
    mv:SetVelocity(vel)
    mv:SetOrigin(mv:GetOrigin() + vel * dt)
    return true
end)
