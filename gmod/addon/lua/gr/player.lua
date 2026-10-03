-- Player sync, GMod side: tells RDR2 where the GMod player is and where it is looking.
--
-- GMod runs on a small flat map and RDR2's world is kilometres wide, so the two are tied
-- together by an anchor: the moment the GMod player is declared to be standing where
-- RDR2's player ped stands. Nothing moves in GMod when that happens; the module only
-- notes which RDR2 position GMod's (0,0,0) now is. The anchor is made when RDR2 connects
-- and again whenever RDR2 gives the player back after owning it (a scripted scene, a
-- horse, death, or the user's handover key).

local GR = GR
GR.Player = GR.Player or {}
local Player = GR.Player

-- The two pictures on screen are not of the same moment: GMod's frame reaches the overlay
-- through the capture and Windows' compositor, later than RDR2's picture of the same view
-- reaches the screen (measured with a steady turn: about 20 to 30 ms), so in a fast turn
-- GMod's things slid against RDR2's world. Holding RDR2's view back would add that much
-- mouse lag to the whole world; instead GMod renders its own view this far ahead,
-- extrapolated from the turn rate, so it lands on screen where RDR2's view is by then. Only
-- a sudden start or stop of a turn shows (a few pixels of overshoot for that long).
local view_lead = CreateClientConVar("gr_view_lead_ms", "20", true, false,
    "Garry's Redemption: ms GMod renders its view ahead of RDR2's, to keep the overlay in step with RDR2 in turns")
local last_t, last_p, last_y
local rate_p, rate_y = 0, 0

-- Turn rate of the view sent to RDR2, smoothed over a few frames (one frame's mouse counts
-- are lumpy).
local function TrackRate(ang)
    local now = SysTime()
    if last_t and now > last_t then
        local dt = now - last_t
        local k = math.min(dt / 0.025, 1)
        rate_p = rate_p + (math.AngleDifference(ang.pitch, last_p) / dt - rate_p) * k
        rate_y = rate_y + (math.AngleDifference(ang.yaw, last_y) / dt - rate_y) * k
    end
    last_t, last_p, last_y = now, ang.pitch, ang.yaw
end

-- `ang` moved on by the lead. Used for GMod's own render view (and thirdperson.lua's).
function Player.LeadGuess(ang)
    local lead = math.Clamp(view_lead:GetFloat(), 0, 60) / 1000
    if lead == 0 or not GR.Native.Anchored() then return ang end
    return Angle(math.Clamp(ang.pitch + rate_p * lead, -89, 89), ang.yaw + rate_y * lead, ang.roll)
end

-- The view lock. Leading the view by a guess could not keep GMod's things still in RDR2's
-- world (user report: "when I move the camera all of the objects placed from GMod move"): the
-- guess is wrong whenever the turn speeds up or slows, and the two games draw at different
-- rates, so their pictures were never of the same view. With the lock, GMod draws every frame
-- with exactly the camera RDR2 says it is showing (GrHostFrame.view_pos), so both pictures
-- are of the same view. RDR2 can hold each view back `gr_view_hold_frames` of its frames to
-- give GMod's picture time to reach the screen; measured (tools/overlay_lag.py on two flat
-- red props, GMod at 250 fps, RDR2 at 160): 0 ms apart with no hold, and each frame of hold
-- puts GMod about 6 ms ahead, so the default is none and no mouse lag is added. The lead is
-- not used with the lock.
local view_lock = CreateClientConVar("gr_view_lock", "1", true, false,
    "Garry's Redemption: 1 = GMod draws with the very camera RDR2 shows, so its things stay put in RDR2's world")
local view_hold = CreateClientConVar("gr_view_hold_frames", "0", true, false,
    "Garry's Redemption: RDR2 frames a view is held back so GMod's picture of it is ready (0 to 12)")

-- The angles of RDR2's camera, or nothing when the lock is off or RDR2 has none.
--
-- Only the angles are locked, not the position. A first version drew from RDR2's camera
-- position too, and the user saw the character stutter while moving: that position is the
-- player's of a few milliseconds ago and arrives at RDR2's frame rate, so everything that
-- goes with the player (its model in third person, what the physgun holds) shook against
-- the camera (measured walking: 0.6 to 0.9 units rms, 5 to 7 peak to peak). GMod's camera
-- stays on GMod's own player, as it was before the lock; a turn is what made things slide.
function Player.LockedView()
    local Native = GR.Native
    if not view_lock:GetBool() or not Native or not Native.HostView or not Native.Anchored() then return end
    local x, _, _, pitch, yaw, roll = Native.HostView()
    if not x then return end
    return Angle(pitch, yaw, roll)
