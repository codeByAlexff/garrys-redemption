-- The GMod player is sized like Arthur, so RDR2's camera (which sits at the GMod player's
-- eyes) is at a man's eye height and doorways and fences come up where they should.
-- GMod's own player is 72 units (1.37 m) tall with its eyes at 64 (1.22 m), which looked
-- like a child's view in RDR2.
--
-- Arthur is about 1.85 m tall; eyes about 1.73 m. Crouched scales GMod's own crouch.
-- Shared: the server moves the player, and the client predicts with the same hull.

local GR = GR
GR.Body = GR.Body or {}
local Body = GR.Body

local M = 1 / 0.01905  -- units per metre (protocol/gr_units.h)
Body.EYE = Vector(0, 0, math.Round(1.73 * M))           -- 91
Body.EYE_DUCKED = Vector(0, 0, math.Round(1.00 * M))    -- 52
Body.HULL_MIN = Vector(-16, -16, 0)
Body.HULL_MAX = Vector(16, 16, math.Round(1.85 * M))    -- 97
Body.HULL_DUCK_MAX = Vector(16, 16, math.Round(1.10 * M)) -- 58

function Body.Apply(ply)
    if not IsValid(ply) then return end
    ply:SetViewOffset(Body.EYE)
    ply:SetViewOffsetDucked(Body.EYE_DUCKED)
    ply:SetHull(Body.HULL_MIN, Body.HULL_MAX)
    ply:SetHullDuck(Body.HULL_MIN, Body.HULL_DUCK_MAX)
end

if SERVER then
    -- After the gamemode's own spawn code, which sets GMod's default size.
    hook.Add("PlayerSpawn", "GR.Body", function(ply)
        timer.Simple(0, function() Body.Apply(ply) end)
    end)
    for _, ply in ipairs(player.GetAll()) do Body.Apply(ply) end
else
    -- The client keeps its own copy of the hull for prediction. Checked every frame because
    -- a respawn resets it; applying is skipped while it already matches.
    hook.Add("Think", "GR.Body", function()
        local ply = LocalPlayer()
        if IsValid(ply) and ply:GetViewOffset() ~= Body.EYE then Body.Apply(ply) end
    end)
end
