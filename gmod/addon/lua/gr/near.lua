-- RDR2's solid things right around the player, solid in GMod too (server side).
--
-- The terrain chunks (terrain.lua) hold RDR2's ground and the map's walls, and the player
-- walked through what they miss: trees, posts, thin rocks and RDR2's objects (user: "add
-- better collision from the gmod character and the rdr2 world"). RDR2 now finds those with a
-- ring of short rays around the player every frame (rdr2/src/near_sync.h) and sends each as a
-- panel; here each panel is an invisible box (gr_blocker), so GMod's own movement stops and
-- slides along them as it does along any wall.

local GR = GR
GR.Near = GR.Near or {}
local Near = GR.Near

local enabled = CreateConVar("gr_near_collide", "1", FCVAR_ARCHIVE,
    "Garry's Redemption: 1 = trees, posts, rocks and RDR2's objects near the player are solid in GMod")

local DEPTH = 6 -- units: how far each box reaches behind the surface

Near.pool = Near.pool or {}

local function Blocker(i)
    local ent = Near.pool[i]
    if IsValid(ent) then return ent end
    ent = ents.Create("gr_blocker")
    if not IsValid(ent) then return end
    ent:Spawn()
    Near.pool[i] = ent
    return ent
end

-- Whether the player's box already overlaps `ent`. A box made solid around the player gets
-- it stuck: such a one is left out until the player is clear of it.
local function Overlaps(ply, ent)
    local pos = ply:GetPos()
    local tr = util.TraceHull({
        start = pos, endpos = pos, mins = ply:OBBMins(), maxs = ply:OBBMaxs(),
        filter = function(e) return e == ent end, ignoreworld = true,
    })
    return tr.Hit or tr.StartSolid
end

hook.Add("Tick", "GR.Near", function()
    local Native = GR.Native
    if not Native or not Native.Near then return end
    local ply = player.GetAll()[1]
    local n = 0
    if enabled:GetBool() and IsValid(ply) and ply:Alive() and ply:GetMoveType() ~= MOVETYPE_NOCLIP then
        n = Native.Near()
    end
    local used = 0
    for i = 1, n do
        local x, y, z, nx, ny, hw, hh = Native.NearPanel(i)
        local ent = x and Blocker(i)
        if ent then
            local normal = Vector(nx, ny, 0)
            ent:SetPos(Vector(x, y, z) - normal * (DEPTH / 2))
            ent:SetAngles(normal:Angle())
            ent:SetCollisionBounds(Vector(-DEPTH / 2, -hw, -hh), Vector(DEPTH / 2, hw, hh))
            ent:SetNotSolid(false)
            if Overlaps(ply, ent) then ent:SetNotSolid(true) end
            used = i
        end
    end
    for i = used + 1, #Near.pool do
        local ent = Near.pool[i]
        if IsValid(ent) then ent:SetNotSolid(true) end
    end
    Near.used = used
end)

concommand.Add("gr_near_status", function(ply)
    if IsValid(ply) and not ply:IsListenServerHost() then return end
    local solid = 0
    for _, ent in ipairs(Near.pool) do
        if IsValid(ent) and ent:IsSolid() then solid = solid + 1 end
    end
    GR.Print(string.format("near: %s, %d panels from RDR2, %d solid", enabled:GetBool() and "on" or "off",
        Near.used or 0, solid))
end, nil, "Garry's Redemption: show RDR2's solid things around the player as GMod has them")
