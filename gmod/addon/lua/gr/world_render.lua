-- GMod's own things in RDR2's world (milestone 9), client side: props, NPCs, ragdolls,
-- dropped weapons and, in third person, the player, drawn into the overlay so they stand in
-- RDR2's world, lit by RDR2's light and hidden behind RDR2's ground, walls and people.
--
-- The overlay already wipes GMod's own world (overlay.lua): the opaque pass is skipped and
-- the frame cleared when the translucent pass starts. Right after that clear this draws:
--   1. RDR2's surfaces into the depth buffer only: the terrain chunks with their walls
--      (the same data GMod's physics uses, terrain.lua) and a box for each person, animal,
--      horse and vehicle proxy. Nothing of them is seen; they only hide what is behind them.
--   2. GMod's entities, with the viewmodel's RDR2 lighting.
-- No hook in RDR2 and no copy of its depth buffer: the occluders are GMod's own copy of the
-- world near the player, so far-away RDR2 scenery (beyond the chunks, ~50 m) hides nothing.

local GR = GR
GR.WorldRender = GR.WorldRender or {}
local WR = GR.WorldRender

local enabled = CreateClientConVar("gr_world", "1", true, false,
    "Garry's Redemption: 1 = GMod's props, NPCs and (in third person) the player are drawn in RDR2's world")
local occlude = CreateClientConVar("gr_world_occlude", "1", true, false,
    "Garry's Redemption: 1 = RDR2's ground, walls and people hide GMod's things behind them; 2 = and show them (debug)")

local SLOTS = 64             -- GR_MAX_CHUNKS
local BUILDS_PER_FRAME = 4
-- Arthur is 1.85 m; GMod's player models are 72 units, 1.37 m.
local PLAYER_SCALE = 1.85 / 1.37
-- Not drawn: RDR2's stand-ins, and what GMod draws elsewhere (viewmodel, hands, beam).
local SKIP = {
    gr_proxy = true, gr_chunk = true, viewmodel = true, gmod_hands = true, physgun_beam = true,
    player = true, worldspawn = true, predicted_viewmodel = true,
    -- Glowing things that draw themselves in the translucent pass, as the physgun's beam
    -- does (overlay.lua keeps their alpha out of the frame there). Drawn here as props they
    -- got a solid black square around the glow (user report: the AR2's energy ball).
    prop_combine_ball = true, env_sprite = true, env_spritetrail = true, env_laserdot = true,
}
local KIND_PERSON = 1
local PERSON_RADIUS = 9      -- units: a body, not RDR2's arms-out box (as gr_proxy's prism)
local DEPTH_COLOR = Color(255, 255, 255, 255)
-- A plain unlit material; its colour is never written. Two-sided: the chunk triangles wind
-- one way, and seen from above the ground was culled (seen with gr_world_occlude 2: only
-- slopes above the eye showed, and a prop half in the ground showed whole).
local DEPTH_MATERIAL = CreateMaterial("gr_occluder2", "UnlitGeneric", {
    ["$basetexture"] = "color/white",
    ["$vertexcolor"] = 1,
    ["$nocull"] = 1,
})

WR.meshes = WR.meshes or {}  -- slot -> { serial, cx, cy, base, origin, mesh, matrix }

-- Keeps one IMesh per terrain slot, built from the same triangles as the physics.
local function UpdateMeshes(Native)
    local origin = Native.OriginSerial()
    local builds = 0
    for slot = 1, SLOTS do
        local serial = Native.ChunkSerial(slot)
        local m = WR.meshes[slot]
        if serial == 0 then
            if m then
                if m.mesh then m.mesh:Destroy() end
                WR.meshes[slot] = nil
            end
        elseif (not m or m.serial ~= serial) and builds < BUILDS_PER_FRAME then
            local s, cx, cy, base, _, _, x, y, z, _, _, verts = Native.ChunkMesh(slot)
            if s then
                builds = builds + 1
                if m and m.mesh then m.mesh:Destroy() end
                local tris = {}
                for i = 1, #verts do tris[i] = { pos = verts[i] } end
                local mesh
                if #tris >= 3 then
                    mesh = Mesh()
                    mesh:BuildFromTriangles(tris)
                end
                local matrix = Matrix()
                matrix:SetTranslation(Vector(x, y, z))
                WR.meshes[slot] = { serial = s, cx = cx, cy = cy, base = base, origin = origin, mesh = mesh,
                    matrix = matrix }
            end
        elseif m and m.origin ~= origin then
            -- The floating origin moved: same chunk, new place.
            m.matrix:SetTranslation(Vector(Native.ChunkPos(m.cx, m.cy, m.base)))
            m.origin = origin
        end
    end
end

local function DrawOccluders(show)
    render.SetMaterial(DEPTH_MATERIAL)
    if not show then
        render.OverrideColorWriteEnable(true, false)
        render.OverrideAlphaWriteEnable(true, false)
    end
    for _, m in pairs(WR.meshes) do
        if m.mesh then
            cam.PushModelMatrix(m.matrix)
            m.mesh:Draw()
            cam.PopModelMatrix()
        end
    end
    for _, ent in ipairs(ents.FindByClass("gr_proxy")) do
        local mins, maxs = ent:OBBMins(), ent:OBBMaxs()
        if ent:GetNW2Int("gr_kind", 0) == KIND_PERSON then
            local cx, cy = (mins.x + maxs.x) / 2, (mins.y + maxs.y) / 2
            local r = math.min((maxs.x - mins.x) / 2, (maxs.y - mins.y) / 2, PERSON_RADIUS)
            mins = Vector(cx - r, cy - r, mins.z)
            maxs = Vector(cx + r, cy + r, maxs.z)
        end
        render.DrawBox(ent:GetPos(), ent:GetAngles(), mins, maxs, DEPTH_COLOR)
    end
    render.OverrideAlphaWriteEnable(false)
    render.OverrideColorWriteEnable(false)
end

-- ---- RDR2's trees and bushes in front of GMod's things (rdr2/src/veil_sync.h)
--
-- RDR2 casts a 4 by 4 grid of rays from the camera to a square in front of each thing and
-- says which were stopped. The same square is drawn here over the thing, taking away what is
-- already in the frame (colour and alpha) by each corner's share, so RDR2's own picture of
-- the bush shows through there. Each corner eases towards its answer: the answers come a few
-- things a frame, and leaves move.
local veil_enabled = CreateClientConVar("gr_world_veil", "1", true, false,
    "Garry's Redemption: 1 = RDR2's trees and bushes hide GMod's things behind them")
local VEIL_GRID = 4          -- GR_VEIL_GRID
local VEIL_MAX = 16          -- GR_MAX_VEILS
local VEIL_MATERIAL = CreateMaterial("gr_veil", "UnlitGeneric", {
    ["$basetexture"] = "color/white",
    ["$vertexcolor"] = 1,
    ["$vertexalpha"] = 1,
    ["$translucent"] = 1,
    ["$nocull"] = 1,
})
WR.veils = WR.veils or {}    -- entity index -> { [1..16] = how hidden each corner is, seen = frame }

local function VeilRequests(list, eye)
    local Native = GR.Native
    if not Native.Veil then return end
    Native.ClearVeils()
    if not veil_enabled:GetBool() then return end
    table.sort(list, function(a, b) return a:GetPos():DistToSqr(eye) < b:GetPos():DistToSqr(eye) end)
    for i = 1, math.min(#list, VEIL_MAX) do
        local ent = list[i]
        local c = ent:WorldSpaceCenter()
        Native.Veil(ent:EntIndex(), c.x, c.y, c.z, math.max(ent:BoundingRadius(), 8))
    end
end

local function DrawVeil(ent, eye)
    local Native = GR.Native
    if not Native.VeilMask or not veil_enabled:GetBool() then return end
    local id = ent:EntIndex()
    local mask = Native.VeilMask(id)
    local v = WR.veils[id]
    if not mask then
        if v then v.seen = FrameNumber() end
        if not v then return end
        mask = v.mask or 0
    end
    if not v then
        if mask == 0 then return end
        v = {}
        for i = 1, VEIL_GRID * VEIL_GRID do v[i] = 0 end
        WR.veils[id] = v
    end
    v.mask = mask
    v.seen = FrameNumber()
    local k = math.min(RealFrameTime() * 8, 1)
    local any = false
    for i = 1, VEIL_GRID * VEIL_GRID do
        local want = bit.band(mask, bit.lshift(1, i - 1)) ~= 0 and 1 or 0
        v[i] = v[i] + (want - v[i]) * k
        if v[i] > 0.01 then any = true end
    end
    if not any then return end

    local centre = ent:WorldSpaceCenter()
    local radius = math.max(ent:BoundingRadius(), 8)
    local dir = centre - eye
    local dist = dir:Length()
    if dist <= radius + 16 then return end
    dir:Mul(1 / dist)
    local right = Vector(dir.y, -dir.x, 0)
    if right:LengthSqr() < 1e-6 then right = Vector(1, 0, 0) end
    right:Normalize()
    local up = right:Cross(dir)
    local front = centre - dir * radius

    render.SetMaterial(VEIL_MATERIAL)
    render.OverrideDepthEnable(true, false)
    render.OverrideAlphaWriteEnable(true, true)
    -- What is there, times (1 - this corner's share), for colour and alpha both.
    render.OverrideBlend(true, BLEND_ZERO, BLEND_ONE_MINUS_SRC_ALPHA, BLENDFUNC_ADD, BLEND_ZERO,
        BLEND_ONE_MINUS_SRC_ALPHA, BLENDFUNC_ADD)
    local n = VEIL_GRID - 1
    local function Corner(col, row)
        local a = (col / n * 2 - 1) * radius
        local b = (row / n * 2 - 1) * radius
        mesh.Position(front + right * a + up * b)
        mesh.Color(255, 255, 255, math.floor(v[row * VEIL_GRID + col + 1] * 255))
        mesh.AdvanceVertex()
    end
    mesh.Begin(MATERIAL_QUADS, n * n)
    for row = 0, n - 1 do
        for col = 0, n - 1 do
            Corner(col, row)
            Corner(col, row + 1)
            Corner(col + 1, row + 1)
            Corner(col + 1, row)
        end
    end
    mesh.End()
    render.OverrideBlend(false)
    render.OverrideAlphaWriteEnable(false)
    render.OverrideDepthEnable(false)
end

-- ---- Shadows on RDR2's ground
--
-- GMod's things cast no shadow in RDR2 and looked pasted on (user report). Each one near
-- the ground gets a soft dark patch under it, laid on RDR2's ground (the module's copy of
-- the terrain) and pushed a little away from the sun: darker in the middle, nothing at the
-- rim. Black with alpha in the overlay darkens RDR2's picture there. Dark out to INNER of
-- the radius and wider than the thing: a patch no bigger than its foot was hidden under it.
local shadow_enabled = CreateClientConVar("gr_world_shadow", "1", true, false,
    "Garry's Redemption: 1 = GMod's things darken RDR2's ground under them")
local SHADOW_SEGMENTS = 12
local SHADOW_REACH = 24      -- units above the ground at which the patch has faded out
local SHADOW_LIFT = 3        -- units above the ground: clear of the depth copy of it
local SHADOW_INNER = 0.55
local SHADOW_MATERIAL = CreateMaterial("gr_shadow", "UnlitGeneric", {
    ["$basetexture"] = "color/white",
    ["$vertexcolor"] = 1,
    ["$vertexalpha"] = 1,
    ["$translucent"] = 1,
    ["$nocull"] = 1,
})
WR.shadows = WR.shadows or {}  -- entity index -> { at, time, centre, rim = {...} }

local function ShadowPoints(ent, id, sun)
    local Native = GR.Native
    local pos = ent:WorldSpaceCenter()
    local s = WR.shadows[id]
    local serial = Native.OriginSerial()
    if s and s.serial == serial and s.at:DistToSqr(pos) < 9 and RealTime() - s.time < 2 then return s end
    local mins, maxs = ent:OBBMins(), ent:OBBMaxs()
    local radius = math.max(math.max(maxs.x - mins.x, maxs.y - mins.y) * 0.5, 8) * 1.7
    -- Away from the light, further the lower it stands.
    local off = Vector(-sun.x, -sun.y, 0) * radius * 0.35
    local cx, cy = pos.x + off.x, pos.y + off.y
    local z = Native.GroundZ(cx, cy)
    s = { at = pos, time = RealTime(), serial = serial, radius = radius }
    if z then
        s.centre = Vector(cx, cy, z + SHADOW_LIFT)
        s.rim, s.inner = {}, {}
        for i = 0, SHADOW_SEGMENTS - 1 do
            local a = i / SHADOW_SEGMENTS * math.pi * 2
            local x, y = cx + math.cos(a) * radius, cy + math.sin(a) * radius
            s.rim[i + 1] = Vector(x, y, (Native.GroundZ(x, y) or z) + SHADOW_LIFT)
            x, y = cx + math.cos(a) * radius * SHADOW_INNER, cy + math.sin(a) * radius * SHADOW_INNER
            s.inner[i + 1] = Vector(x, y, (Native.GroundZ(x, y) or z) + SHADOW_LIFT)
        end
    end
    WR.shadows[id] = s
    return s
end

local function DrawShadow(ent, sun, strength)
    local s = ShadowPoints(ent, ent:EntIndex(), sun)
    s.seen = FrameNumber()
    if not s.centre then return end
    local lo = ent:WorldSpaceAABB()
    local gap = lo.z - (s.centre.z - SHADOW_LIFT)
    local a = strength * (1 - math.Clamp(gap / SHADOW_REACH, 0, 1))
    if a < 0.02 then return end
    local alpha = math.floor(a * 255)
    local function Point(p, a)
        mesh.Position(p) mesh.Color(0, 0, 0, a) mesh.AdvanceVertex()
    end
    mesh.Begin(MATERIAL_TRIANGLES, SHADOW_SEGMENTS * 3)
    for i = 1, SHADOW_SEGMENTS do
        local j = i % SHADOW_SEGMENTS + 1
        Point(s.centre, alpha) Point(s.inner[i], alpha) Point(s.inner[j], alpha)
        Point(s.inner[i], alpha) Point(s.rim[i], 0) Point(s.rim[j], 0)
        Point(s.inner[i], alpha) Point(s.rim[j], 0) Point(s.inner[j], alpha)
    end
    mesh.End()
end

local function DrawShadows(list)
    if not shadow_enabled:GetBool() or not GR.Native.GroundZ then return end
    local dx, dy, dz, r, g, b = GR.Native.Light()
    if not dx then return end
    local sun = Vector(dx, dy, dz)
    sun:Normalize()
    -- Under a sun or moon the patch is a shadow; in shade it is only the dimness under a thing.
    local strength = (r + g + b) > 0.01 and 0.7 or 0.5
    render.SetMaterial(SHADOW_MATERIAL)
    render.OverrideDepthEnable(true, false)
    render.OverrideAlphaWriteEnable(true, true)
    render.OverrideBlend(true, BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA, BLENDFUNC_ADD, BLEND_ONE,
        BLEND_ONE_MINUS_SRC_ALPHA, BLENDFUNC_ADD)
    for _, ent in ipairs(list) do DrawShadow(ent, sun, strength) end
    render.OverrideBlend(false)
    render.OverrideAlphaWriteEnable(false)
    render.OverrideDepthEnable(false)
end

-- Forgets things that are gone.
local function Forget()
    local frame = FrameNumber()
    for id, v in pairs(WR.veils) do
        if (v.seen or 0) < frame - 60 then WR.veils[id] = nil end
    end
    for id, s in pairs(WR.shadows) do
        if (s.seen or 0) < frame - 60 then WR.shadows[id] = nil end
    end
end

local function ShouldDraw(ent, ply)
    if not IsValid(ent) or ent:IsWorld() or SKIP[ent:GetClass()] then return false end
    local model = ent:GetModel()
    if not model or model == "" or string.sub(model, 1, 1) == "*" then return false end  -- brush entities
    if ent:GetNoDraw() then return false end
    -- A held weapon is drawn with its holder; the player's own in third person below.
    if ent:IsWeapon() and IsValid(ent:GetOwner()) then return false end
    if ent:GetOwner() == ply and ent:GetParent() == ply then return false end
    return true
end

-- Where the right hand of the player's model is as it is drawn (scaled about its feet), a
-- little along the aim so the grip sits in the palm. For RDR2's gun in third person.
function WR.Hand(ply)
    local bone = ply:LookupBone("ValveBiped.Bip01_R_Hand")
    local m = bone and ply:GetBoneMatrix(bone)
    if not m then return end
    local feet = ply:GetPos()
    return feet + (m:GetTranslation() - feet) * PLAYER_SCALE + ply:EyeAngles():Forward() * 4
end

local function DrawPlayer(ply)
    if not ply:Alive() then return end
    local m = Matrix()
    m:Scale(Vector(PLAYER_SCALE, PLAYER_SCALE, PLAYER_SCALE))
    ply:EnableMatrix("RenderMultiply", m)
    WR.player_at = ply:GetPos()
    ply:DrawModel()
    ply:DisableMatrix("RenderMultiply")
    -- The weapon rides the hand bone. Works around: drawn as it was, it hung under the arm
    -- (user report): the player is drawn scaled and the weapon was not, so its merged bones
    -- sat where the unscaled hand would be.
    local weapon = ply:GetActiveWeapon()
    -- Not one of RDR2's guns: RDR2 draws its own model of it in this hand (weapon_view.h).
    if IsValid(weapon) and not weapon.GR_Weapon then
        weapon:EnableMatrix("RenderMultiply", m)
        weapon:SetupBones()
        weapon:DrawModel()
        weapon:DisableMatrix("RenderMultiply")
    end
end

-- Works around: the third-person player's head and skin were see-through, the face barely
-- there (user: "the opacity is too low and I can't see the head"). The overlay shows GMod's
-- frame by its alpha, and a model writes its texture's alpha, which on skin and face textures
-- is a shine mask, not coverage. Every pixel an opaque model drew is marked in the stencil
-- buffer as it draws (alpha-tested hair and lashes that were cut away are not), and those
-- pixels then get alpha 1, colour untouched.
local function Opaque(ent)
    if ent:GetColor().a < 255 then return false end
    return not ent.GetRenderGroup or ent:GetRenderGroup() ~= RENDERGROUP_TRANSLUCENT
end

local function MarkBegin()
    render.SetStencilEnable(true)
    render.ClearStencil()
    render.SetStencilWriteMask(1)
    render.SetStencilTestMask(1)
    render.SetStencilReferenceValue(1)
    render.SetStencilCompareFunction(STENCIL_ALWAYS)
    render.SetStencilPassOperation(STENCIL_REPLACE)
    render.SetStencilFailOperation(STENCIL_KEEP)
    render.SetStencilZFailOperation(STENCIL_KEEP)
end

-- The part of the screen `ent` can cover (its box, the player's grown as it is drawn), so
-- the fill below does not run over the whole 4K frame for every prop. The whole screen if a
-- corner is behind the camera.
local function ScreenBox(ent, scale)
    local lo, hi = ent:WorldSpaceAABB()
    if scale then
        local feet = ent:GetPos()
        lo = feet + (lo - feet) * scale
        hi = feet + (hi - feet) * scale
    end
    local x0, y0, x1, y1 = math.huge, math.huge, -math.huge, -math.huge
    for i = 0, 7 do
        local corner = Vector(bit.band(i, 1) ~= 0 and hi.x or lo.x, bit.band(i, 2) ~= 0 and hi.y or lo.y,
            bit.band(i, 4) ~= 0 and hi.z or lo.z)
        local p = corner:ToScreen()
        if not p.visible then return 0, 0, ScrW(), ScrH() end
        x0, y0 = math.min(x0, p.x), math.min(y0, p.y)
        x1, y1 = math.max(x1, p.x), math.max(y1, p.y)
    end
    x0, y0 = math.max(math.floor(x0) - 2, 0), math.max(math.floor(y0) - 2, 0)
    x1, y1 = math.min(math.ceil(x1) + 2, ScrW()), math.min(math.ceil(y1) + 2, ScrH())
    return x0, y0, x1, y1
end

local function MarkEnd(x0, y0, x1, y1)
    if x1 <= x0 or y1 <= y0 then
        render.SetStencilEnable(false)
        return
    end
    render.SetStencilCompareFunction(STENCIL_EQUAL)
    render.SetStencilPassOperation(STENCIL_KEEP)
    render.OverrideColorWriteEnable(true, false)
    render.OverrideAlphaWriteEnable(true, true)
    render.OverrideBlend(true, BLEND_ONE, BLEND_ZERO, BLENDFUNC_ADD, BLEND_ONE, BLEND_ZERO, BLENDFUNC_ADD)
    cam.Start2D()
    surface.SetDrawColor(255, 255, 255, 255)
    surface.DrawRect(x0, y0, x1 - x0, y1 - y0)
    cam.End2D()
    render.OverrideBlend(false)
    render.OverrideAlphaWriteEnable(false)
    render.OverrideColorWriteEnable(false)
    render.SetStencilEnable(false)
end

-- Called by overlay.lua right after it clears the frame for the translucent pass.
function WR.Draw()
    local Native = GR.Native
    if not enabled:GetBool() or not Native or not Native.ChunkMesh then return end
    local ply = LocalPlayer()
    UpdateMeshes(Native)
    if occlude:GetInt() > 0 then DrawOccluders(occlude:GetInt() == 2) end
    local eye = EyePos()
    local list = {}
    for _, ent in ipairs(ents.GetAll()) do
        if ShouldDraw(ent, ply) then list[#list + 1] = ent end
    end
    -- Not while possessing (possess.lua): the player is the RDR2 ped then, which RDR2 draws.
    local third = GR.Third and GR.Third.Active and GR.Third.Active() and ply:Alive()
        and not (GR.Possess and GR.Possess.Of(ply))
    if third then list[#list + 1] = ply end
    VeilRequests(list, eye)
    DrawShadows(list)
    -- Furthest first: a veil takes away whatever is already drawn behind its square.
    table.sort(list, function(a, b) return a:GetPos():DistToSqr(eye) > b:GetPos():DistToSqr(eye) end)
    for _, ent in ipairs(list) do
        if GR.Overlay.BeginLight then GR.Overlay.BeginLight() end
        -- DrawModel leaves out the entity's own colour (the colour tool's).
        local c = ent:GetColor()
        render.SetColorModulation(c.r / 255, c.g / 255, c.b / 255)
        local opaque = Opaque(ent)
        if opaque then MarkBegin() end
        if ent == ply then DrawPlayer(ply) else ent:DrawModel() end
        if opaque then MarkEnd(ScreenBox(ent, ent == ply and PLAYER_SCALE or nil)) end
        render.SetColorModulation(1, 1, 1)
        if GR.Overlay.EndLight then GR.Overlay.EndLight() end
        DrawVeil(ent, eye)
    end
    Forget()
end
