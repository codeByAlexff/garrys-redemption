-- Garry's Redemption: entry point. Everything lives in the one GR table; each feature is
-- a file under lua/gr/.
--
-- This is a shared autorun file rather than autorun/client because the files under
-- lua/gr/ are not in a folder the game sends to the client by itself: include() refuses
-- a client file that was never AddCSLuaFile'd.

-- In load order: bridge.lua starts the bridge and uses the other two.
local FILES = {
    "gr/body.lua",
    "gr/sounds.lua",
    "gr/terrain.lua",
    "gr/input.lua",
    "gr/player.lua",
    "gr/overlay.lua",
    "gr/world_render.lua",
    "gr/thirdperson.lua",
    "gr/quickinfo.lua",
    "gr/bridge.lua",
    "gr/probe.lua",
    "gr/spawn.lua",
    "gr/possess.lua",
    "gr/sweps.lua",
    "gr/health.lua",
    "gr/fly.lua",
    "gr/commands.lua",
}

if SERVER then
    for _, name in ipairs(FILES) do
        AddCSLuaFile(name)
    end
    -- The spawn menu's model list, made by tools/gen_models.py; spawn.lua includes it when
    -- the tab is first opened.
    if file.Exists("gr/models_data.lua", "LUA") then AddCSLuaFile("gr/models_data.lua") end
end

GR = GR or {}

local TAG_COLOR = Color(214, 60, 50)

-- One tagged console line. Used for everything the user is meant to read.
function GR.Print(...)
    MsgC(TAG_COLOR, "[Garry's Redemption] ", color_white, ...)
    MsgC("\n")
end

if SERVER then
    -- The server realm has the player's size and the proxies (the physgun acts here).
    include("gr/body.lua")
    include("gr/sounds.lua")
    include("gr/proxies.lua")
    include("gr/collide.lua")
    include("gr/terrain.lua")
    include("gr/weapons.lua")
    include("gr/tools.lua")
    include("gr/thirdperson.lua")
    include("gr/spawn.lua")
    include("gr/possess.lua")
    include("gr/sweps.lua")
    include("gr/health.lua")
    include("gr/fly.lua")
    include("gr/near.lua")
    include("gr/surfaces.lua")
    include("gr/commands.lua")
    return
end

for _, name in ipairs(FILES) do
    include(name)
end
