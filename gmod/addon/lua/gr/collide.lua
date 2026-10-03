-- RDR2's walls stop the GMod player too (server side).
--
-- GMod has no copy of RDR2's world yet (milestone 5), so RDR2 tests each move of the
-- player against its own world and holds the player at a wall, a wagon or a rock
-- (rdr2/src/player_sync.h, Collide). Each time it does, it says where and which way the
-- surface faces. Here the GMod player is pushed back out along that normal to the same
-- spot and loses the part of its velocity going into the surface, so the two players never
-- part and walking along a wall slides as it would in GMod.

local GR = GR
GR.Collide = GR.Collide or {}
local Collide = GR.Collide

local enabled = CreateConVar("gr_collide", "1", FCVAR_ARCHIVE,
    "Garry's Redemption: 1 = RDR2's walls and objects stop the GMod player")

Collide.serial = Collide.serial or 0
Collide.count = Collide.count or 0

hook.Add("FinishMove", "GR.Collide", function(ply, mv)
    if not enabled:GetBool() or not GR.Native or not GR.Native.Block then return end
    if not game.SinglePlayer() and not ply:IsListenServerHost() then return end
    local serial, bx, by, nx, ny = GR.Native.Block()
    if not serial or serial == Collide.serial then return end
    Collide.serial = serial
    if ply:GetMoveType() == MOVETYPE_NOCLIP then return end

    local length = math.sqrt(nx * nx + ny * ny)
    if length < 1e-6 then return end
    nx, ny = nx / length, ny / length

    -- How far the player is past the held spot, into the surface.
    local origin = mv:GetOrigin()
    local past = (origin.x - bx) * nx + (origin.y - by) * ny
    if past < 0 then
        origin.x = origin.x - past * nx
        origin.y = origin.y - past * ny
        mv:SetOrigin(origin)
    end
    local vel = mv:GetVelocity()
    local into = vel.x * nx + vel.y * ny
    if into < 0 then
        vel.x = vel.x - into * nx
        vel.y = vel.y - into * ny
        mv:SetVelocity(vel)
    end
    Collide.count = Collide.count + 1
end)
