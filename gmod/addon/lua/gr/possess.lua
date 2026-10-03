-- The possess tool: the GMod player becomes one of RDR2's people, horses or animals.
--
-- The ped is not carried about by GMod: RDR2 walks it with its own movement task along the
-- way the player pushes (rdr2/src/possess_sync.h), so it is animated and stays on RDR2's
-- ground. Here the GMod player goes into noclip, unseen, and is kept at the ped's proxy every
-- tick; the camera is the third-person one, around the proxy (thirdperson.lua). W/A/S/D steer
-- relative to the view, +speed sprints, +walk walks, +jump jumps.
--
-- The toolgun's Possess tool (weapons/gmod_tool/stools/gr_possess.lua) starts and ends it;
-- so do the console commands gr_possess (what the crosshair is on) and gr_unpossess.

local GR = GR
GR.Possess = GR.Possess or {}
local Possess = GR.Possess

-- The proxy this player is possessing, or nil.
function Possess.Of(ply)
    if not IsValid(ply) then return end
    local proxy = ply:GetNW2Entity("gr_possess")
    if IsValid(proxy) then return proxy end
end

-- The player's own movement stays out of it: the keys steer the ped instead. Both realms, so
-- the client's prediction agrees with the server.
hook.Add("SetupMove", "GR.Possess", function(ply, mv)
    if not Possess.Of(ply) then return end
    if SERVER then
        local yaw = Angle(0, mv:GetMoveAngles().yaw, 0)
        local dir = yaw:Forward() * mv:GetForwardSpeed() + yaw:Right() * mv:GetSideSpeed()
        dir.z = 0
        local pace = 0
        if dir:LengthSqr() > 1 then
            dir:Normalize()
            pace = mv:KeyDown(IN_WALK) and 1 or mv:KeyDown(IN_SPEED) and 3 or 2
        end
        Possess.move = dir * pace
        Possess.jump = mv:KeyDown(IN_JUMP)
    end
    mv:SetForwardSpeed(0)
    mv:SetSideSpeed(0)
    mv:SetUpSpeed(0)
    mv:SetButtons(bit.band(mv:GetButtons(), bit.bnot(IN_JUMP + IN_DUCK)))
    mv:SetVelocity(vector_origin)
end)

if CLIENT then return end

local function Ready()
    return GR.Native and GR.Native.Possess and GR.Native.Anchored()
end

local function CanPossess(ent)
    if not IsValid(ent) or ent:GetClass() ~= "gr_proxy" or not ent.gr_handle then return false end
    local kind = ent.gr_kind or 0
    return kind >= 1 and kind <= 3
end
Possess.CanPossess = CanPossess

-- Where the player's feet go: the bottom of the ped's box.
local function Feet(proxy)
    return proxy:GetPos() + Vector(0, 0, proxy:OBBMins().z)
end

function Possess.Start(ply, proxy)
    if not Ready() or not IsValid(ply) or not CanPossess(proxy) then return false end
    if Possess.proxy == proxy then return true end
    if IsValid(Possess.proxy) then Possess.Stop(true) end
    Possess.ply, Possess.proxy = ply, proxy
    Possess.move, Possess.jump = vector_origin, false
    ply:SetMoveType(MOVETYPE_NOCLIP)
    ply:SetNoDraw(true)
    ply:SetNW2Entity("gr_possess", proxy)
    ply:SetPos(Feet(proxy))
    GR.Print("possess: the player is RDR2 entity ", proxy.gr_handle)
    return true
end

-- `switching`: another one is taken straight away, so the player stays as it is.
function Possess.Stop(switching)
    local ply, proxy = Possess.ply, Possess.proxy
    if not ply then return end
    Possess.ply, Possess.proxy = nil, nil
    if GR.Native and GR.Native.Possess then GR.Native.Possess(0, 0, 0, false) end
    if not IsValid(ply) then return end
    ply:SetNW2Entity("gr_possess", NULL)
    if switching then return end
    ply:SetNoDraw(false)
    ply:SetMoveType(MOVETYPE_WALK)
    ply:SetLocalVelocity(vector_origin)
    if IsValid(proxy) then
        -- Step out behind it, a little above the ground it stands on (terrain.lua settles
        -- the player if that is inside a slope).
        local back = Angle(0, ply:EyeAngles().yaw, 0):Forward()
        ply:SetPos(Feet(proxy) + Vector(0, 0, 8) - back * (proxy:BoundingRadius() + 40))
    end
    GR.Print("possess: the player is itself again")
end

hook.Add("Tick", "GR.Possess", function()
    local ply, proxy = Possess.ply, Possess.proxy
    if not ply then return end
    -- The ped died, was removed or went out of RDR2's list; the player died; RDR2 took the
    -- player back.
    if not IsValid(ply) or not ply:Alive() or not CanPossess(proxy) or not Ready() then
        Possess.Stop()
        return
    end
    ply:SetPos(Feet(proxy))
    ply:SetLocalVelocity(vector_origin)
    local move = Possess.move or vector_origin
    GR.Native.Possess(proxy.gr_handle, move.x, move.y, Possess.jump or false)
end)

hook.Add("PlayerDeath", "GR.Possess", function(ply)
    if ply == Possess.ply then Possess.Stop() end
end)

-- What the player's crosshair is on, past the proxy it is inside of.
function Possess.Target(ply)
    local eye = ply:EyePos()
    local own = Possess.proxy
    local tr = util.TraceLine({ start = eye, endpos = eye + ply:GetAimVector() * 8000, mask = MASK_SHOT,
        filter = function(ent) return ent ~= ply and ent ~= own end })
    return tr.Entity
end

concommand.Add("gr_possess", function(ply)
    if not IsValid(ply) then ply = player.GetAll()[1] end
    if not IsValid(ply) or not ply:IsListenServerHost() then return end
    if not Possess.Start(ply, Possess.Target(ply)) then
        GR.Print("possess: point at one of RDR2's people, horses or animals")
    end
end, nil, "Garry's Redemption: possess the RDR2 person, horse or animal under the crosshair")

concommand.Add("gr_unpossess", function(ply)
    if IsValid(ply) and not ply:IsListenServerHost() then return end
    Possess.Stop()
end, nil, "Garry's Redemption: stop possessing")
