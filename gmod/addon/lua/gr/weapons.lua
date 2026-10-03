-- Weapons and damage (milestone 6), server side.
--
-- GMod decides the hit, RDR2 applies it. GMod's weapons fire in GMod and hit the proxies;
-- each hit goes to RDR2 as an event (rdr2/src/combat_sync.h): a bullet becomes a real RDR2
-- bullet along the same line, fired by the player's ped, so RDR2 does the blood, the
-- reaction, the headshot and the law; a blow (crowbar, fist, a thrown prop) becomes damage
-- and a push. Explosions in GMod become real explosions in RDR2. The proxies themselves
-- take no damage: they have no health of their own.
--
-- The other way, whatever RDR2 does to the player's hidden ped (lawmen shooting back, fire,
-- dynamite) hurts the GMod player, whose health is the one that counts.

local GR = GR
GR.Weapons = GR.Weapons or {}
local Weapons = GR.Weapons

local enabled = CreateConVar("gr_weapons", "1", FCVAR_ARCHIVE,
    "Garry's Redemption: 1 = GMod's hits and explosions happen in RDR2, and RDR2 hurts the GMod player")
-- GMod's weapons are balanced for 100-health targets that take several shots (a pistol
-- does 12, the .357 75); RDR2's people die to one or two revolver shots to the body.
local scale = CreateConVar("gr_damage_scale", "3", FCVAR_ARCHIVE,
    "Garry's Redemption: RDR2 health taken per point of GMod damage")

