-- RDR2's guns as GMod weapons (Weapons tab, "Red Dead Redemption 2").
--
-- Each is picked from GMod's weapon HUD like any weapon and has the clip, pace and punch of
-- the RDR2 gun it is named after, but the gun itself is RDR2's: GMod draws no viewmodel, RDR2
-- shows its own model of it in front of the camera (rdr2/src/weapon_view.h), and its shots go
-- to RDR2 with that gun's weapon hash (weapons.lua, GrEvent.model), so the bullet and the
-- gunshot are that gun's own, made by RDR2. GMod plays no sound for it unless
-- gr_rdr2_gun_gmod_sound is 1. The Half-Life 2 models named here are only what other players
-- and third person see.
--
-- The hashes are the joaat of the RDR2 weapon name, taken from
-- github.com/femga/rdr3_discoveries weapons/weapons.lua (checked 2026-10-03).

local gmod_sound = CreateConVar("gr_rdr2_gun_gmod_sound", "0", FCVAR_ARCHIVE + FCVAR_REPLICATED,
    "Garry's Redemption: 1 = RDR2's guns also play a Half-Life 2 gunshot in GMod")

local REVOLVER = { vm = "models/weapons/c_357.mdl", wm = "models/weapons/w_357.mdl", hold = "revolver",
    sound = "Weapon_357.Single", ammo = "357", slot = 1 }
local PISTOL = { vm = "models/weapons/c_pistol.mdl", wm = "models/weapons/w_pistol.mdl", hold = "pistol",
    sound = "Weapon_Pistol.Single", ammo = "Pistol", slot = 1 }
local LONG = { vm = "models/weapons/c_shotgun.mdl", wm = "models/weapons/w_shotgun.mdl", hold = "shotgun",
    sound = "Weapon_357.Single", ammo = "357", slot = 3 }
local SHOTGUN = { vm = "models/weapons/c_shotgun.mdl", wm = "models/weapons/w_shotgun.mdl", hold = "shotgun",
    sound = "Weapon_Shotgun.Single", ammo = "Buckshot", slot = 3 }

-- damage is GMod's (RDR2 takes gr_damage_scale times it); delay in seconds between shots.
local GUNS = {
    { class = "gr_revolver_cattleman", name = "Cattleman Revolver", hash = 0x169F59F7, kind = REVOLVER,
        damage = 40, delay = 0.45, clip = 6, cone = 0.012, recoil = 3 },
    { class = "gr_revolver_schofield", name = "Schofield Revolver", hash = 0x7BBD1FF6, kind = REVOLVER,
        damage = 50, delay = 0.5, clip = 6, cone = 0.01, recoil = 4 },
    { class = "gr_pistol_volcanic", name = "Volcanic Pistol", hash = 0x020D13FF, kind = REVOLVER,
        damage = 65, delay = 0.9, clip = 8, cone = 0.012, recoil = 6 },
    { class = "gr_pistol_mauser", name = "Mauser Pistol", hash = 0x8580C63E, kind = PISTOL,
        damage = 25, delay = 0.16, clip = 10, cone = 0.02, recoil = 1.5, auto = true },
    { class = "gr_repeater_carbine", name = "Carbine Repeater", hash = 0xF5175BA1, kind = LONG,
        damage = 45, delay = 0.55, clip = 7, cone = 0.006, recoil = 3 },
    { class = "gr_repeater_lancaster", name = "Lancaster Repeater", hash = 0xA84762EC, kind = LONG,
        damage = 40, delay = 0.4, clip = 14, cone = 0.007, recoil = 2.5 },
    { class = "gr_rifle_boltaction", name = "Bolt Action Rifle", hash = 0x772C8DD6, kind = LONG,
        damage = 85, delay = 1.1, clip = 5, cone = 0.002, recoil = 5 },
    { class = "gr_sniperrifle_rollingblock", name = "Rolling Block Rifle", hash = 0xE1D2B317, kind = LONG,
        damage = 120, delay = 1.6, clip = 1, cone = 0.0005, recoil = 7, scope = 20 },
    { class = "gr_shotgun_doublebarrel", name = "Double-Barreled Shotgun", hash = 0x6DFA071B, kind = SHOTGUN,
        damage = 14, pellets = 9, delay = 0.3, clip = 2, cone = 0.07, recoil = 7 },
    { class = "gr_shotgun_pump", name = "Pump-Action Shotgun", hash = 0x31B7B9FE, kind = SHOTGUN,
        damage = 12, pellets = 9, delay = 0.8, clip = 5, cone = 0.06, recoil = 6 },
    { class = "gr_shotgun_sawedoff", name = "Sawed-Off Shotgun", hash = 0x1765A8F8, kind = SHOTGUN,
        damage = 12, pellets = 9, delay = 0.35, clip = 2, cone = 0.11, recoil = 8 },
}