end

function Player.Lead(ang)
    if view_lock:GetBool() then return ang end
    return Player.LeadGuess(ang)
end

hook.Add("CalcView", "GR.Lead", function(ply, origin, angles, fov)
    if GR.Third and GR.Third.Active and GR.Third.Active() then return end
    if not GR.Native or not GR.Native.Anchored() then return end
    local ang = Player.LockedView()
    if ang then return { origin = origin, angles = ang, fov = fov } end
    if view_lead:GetFloat() <= 0 then return end
    return { origin = origin, angles = Player.Lead(angles), fov = fov }
end)

-- Once per frame, between the module's Tick and Publish.
function Player.Frame()
    local ply = LocalPlayer()
    if not IsValid(ply) then return end
    local Native = GR.Native

    -- With RDR2's ground built in GMod the anchor is made at HOME, where the server is
    -- holding the player (terrain.lua). Until this realm has seen it get there, wait.
    local pos = ply:GetPos()
    local away = Native.Terrain() and pos:DistToSqr(GR.Terrain.HOME) > GR.Terrain.NEAR_HOME ^ 2
    if not Native.Anchored() and not away then
        -- HOME itself, not what this realm sees: the server holds the player exactly there,
        -- and this realm's copy can be a little behind (seen: 33 units up, which put the
        -- player inside RDR2's ground).
        local at = Native.Terrain() and GR.Terrain.HOME or pos
        local ok, _, yaw = Native.Anchor(at.x, at.y, at.z)
        if ok then
            -- Face the way RDR2's ped was facing, level. Except after a pause: RDR2's pause
            -- menu and map stop its script thread, so GMod lets go and anchors again when
            -- they close, and the ped still faces the yaw GMod gave it. Then keep the
            -- pitch too, or every trip to the map would level the view.
            local view = GR.Input.view
            local same = math.abs(math.AngleDifference(yaw, view.yaw)) < 1
            GR.Input.SetView(same and view.pitch or 0, yaw)
            GR.Print("player: anchored to RDR2's player. GMod has the player.")
        elseif Player.was_anchored then
            GR.Print("player: anchor dropped. RDR2 has the player, or has gone.")
        end
        Player.was_anchored = ok
    end

    -- Also while not anchored: it is what lets go of keys that were down when RDR2 took
    -- the player or went away.
    GR.Input.Frame()
    if not Native.Anchored() then return end

    local pos = ply:GetPos()
    local vel = ply:GetVelocity()
    local eye = ply:EyePos()
    local ang = ply:EyeAngles()
    -- In third person RDR2's camera is where GMod's is: behind the player.
    if GR.Third and GR.Third.Active and GR.Third.Active() then eye, ang = GR.Third.View(ply) end
    TrackRate(ang)
    if Native.SetViewHold then Native.SetViewHold(view_lock:GetBool() and view_hold:GetInt() or 0) end
    -- One of RDR2's guns in hand (sweps.lua): RDR2 shows its own model of it. Not in third
    -- person, where no gun is held up to the camera.
    if Native.SetWeapon then
        local weapon = ply:GetActiveWeapon()
        local hash = ply:Alive() and IsValid(weapon) and weapon.GR_Weapon or 0
        local third = GR.Third and GR.Third.Active and GR.Third.Active()
        local hand = third and hash ~= 0 and not (GR.Possess and GR.Possess.Of(ply)) and GR.WorldRender
            and GR.WorldRender.Hand and GR.WorldRender.Hand(ply)
        if hand then
            -- Third person: RDR2's gun in the hand of the player's model, along its aim.
            local aim = ply:EyeAngles()
            Native.SetWeapon(hash, hand.x, hand.y, hand.z, aim.pitch, aim.yaw, 0)
        else
            Native.SetWeapon(third and 0 or hash)
        end
    end
    Native.SetPlayer(pos.x, pos.y, pos.z, vel.x, vel.y, vel.z, eye.x, eye.y, eye.z,
        ang.pitch, ang.yaw, ang.roll, ply:GetFOV(),
        ply:IsOnGround(), ply:Crouching(), ply:GetMoveType() == MOVETYPE_NOCLIP, vgui.CursorVisible())
end
