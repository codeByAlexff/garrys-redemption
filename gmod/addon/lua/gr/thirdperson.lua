-- Third person (milestone 9), client side: the camera goes behind the GMod player's
-- shoulder, RDR2's camera goes with it (player.lua sends this view instead of the eyes), and
-- the player's model is drawn into RDR2's world (world_render.lua). RDR2's own ped stays
-- hidden: the GMod player is the one seen.
--
-- `gr_thirdperson` switches it on and off, `gr_firstperson` off. Keys bound to GMod's own
-- `thirdperson` and `firstperson` do the same from RDR2 (input.lua); or bind a key to
-- gr_thirdperson.

local GR = GR
GR.Third = GR.Third or { on = false }
local Third = GR.Third

local SIDE = 20   -- units to the right: over the shoulder
local UP = 6
local HULL = Vector(6, 6, 6)

-- In a GMod vehicle (user request: a third-person camera for vehicles): GMod's own vehicle
-- view, behind the vehicle by its size times (1 + its camera distance), which the mouse wheel
-- changes, and switched with Ctrl (GM:VehicleMove). It starts in third person. GMod's view
-- never reached RDR2 before: RDR2's camera is sent the view built here.
local VEHICLE_UP = 0.15  -- of the distance back: the camera a little above, looking over the roof

local function VehicleThird(ply)
    local vehicle = ply:GetVehicle()
    if not IsValid(vehicle) or not vehicle.GetThirdPersonMode or not vehicle:GetThirdPersonMode() then return end
    return vehicle
end
Third.VehicleThird = VehicleThird

local function VehicleBack(vehicle)
    -- Its collision box, not GMod's render bounds: the server's wall ray needs it too, and
    -- GetRenderBounds is client only.
    local radius = (vehicle:OBBMaxs() - vehicle:OBBMins()):Length()
    return radius + radius * (vehicle.GetCameraDistance and vehicle:GetCameraDistance() or 0)
end

-- The camera's offset from the focus.
local function Offset(ply, angles, back)
    if VehicleThird(ply) then return -angles:Forward() * back + Vector(0, 0, back * VEHICLE_UP) end
    return -angles:Forward() * back + angles:Right() * SIDE + angles:Up() * UP
end

-- What the camera goes around and how far behind it: the player's eyes, or while possessing
-- (possess.lua) the possessed one's head, further back the bigger it is. Its proxy's own
-- position, which the engine smooths between ticks on the client, not the player's, which
-- the server sets tick by tick.
local function Focus(ply, back)
    local vehicle = VehicleThird(ply)
    if vehicle then return ply:EyePos(), VehicleBack(vehicle), vehicle end
    local proxy = GR.Possess and GR.Possess.Of(ply)
    if not proxy then return ply:EyePos(), back end
    local mins, maxs = proxy:OBBMins(), proxy:OBBMaxs()
    return proxy:GetPos() + Vector(0, 0, maxs.z - 8), back + math.max(maxs.x - mins.x, maxs.y - mins.y) / 2, proxy
end

if SERVER then
    hook.Add("PlayerEnteredVehicle", "GR.Third", function(ply, vehicle)
        if vehicle.SetThirdPersonMode and GR.Native and GR.Native.Anchored and GR.Native.Anchored() then
            vehicle:SetThirdPersonMode(true)
        end
    end)

    concommand.Add("gr_vehicle_view", function(ply)
        local vehicle = IsValid(ply) and ply:GetVehicle()
        if IsValid(vehicle) and vehicle.SetThirdPersonMode then vehicle:SetThirdPersonMode(not vehicle:GetThirdPersonMode()) end
    end, nil, "Garry's Redemption: switch the vehicle's camera between first and third person")

    -- RDR2's ground and walls are physics only in the server realm, so the ray that keeps
    -- the camera out of them is cast here, every tick, and the client is told how much of
    -- the way to the camera is free. Works around: the camera went through walls and hills.
    hook.Add("Tick", "GR.Third", function()
        local ply = player.GetAll()[1]
        if not IsValid(ply) then return end
        local angles = ply:EyeAngles()
        local eye, back, proxy = Focus(ply, ply:GetInfoNum("gr_thirdperson_distance", 110))
        -- The possessed one's own proxy is around the start of the ray: not an obstacle.
        -- So is the vehicle the player sits in.
        local tr = util.TraceHull({ start = eye, endpos = eye + Offset(ply, angles, back),
            mins = -HULL, maxs = HULL, filter = proxy and { ply, proxy } or ply, mask = MASK_SOLID })
        ply:SetNW2Float("gr_cam_free", tr.Fraction)
    end)
    return
end

local distance = CreateClientConVar("gr_thirdperson_distance", "110", true, true,
    "Garry's Redemption: how far behind the player the third-person camera is, in units")

-- How much of the way to the camera is free: pulled in at once, let out slowly, so the
-- camera never shows the inside of a wall and does not jump back and forth at an edge.
local free, free_time = 1, 0
local function Free(ply)
    local now = RealTime()
    if now ~= free_time then
        local target = ply:GetNW2Float("gr_cam_free", 1)
        free = target < free and target or math.min(target, free + (now - free_time) * 2)
        free_time = now
    end
    return free
end

-- Where the camera is and which way it looks, in third person.
function Third.View(ply, angles)
    angles = angles or ply:EyeAngles()
    local focus, back = Focus(ply, distance:GetFloat())
    return focus + Offset(ply, angles, back) * Free(ply), angles
end

local function Active()
    if not (GR.Native and GR.Native.Anchored and GR.Native.Anchored()) then return false end
    local ply = LocalPlayer()
    if ply:InVehicle() then return VehicleThird(ply) ~= nil end
    return Third.on or (GR.Possess and GR.Possess.Of(ply) ~= nil)
end
Third.Active = Active

hook.Add("CalcView", "GR.Third", function(ply, _, angles, fov)
    if not Active() then return end
    -- Rendered ahead like the first-person view (player.lua, Player.Lead).
    if GR.Player and GR.Player.Lead then angles = GR.Player.Lead(angles) end
    -- The view lock (player.lua): RDR2's camera angles, around GMod's own player.
    angles = GR.Player.LockedView() or angles
    local origin = Third.View(ply, angles)
    Third.last_origin = origin
    return { origin = origin, angles = angles, fov = fov, drawviewer = true }
end)

hook.Add("ShouldDrawLocalPlayer", "GR.Third", function()
    if Active() then return not (GR.Possess and GR.Possess.Of(LocalPlayer())) end
end)

-- No gun floating in front of the camera in third person.
hook.Add("PreDrawViewModel", "GR.Third", function()
    if Active() then return true end
end)

concommand.Add("gr_thirdperson", function(ply)
    -- In a vehicle it switches the vehicle's view, as Ctrl does.
    if IsValid(ply) and ply:InVehicle() then
        RunConsoleCommand("gr_vehicle_view")
        return
    end
    Third.on = not Third.on
    GR.Print("third person ", Third.on and "on" or "off")
end, nil, "Garry's Redemption: switch between first and third person")

concommand.Add("gr_firstperson", function()
    if Third.on then GR.Print("third person off") end
    Third.on = false
end, nil, "Garry's Redemption: back to first person")