-- GMod's own guns keep GMod's own sound and nothing else (the user's wish): only RDR2's guns
-- (sweps.lua) fire an audible RDR2 shot. With 1, every GMod shot is also heard in RDR2, so
-- its people react to gunfire that hits nothing.
-- GMod's copy of RDR2's world has no trees beyond the player's reach and no walls beyond the
-- terrain chunks, so its bullets went through them and left no mark (user: weapons should
-- "collide with objects like trees, houses"). Each bullet that hit none of RDR2's people goes
-- to RDR2 as a harmless, silent bullet along the same line (GR_EVENT_IMPACT): RDR2 stops it at
-- its own trees, walls and rocks and marks them. People behind those were already safe: the
-- bullet that hurts them is RDR2's, and RDR2's world stops it.
local impacts = CreateConVar("gr_bullet_impacts", "1", FCVAR_ARCHIVE,
    "Garry's Redemption: 1 = GMod's bullets hit RDR2's trees, walls and rocks and leave RDR2's marks")
local IMPACT_REACH = 16384 -- units, about 312 m

local gmod_shots = CreateConVar("gr_gmod_shots_in_rdr2", "0", FCVAR_ARCHIVE,
    "Garry's Redemption: 1 = GMod's own guns also make an RDR2 gunshot (RDR2's people hear misses)")

local EVENT_BULLET, EVENT_HIT, EVENT_EXPLOSION, EVENT_SHOT, EVENT_IMPACT = 1, 2, 3, 7, 9
local EVENTF_BUCKSHOT = 1

-- Removed when they go off: their position then is the blast's. Radius in units, from the
-- HL2 skill defaults for each.
local EXPLOSIVES = {
    npc_grenade_frag = 250,
    grenade_ar2 = 250,
    rpg_missile = 200,
    npc_satchel = 250,
    npc_tripmine = 200,
}
local DEFAULT_RADIUS = 250
-- Several damage reports of one blast arrive together: one explosion within this distance
-- and time of another is the same one.
local SAME_BLAST_DIST = 120
local SAME_BLAST_TIME = 0.2

Weapons.sent = Weapons.sent or 0
Weapons.blasts = Weapons.blasts or {}

local function On()
    return enabled:GetBool() and GR.Native and GR.Native.Event and GR.Native.Anchored()
end

local function SendExplosion(pos, radius)
    local now = CurTime()
    for i = #Weapons.blasts, 1, -1 do
        local b = Weapons.blasts[i]
        if now - b.t > SAME_BLAST_TIME then
            table.remove(Weapons.blasts, i)
        elseif b.pos:Distance(pos) < SAME_BLAST_DIST then
            return
        end
    end
    Weapons.blasts[#Weapons.blasts + 1] = { pos = pos, t = now }
    GR.Native.Event(EVENT_EXPLOSION, 0, 0, 0, radius, pos.x, pos.y, pos.z, pos.x, pos.y, pos.z, 0, 0, 1, 0)
    Weapons.sent = Weapons.sent + 1
end

-- The RDR2 gun the shooter holds (gr/sweps.lua), as its weapon hash, or 0 for GMod's own guns.
local function Gun(shooter)
    local weapon = IsValid(shooter) and shooter.GetActiveWeapon and shooter:GetActiveWeapon()
    return IsValid(weapon) and weapon.GR_Weapon or 0
end

local function IsGround(ent)
    return not IsValid(ent) or ent:IsWorld() or ent:GetClass() == "gr_chunk"
end

hook.Add("EntityTakeDamage", "GR.Weapons", function(target, dmg)
    if not On() or not IsValid(target) then return end

    if dmg:IsDamageType(DMG_BLAST) then
        -- Whatever the blast came from (a grenade, an explosive barrel, the dynamite tool),
        -- RDR2 gets one explosion where it went off.
        local inflictor = dmg:GetInflictor()
        local pos = IsValid(inflictor) and not inflictor:IsWorld() and inflictor:GetPos() or dmg:GetDamagePosition()
        SendExplosion(pos, EXPLOSIVES[IsValid(inflictor) and inflictor:GetClass() or ""] or DEFAULT_RADIUS)
        -- A proxy is hurt by the RDR2 explosion itself.
        if target:GetClass() == "gr_proxy" then return true end
        return
    end

    if target:GetClass() ~= "gr_proxy" or not target.gr_handle then return end
    local Native = GR.Native
    local pos = dmg:GetDamagePosition()
    if pos:IsZero() then pos = target:WorldSpaceCenter() end
    local attacker = dmg:GetAttacker()
    local from = IsValid(attacker) and (attacker:IsPlayer() or attacker:IsNPC()) and attacker:EyePos() or pos
    local amount = dmg:GetDamage() * scale:GetFloat()

    if dmg:IsBulletDamage() or dmg:IsDamageType(DMG_BUCKSHOT) then
        local dir = pos - from
        local flags = dmg:IsDamageType(DMG_BUCKSHOT) and EVENTF_BUCKSHOT or 0
        Native.Event(EVENT_BULLET, flags, target.gr_handle, amount, 0, from.x, from.y, from.z, pos.x, pos.y, pos.z,
            dir.x, dir.y, dir.z, 0, Gun(attacker))
    else
        -- A proxy GMod is throwing hits things in RDR2 as well (it is a ragdoll there), and
        -- RDR2 hurts it for that itself. So does the ground.
        if dmg:IsDamageType(DMG_CRUSH) and (target.gr_owned or IsGround(dmg:GetInflictor())) then return true end
        local force = dmg:GetDamageForce()
        local dir = force:LengthSqr() > 1 and force or (pos - from)
        -- GMod's damage force is an impulse; over the proxy's mass it is a change of speed.
        local phys = target:GetPhysicsObject()
        local mass = IsValid(phys) and phys:GetMass() or 80
        local push = math.min(force:Length() / mass, 1500)
        Native.Event(EVENT_HIT, 0, target.gr_handle, amount, 0, from.x, from.y, from.z, pos.x, pos.y, pos.z,
            dir.x, dir.y, dir.z, push)
    end
    Weapons.sent = Weapons.sent + 1
    return true
end)

-- What one bullet did (FireBullets' callback): one that stopped at GMod's copy of RDR2's
-- ground or walls, or hit nothing, goes on in RDR2 as an impact. One that hit RDR2's people is
-- a BULLET (EntityTakeDamage); one that hit GMod's own things stopped there.
local SURFACES = { gr_chunk = true, gr_blocker = true, gr_surface = true }

local function Impact(tr, flags, gun)
    local hit = tr.Entity
    if IsValid(hit) and not hit:IsWorld() and not SURFACES[hit:GetClass()] then return end
    local from, dir = tr.StartPos, tr.Normal
    local to = from + dir * IMPACT_REACH
    GR.Native.Event(EVENT_IMPACT, flags, 0, 0, 0, from.x, from.y, from.z, to.x, to.y, to.z, dir.x, dir.y, dir.z, 0, gun)
    Weapons.impacts = (Weapons.impacts or 0) + 1
end

-- Every shot the player fires is heard in RDR2 (combat_sync.h, Shot): RDR2's gun sound, and
-- its people react to gunfire, hit or miss. One per trigger pull, however many pellets.
local shot_tick = -1
hook.Add("EntityFireBullets", "GR.Weapons", function(ent, data)
    if not On() or not IsValid(ent) or not ent:IsPlayer() then return end
    local gun = Gun(ent)
    local heard = gun ~= 0 or gmod_shots:GetBool()
    local flags = (data.Num or 1) > 1 and EVENTF_BUCKSHOT or 0
    if heard and engine.TickCount() ~= shot_tick then
        shot_tick = engine.TickCount()
        local from = data.Src
        local tr = util.TraceLine({ start = from, endpos = from + data.Dir * 32768, filter = ent, mask = MASK_SHOT })
        local to = tr.HitPos
        GR.Native.Event(EVENT_SHOT, flags, 0, 0, 0, from.x, from.y, from.z, to.x, to.y, to.z, data.Dir.x, data.Dir.y,
            data.Dir.z, 0, gun)
        -- That audible bullet marks what a single bullet hits already.
        if (data.Num or 1) <= 1 then return end
    end
    if not impacts:GetBool() then return end
    local callback = data.Callback
    data.Callback = function(attacker, tr, dmg)
        Impact(tr, flags, gun)
        if callback then return callback(attacker, tr, dmg) end
    end
    return true
end)

hook.Add("EntityRemoved", "GR.Weapons", function(ent)
    local radius = EXPLOSIVES[ent:GetClass()]
    if radius and On() then SendExplosion(ent:GetPos(), radius) end
end)

-- RDR2's damage to the player's ped.
hook.Add("Tick", "GR.Weapons", function()
    if not enabled:GetBool() or not GR.Native or not GR.Native.TakePlayerHurt then return end
    local hurt = GR.Native.TakePlayerHurt()
    if hurt <= 0 then return end
    local ply = player.GetAll()[1]
    if not IsValid(ply) or not ply:Alive() then return end
    local info = DamageInfo()
    info:SetDamage(hurt)
    info:SetDamageType(DMG_BULLET)
    info:SetAttacker(game.GetWorld())
    info:SetInflictor(game.GetWorld())
    info:SetDamagePosition(ply:EyePos())
    ply:TakeDamageInfo(info)
    Weapons.hurt = (Weapons.hurt or 0) + hurt
end)

concommand.Add("gr_weapons_status", function(ply)
    if IsValid(ply) and not ply:IsListenServerHost() then return end
    GR.Print(string.format("weapons: %s, damage scale %.1f, %d events sent to RDR2, %d bullets went on in RDR2 as impacts, %d health taken by RDR2",
        enabled:GetBool() and "on" or "off", scale:GetFloat(), Weapons.sent, Weapons.impacts or 0, Weapons.hurt or 0))
end, nil, "Garry's Redemption: show what GMod's weapons have sent to RDR2")