-- The Red Dead tab lists them (spawn.lua). Nobody starts with one: they are picked there, or
-- in the Weapons tab.
GR.Guns = GUNS

for index, gun in ipairs(GUNS) do
    local kind = gun.kind
    local SWEP = { Primary = {}, Secondary = {} }
    SWEP.Base = "weapon_base"
    SWEP.PrintName = gun.name
    SWEP.Category = "Red Dead Redemption 2"
    SWEP.Purpose = "RDR2's " .. gun.name .. ": RDR2 fires and hears this very gun"
    SWEP.Spawnable = true
    SWEP.AdminOnly = false
    SWEP.ViewModel = kind.vm
    SWEP.WorldModel = kind.wm
    SWEP.UseHands = false
    SWEP.ViewModelFOV = 54
    SWEP.Slot = kind.slot
    SWEP.SlotPos = 10 + index
    SWEP.DrawAmmo = true
    SWEP.DrawCrosshair = true
    SWEP.Primary.ClipSize = gun.clip
    SWEP.Primary.DefaultClip = gun.clip * 8
    SWEP.Primary.Automatic = gun.auto or false
    SWEP.Primary.Ammo = kind.ammo
    SWEP.Secondary.ClipSize = -1
    SWEP.Secondary.DefaultClip = -1
    SWEP.Secondary.Automatic = false
    SWEP.Secondary.Ammo = "none"
    -- weapons.lua sends this with every shot and hit of the weapon.
    SWEP.GR_Weapon = gun.hash

    function SWEP:Initialize()
        self:SetHoldType(kind.hold)
    end

    -- RDR2 draws the gun (weapon_view.h) while the bridge is up; on its own GMod shows the
    -- Half-Life 2 stand-in.
    function SWEP:PreDrawViewModel()
        if GR.Native and GR.Native.Anchored and GR.Native.Anchored() then return true end
    end

    function SWEP:PrimaryAttack()
        if not self:CanPrimaryAttack() then return end
        self:SetNextPrimaryFire(CurTime() + gun.delay)
        if gmod_sound:GetBool() then self:EmitSound(kind.sound) end
        self:ShootBullet(gun.damage, gun.pellets or 1, gun.cone)
        self:TakePrimaryAmmo(1)
        local owner = self:GetOwner()
        if IsValid(owner) and owner:IsPlayer() then owner:ViewPunch(Angle(-gun.recoil, 0, 0)) end
    end

    -- A scope for the rifle that has one in RDR2; nothing for the others.
    function SWEP:SecondaryAttack()
        if not gun.scope then return end
        local owner = self:GetOwner()
        if not IsValid(owner) or not owner:IsPlayer() then return end
        self.gr_scoped = not self.gr_scoped
        owner:SetFOV(self.gr_scoped and gun.scope or 0, 0.15)
        self:SetNextSecondaryFire(CurTime() + 0.3)
    end

    local function Unscope(self)
        local owner = self:GetOwner()
        if self.gr_scoped and IsValid(owner) and owner:IsPlayer() then owner:SetFOV(0, 0.1) end
        self.gr_scoped = false
    end

    function SWEP:Reload()
        if self:Clip1() >= self.Primary.ClipSize then return end
        Unscope(self)
        self:DefaultReload(ACT_VM_RELOAD)
    end

    function SWEP:Holster()
        Unscope(self)
        return true
    end

    weapons.Register(SWEP, gun.class)
end
