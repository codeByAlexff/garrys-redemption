// Every RDR2 native the plugin calls, by hash.
//
// Rule: nothing goes in here from memory. Each entry was looked up in alloc8or's RDR3
// native database (https://alloc8or.re/rdr3/nativedb/, data at
// github.com/alloc8or/rdr3-nativedb-data) and is listed in docs/NATIVES.md with its
// signature and the date it was checked. A name starting with an underscore is one the
// database marks as unofficial (the hash is real, the name is the community's).

#pragma once

#include <cstdint>

namespace gr::native_hash {

// NETWORK
inline constexpr uint64_t NETWORK_IS_IN_SESSION = 0xCA97246103B63917;        // BOOL ()
inline constexpr uint64_t NETWORK_IS_SESSION_STARTED = 0x9DE624D2FC4B603F;   // BOOL ()
inline constexpr uint64_t NETWORK_IS_GAME_IN_PROGRESS = 0x10FAB35428CCC9D7;  // BOOL ()

// MISC
inline constexpr uint64_t GET_GAME_TIMER = 0x4F67E8ECA7D3F667;  // int ()
inline constexpr uint64_t GET_FRAME_TIME = 0x5E72022914CE3C38;  // float ()
inline constexpr uint64_t VAR_STRING = 0xFA925AC00EB830B9;      // const char* (int flags, ...)

inline constexpr uint64_t GET_MODEL_DIMENSIONS = 0xDCB8DDD5D054A7E7;  // void (Hash model, Vector3* min, Vector3* max)
// BOOL (float x, float y, float z, float* groundZ, BOOL p4)
inline constexpr uint64_t GET_GROUND_Z_FOR_3D_COORD = 0x24FA4267BB8D2431;

// STREAMING (milestone 7)
inline constexpr uint64_t REQUEST_MODEL = 0xFA28FE3A6246FC30;                  // void (Hash model, BOOL p1)
inline constexpr uint64_t HAS_MODEL_LOADED = 0x1283B8B89DD5D1B6;               // BOOL (Hash model)
inline constexpr uint64_t SET_MODEL_AS_NO_LONGER_NEEDED = 0x4AD96EF928BD4F9A;  // void (Hash model)
inline constexpr uint64_t IS_MODEL_IN_CDIMAGE = 0xD6F3B6D7716CFF8E;            // BOOL (Hash model)
inline constexpr uint64_t IS_MODEL_A_PED = 0xC3F09DE9D6D17DDA;                 // BOOL (Hash model)
inline constexpr uint64_t IS_MODEL_A_VEHICLE = 0x354F62672DE7DB0A;             // BOOL (Hash model)

// LAW (milestone 7: the sandbox's amnesty)
inline constexpr uint64_t CLEAR_WANTED_SCORE = 0x062B4A4A3396351D;  // void (Player player)
inline constexpr uint64_t CLEAR_BOUNTY = 0xC76F252371150D9A;        // void (Player player)
inline constexpr uint64_t GET_BOUNTY = 0x54310AAB97B92816;          // int (Player player)
// _SET_BOUNTY_HUNTER_PURSUIT_CLEARED: void (). "Force clears local player's wanted level".
inline constexpr uint64_t SET_BOUNTY_HUNTER_PURSUIT_CLEARED = 0x55F37F5F3F2475E1;

// SHAPETEST
// ScrHandle (float x1, float y1, float z1, float x2, float y2, float z2, int flags,
//            Entity entityToIgnore, int p8). Blocks until done, so the result is ready at once.
inline constexpr uint64_t START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE = 0x377906D8A31E5586;
// int (ScrHandle handle, BOOL* hit, Vector3* endCoords, Vector3* surfaceNormal, Entity* entityHit):
// 0 invalid handle, 1 pending, 2 done
inline constexpr uint64_t GET_SHAPE_TEST_RESULT = 0xEDE8AC7C5108FB1D;

// CLOCK
inline constexpr uint64_t GET_CLOCK_HOURS = 0xC82CF208C2B19199;    // int ()
inline constexpr uint64_t GET_CLOCK_MINUTES = 0x4E162231B823DBBF;  // int ()
inline constexpr uint64_t GET_CLOCK_SECONDS = 0xB6101ABE62B5F080;  // int ()

// INTERIOR
inline constexpr uint64_t GET_INTERIOR_FROM_ENTITY = 0xB417689857646F61;  // Interior (Entity entity), 0 outside

// PLAYER
inline constexpr uint64_t PLAYER_PED_ID = 0x096275889B8E0EE0;         // Ped ()
inline constexpr uint64_t PLAYER_ID = 0x217E9DC48139933D;             // Player ()
inline constexpr uint64_t IS_PLAYER_CONTROL_ON = 0x7964097FCE4C244B;  // BOOL (Player player)

// ENTITY
inline constexpr uint64_t GET_ENTITY_COORDS = 0xA86D5F069399F44D;  // Vector3 (Entity entity, BOOL alive, BOOL realCoords)
inline constexpr uint64_t GET_ENTITY_ROTATION = 0xE09CAF86C32CB48F;  // Vector3 (Entity entity, int rotationOrder)
inline constexpr uint64_t GET_ENTITY_HEADING = 0xC230DD956E2F5507;  // float (Entity entity)
inline constexpr uint64_t GET_ENTITY_FORWARD_VECTOR = 0x2412D9C05BB09B97;  // Vector3 (Entity entity)
inline constexpr uint64_t GET_ENTITY_MODEL = 0xDA76A9F39210D365;  // Hash (Entity entity)
inline constexpr uint64_t GET_ENTITY_HEIGHT_ABOVE_GROUND = 0x0D3B5BAEA08F63E9;  // float (Entity entity)
inline constexpr uint64_t IS_ENTITY_DEAD = 0x7D5B1F88E7504BBA;     // BOOL (Entity entity)
inline constexpr uint64_t IS_ENTITY_VISIBLE = 0xFFC96ECB7FA404CA;  // BOOL (Entity entity)
inline constexpr uint64_t GET_ENTITY_COLLISION_DISABLED = 0xAA2FADD30F45A9DA;  // BOOL (Entity entity)
inline constexpr uint64_t SET_ENTITY_VISIBLE = 0x1794B4FCC84D812F;  // void (Entity entity, BOOL toggle)
inline constexpr uint64_t FREEZE_ENTITY_POSITION = 0x7D9EFB7AD6B19754;  // void (Entity entity, BOOL toggle)
inline constexpr uint64_t SET_ENTITY_HEADING = 0xCF2B9C0645C4651B;  // void (Entity entity, float heading)
// void (Entity entity, float x, float y, float z, BOOL xAxis, BOOL yAxis, BOOL zAxis)
inline constexpr uint64_t SET_ENTITY_COORDS_NO_OFFSET = 0x239A3351AC1DA385;
inline constexpr uint64_t DOES_ENTITY_EXIST = 0xD42BD6EB2E0F1677;     // BOOL (Entity entity)
inline constexpr uint64_t GET_ENTITY_VELOCITY = 0x4805D2B1D8CF94A9;   // Vector3 (Entity entity, int p1)
inline constexpr uint64_t SET_ENTITY_VELOCITY = 0x1C99BB7B6E96D16F;   // void (Entity entity, float x, float y, float z)
inline constexpr uint64_t GET_ENTITY_HEALTH = 0x82368787EA73C0F7;     // int (Entity entity)
// void (Entity entity, int healthAmount, Entity entityKilledBy)
inline constexpr uint64_t SET_ENTITY_HEALTH = 0xAC2767ED8BDFAB15;
inline constexpr uint64_t IS_ENTITY_A_PED = 0xCF8176912DDA4EA5;      // BOOL (Entity entity)
inline constexpr uint64_t IS_ENTITY_A_VEHICLE = 0xC3D96AF45FCCEC4C;  // BOOL (Entity entity)
// void (Entity entity, float pitch, float roll, float yaw, int rotationOrder, BOOL p5)
inline constexpr uint64_t SET_ENTITY_ROTATION = 0x9CC8314DFEDE441E;
// void (Entity entity, BOOL scriptHostObject, BOOL grabFromOtherScript)
inline constexpr uint64_t SET_ENTITY_AS_MISSION_ENTITY = 0xDC19C288082E586E;
inline constexpr uint64_t DELETE_ENTITY = 0x4CD38C78BD19A497;  // void (Entity* entity)
inline constexpr uint64_t ACTIVATE_PHYSICS = 0x710311ADF0E20730;  // PHYSICS: void (Entity entity)

// Creation (milestone 7)
// Ped (Hash modelHash, float x, float y, float z, float heading, BOOL isNetwork, BOOL bScriptHostPed,
//      BOOL p7, BOOL p8)
inline constexpr uint64_t CREATE_PED = 0xD49F9B0955C367DE;
// _SET_RANDOM_OUTFIT_VARIATION: void (Ped ped, BOOL p1). A ped made by script has no
// outfit, and is invisible, until it gets one.
inline constexpr uint64_t SET_RANDOM_OUTFIT_VARIATION = 0x283978A15512B2FE;
// Vehicle (Hash modelHash, float x, float y, float z, float heading, BOOL isNetwork,
//          BOOL bScriptHostVeh, BOOL bDontAutoCreateDraftAnimals, BOOL p8)
inline constexpr uint64_t CREATE_VEHICLE = 0xAF35D0D2583051B0;
inline constexpr uint64_t SET_VEHICLE_ON_GROUND_PROPERLY = 0x7263332501E07F52;  // BOOL (Vehicle vehicle, BOOL p1)
// _GET_PED_IN_DRAFT_HARNESS: Ped (Vehicle vehicle, int harnessId)
inline constexpr uint64_t GET_PED_IN_DRAFT_HARNESS = 0xA8BA0BAE0173457B;
// Object (Hash modelHash, float x, float y, float z, BOOL isNetwork, BOOL bScriptHostObj,
//         BOOL dynamic, BOOL p7, BOOL p8)
inline constexpr uint64_t CREATE_OBJECT = 0x509D5878EB39E842;

// PED
inline constexpr uint64_t IS_PED_ON_MOUNT = 0x460BC76A0E10655E;        // BOOL (Ped ped)
inline constexpr uint64_t IS_PED_IN_ANY_VEHICLE = 0x997ABD671D25CA0B;  // BOOL (Ped ped, BOOL atGetIn)
inline constexpr uint64_t IS_PED_HUMAN = 0xB980061DA992779D;           // BOOL (Ped ped)
inline constexpr uint64_t IS_THIS_MODEL_A_HORSE = 0x772A1969F649E902;  // _IS_THIS_MODEL_A_HORSE: BOOL (Hash model)
inline constexpr uint64_t IS_PED_RAGDOLL = 0x47E4E977581C5B55;         // BOOL (Ped ped)
// BOOL (Ped ped, int timeMin, int timeMax, int ragdollType, BOOL abortIfInjured, BOOL abortIfDead,
//       const char* nmTaskMessageParameterName)
inline constexpr uint64_t SET_PED_TO_RAGDOLL = 0xAE99FB955581844A;
inline constexpr uint64_t RESET_PED_RAGDOLL_TIMER = 0x9FA4664CF62E47E8;  // void (Ped ped)
inline constexpr uint64_t SET_PED_CAN_RAGDOLL = 0xB128377056A54E2A;      // void (Ped ped, BOOL toggle)
inline constexpr uint64_t RESURRECT_PED = 0x71BC8E838B9C6035;            // void (Ped ped)
inline constexpr uint64_t SET_ENTITY_DYNAMIC = 0xFBFC4473F66CE344;       // void (Entity entity, BOOL toggle)
inline constexpr uint64_t SET_ENTITY_HAS_GRAVITY = 0x0CEDB728A1083FA7;   // void (Entity entity, BOOL toggle)
inline constexpr uint64_t SET_BOAT_ANCHOR = 0xAEAB044F05B92659;          // void (Vehicle vehicle, BOOL toggle)
inline constexpr uint64_t IS_ENTITY_FROZEN = 0x083D497D57B7400F;         // _IS_ENTITY_FROZEN: BOOL (Entity entity)
inline constexpr uint64_t IS_ENTITY_IN_WATER = 0xDDE5C125AC446723;       // BOOL (Entity entity)
// void (Object object, BOOL toggle)
inline constexpr uint64_t SET_ACTIVATE_OBJECT_PHYSICS_AS_SOON_AS_IT_IS_UNFROZEN = 0x406137F8EF90EAF5;
inline constexpr uint64_t SET_PED_GRAVITY = 0x9FF447B6B6AD960A;          // void (Ped ped, BOOL toggle)
// void (Ped ped, BOOL p1, BOOL resetCrouch): stops whatever the ped is doing at once
inline constexpr uint64_t CLEAR_PED_TASKS_IMMEDIATELY = 0xAAA34F8A7CB32098;
inline constexpr uint64_t IS_PED_USING_ANY_SCENARIO = 0x57AB4A3080F85143;  // BOOL (Ped ped)
// Entities near a point, for when Script Hook's pool walk answers nothing (world_sync.h).
// int (float x, float y, float z, float radius, ItemSet itemSet, int p5): p5 1 peds, 2 vehicles,
// 3 objects, 0 nothing (measured 2026-10-03 in Strawberry; the itemset held 48 of 256 objects)
inline constexpr uint64_t GET_ENTITIES_NEAR_POINT = 0x59B57C4B06531E1E;  // _GET_ENTITIES_NEAR_POINT
inline constexpr uint64_t CREATE_ITEMSET = 0xA1AF16083320065A;           // ItemSet (BOOL p0)
inline constexpr uint64_t IS_ITEMSET_VALID = 0xD30765D153EF5C76;         // BOOL (ItemSet itemset)
inline constexpr uint64_t CLEAR_ITEMSET = 0x20A4BF0E09BEE146;            // _CLEAR_ITEMSET: void (ItemSet itemset)
inline constexpr uint64_t GET_ITEMSET_SIZE = 0x55F2E375AC6018A9;         // int (ItemSet itemset)
inline constexpr uint64_t GET_INDEXED_ITEM_IN_ITEMSET = 0x275A2E2C0FAB7612;  // ScrHandle (int index, ItemSet itemset)
inline constexpr uint64_t IS_ENTITY_AN_OBJECT = 0x0A27A546A375FDEF;      // BOOL (Entity entity)
inline constexpr uint64_t GET_PED_MAX_HEALTH = 0x4700A416E8324EF3;  // int (Ped ped)
// The possess tool (possess_sync.h).
inline constexpr uint64_t SET_BLOCKING_OF_NON_TEMPORARY_EVENTS = 0x9F8AA94D6D97DBF4;  // void (Ped ped, BOOL toggle)
inline constexpr uint64_t CLEAR_PED_TASKS = 0xE1EF3C1216AFF2CD;   // void (Ped ped, BOOL p1, BOOL p2)
inline constexpr uint64_t TASK_STAND_STILL = 0x919BE13EED931959;  // void (Ped ped, int time)
inline constexpr uint64_t TASK_JUMP = 0x0AE4086104E067B1;         // void (Ped ped, BOOL unused)
// void (Ped ped, float x, float y, float z, float moveBlendSpeedY, int timeBeforeTeleport,
//       float finalHeading, float targetRadius, int p8): no navmesh; -1 never teleports
inline constexpr uint64_t TASK_GO_STRAIGHT_TO_COORD = 0xD76B57B44F1E6F8B;
inline constexpr uint64_t IS_WEAPON_VALID = 0x937C71165CF334B3;   // BOOL (Hash weaponHash)
// RDR2's gun in the player's view (weapon_view.h). Names with an underscore in the database.
inline constexpr uint64_t REQUEST_WEAPON_ASSET = 0x72D4CB5DB927009C;     // void (Hash weaponHash, int p1, BOOL p2)
inline constexpr uint64_t HAS_WEAPON_ASSET_LOADED = 0xFF07CF465F48B830;  // BOOL (Hash weaponHash)
inline constexpr uint64_t REMOVE_WEAPON_ASSET = 0xC3896D03E2852236;      // void (Hash weaponHash)
// Object (Hash weaponHash, int ammoCount, float x, float y, float z, BOOL showWorldModel, float scale)
inline constexpr uint64_t CREATE_WEAPON_OBJECT = 0x9888652B8BA77F73;
inline constexpr uint64_t SET_ENTITY_COLLISION = 0xF66F820909453B8C;  // void (Entity entity, BOOL toggle, BOOL keepPhysics)
// void (Ped ped, int damageAmount, BOOL damageArmour, int boneId, Ped pedKiller)
inline constexpr uint64_t APPLY_DAMAGE_TO_PED = 0x697157CED63F18D4;
// Vector3 (Ped ped, int boneId, float offsetX, float offsetY, float offsetZ). boneId is the
// bone's id, not its index.
inline constexpr uint64_t GET_PED_BONE_COORDS = 0x17C07FC640E86B4E;

// MISC, weapons
// void (float x1, float y1, float z1, float x2, float y2, float z2, int damage, BOOL p7,
//       Hash weaponHash, Ped ownerPed, BOOL isAudible, BOOL isInvisible, float speed, BOOL p13)
inline constexpr uint64_t SHOOT_SINGLE_BULLET_BETWEEN_COORDS = 0x867654CBC7606F2C;

// ENTITY, attachments (milestone 8)
// void (Entity entity1, Entity entity2, int boneIndex, float xPos, float yPos, float zPos,
//       float xRot, float yRot, float zRot, BOOL p9, BOOL useSoftPinning, BOOL collision,
//       BOOL isPed, int vertexIndex, BOOL fixedRot, BOOL p15, BOOL p16)
inline constexpr uint64_t ATTACH_ENTITY_TO_ENTITY = 0x6B9BBD38AB0796DF;
inline constexpr uint64_t DETACH_ENTITY = 0x64CDE9D6BF8ECAD3;      // void (Entity entity, BOOL p1, BOOL collision)
inline constexpr uint64_t IS_ENTITY_ATTACHED = 0xEE6AD63ABF59C0B7;  // BOOL (Entity entity)
// Vector3 (Entity entity, float posX, float posY, float posZ): a world point in the entity's space
inline constexpr uint64_t GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS = 0x497C6B1A2C9AE69C;
// Vector3 (Entity entity, float offsetX, float offsetY, float offsetZ): x right, y forward, z up
inline constexpr uint64_t GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS = 0x1899F328B0E12848;
// void (Entity entity1, Entity entity2, BOOL thisFrameOnly)
inline constexpr uint64_t SET_ENTITY_NO_COLLISION_ENTITY = 0xE037BF068223C38D;

// PHYSICS, ropes
// _ADD_ROPE_2: int (float x, float y, float z, float rotX, float rotY, float rotZ, float length,
//                  int ropeType, BOOL isNetworked, int p9, float p10)
inline constexpr uint64_t ADD_ROPE_2 = 0xE9C59F6809373A99;
// _ATTACH_ENTITIES_TO_ROPE_2: void (int ropeId, Entity entity1, Entity entity2, float ent1X,
//     float ent1Y, float ent1Z, float ent2X, float ent2Y, float ent2Z, const char* boneName1,
//     const char* boneName2)
inline constexpr uint64_t ATTACH_ENTITIES_TO_ROPE_2 = 0x462FF2A432733A44;
inline constexpr uint64_t DELETE_ROPE = 0x52B4829281364649;      // void (int* ropeId)
inline constexpr uint64_t DOES_ROPE_EXIST = 0xFD5448BE3111ED96;  // BOOL (int ropeId)

// FIRE
// void (Ped ped, float x, float y, float z, int explosionType, float damageScale, BOOL isAudible,
//       BOOL isInvisible, float cameraShake)
inline constexpr uint64_t ADD_OWNED_EXPLOSION = 0xD84A917A64D4D016;

// CAMERA
inline constexpr uint64_t CREATE_CAM = 0xE72CDBA7F0A02DD6;      // Cam (const char* camName, BOOL p1)
inline constexpr uint64_t DESTROY_CAM = 0x4E67E0B6D7FD5145;     // void (Cam cam, BOOL p1)
inline constexpr uint64_t DOES_CAM_EXIST = 0x153AD457764FD704;  // BOOL (Cam cam)
inline constexpr uint64_t SET_CAM_ACTIVE = 0x87295BCA613800C8;  // void (Cam cam, BOOL active)
inline constexpr uint64_t SET_CAM_COORD = 0xF9EE7D419EE49DE6;   // void (Cam cam, float x, float y, float z)
inline constexpr uint64_t SET_CAM_ROT = 0x63DFA6810AD78719;  // void (Cam cam, float rotX, float rotY, float rotZ, int rotationOrder)
inline constexpr uint64_t SET_CAM_FOV = 0x27666E5988D9D429;  // void (Cam cam, float fov), 1 to 130
// void (Cam cam, float x, y, z, float rotX, rotY, rotZ, float fov, Any p8, int graphType1,
//       int graphType2, int rotationOrder, Any p12, Any p13)
inline constexpr uint64_t SET_CAM_PARAMS = 0xA47BBFFFB83D4D0A;
// void (Cam cam, Entity entity, float xOffset, float yOffset, float zOffset, BOOL isRelative)
inline constexpr uint64_t ATTACH_CAM_TO_ENTITY = 0xFDC0DF7F6FB0A592;
inline constexpr uint64_t DETACH_CAM = 0x05B41DDBEB559556;  // void (Cam cam)
// void (Entity entity, int alphaLevel, BOOL skin)
inline constexpr uint64_t SET_ENTITY_ALPHA = 0x0DF7692B1D9E7BA7;
inline constexpr uint64_t RESET_ENTITY_ALPHA = 0x744B9EF44779D9AB;  // void (Entity entity)
inline constexpr uint64_t SET_PED_ALL_WEAPONS_VISIBILITY = 0x4F806A6CFED89468;  // _SET_PED_ALL_WEAPONS_VISIBILITY: void (Ped ped, BOOL visible)
inline constexpr uint64_t GET_PLAYER_INVINCIBLE = 0x0CBBCB2CCFA7DC4E;  // BOOL (Player player)
inline constexpr uint64_t SET_PLAYER_INVINCIBLE = 0xFEBEEBC9CBDF4B12;  // void (Player player, BOOL toggle)
inline constexpr uint64_t GET_ENTITY_CAN_BE_DAMAGED = 0x75DF9E73F2F005FD;  // BOOL (Entity entity)
inline constexpr uint64_t SET_ENTITY_CAN_BE_DAMAGED = 0x0D06D522B90E861F;  // void (Entity entity, BOOL toggle)
// void (BOOL render, BOOL ease, int easeTime, BOOL p3, BOOL p4, int renderingFlags)
inline constexpr uint64_t RENDER_SCRIPT_CAMS = 0x33281167E4942E4F;
inline constexpr uint64_t GET_GAMEPLAY_CAM_COORD = 0x595320200B98596E;      // Vector3 ()
inline constexpr uint64_t GET_GAMEPLAY_CAM_ROT = 0x0252D2B5582957A6;        // Vector3 (int rotationOrder)
inline constexpr uint64_t GET_FINAL_RENDERED_CAM_FOV = 0x04AF77971E508F6A;  // float ()

// PAD. `control` is 0 for everything the plugin does (the player's own controls).
inline constexpr uint64_t DISABLE_ALL_CONTROL_ACTIONS = 0x5F4B6931816E599B;  // void (int control)
inline constexpr uint64_t ENABLE_CONTROL_ACTION = 0x351220255D64C155;  // void (int control, Hash action, BOOL enableRelatedActions)

// HUD
inline constexpr uint64_t IS_PAUSE_MENU_ACTIVE = 0x535384D6067BA42E;  // BOOL ()
inline constexpr uint64_t SHOW_PLAYER_CORES = 0x50C803A4CD5932C5;  // _SHOW_PLAYER_CORES: void (BOOL state)
inline constexpr uint64_t SHOW_HORSE_CORES = 0xD4EE21B7CC7FD350;   // _SHOW_HORSE_CORES: void (BOOL state)

// MAP
inline constexpr uint64_t DISPLAY_RADAR = 0x1B3DA717B9AFF828;  // void (BOOL toggle): the minimap

// UIDEBUG (all three exist from game build 1355)
inline constexpr uint64_t BG_DISPLAY_TEXT = 0x16794E044C9EFB58;    // void (const char* text, float x, float y)
inline constexpr uint64_t BG_SET_TEXT_SCALE = 0xA1253A3C870B6843;  // void (float scaleX, float scaleY)
inline constexpr uint64_t BG_SET_TEXT_COLOR = 0x16FA5CE47F184F1E;  // void (int r, int g, int b, int a)

}  // namespace gr::native_hash

// Control actions, for PAD natives. Not natives: these are the joaat hash of the name,
// checked against the list in github.com/femga/rdr3_discoveries (Controls), which the
// native database links to from _SET_CONTROL_CONTEXT.
namespace gr::control_hash {

inline constexpr uint32_t INPUT_FRONTEND_PAUSE = 0xD82E0BD2;            // default key P
inline constexpr uint32_t INPUT_FRONTEND_PAUSE_ALTERNATE = 0x4A903C11;  // default key Esc
inline constexpr uint32_t INPUT_MAP = 0xE31C6A41;                       // default key M

}  // namespace gr::control_hash

// Weapons and bones. Not natives: weapon hashes are the joaat hash of the name, computed and
// checked against github.com/femga/rdr3_discoveries weapons/weapons.lua; bone ids are from
// its boneNames/mp_male__boneNames.lua (ids are hashes of the bone's name, the same for
// every human skeleton).
namespace gr::weapon_hash {

inline constexpr uint32_t WEAPON_REVOLVER_CATTLEMAN = 0x169F59F7;
inline constexpr uint32_t WEAPON_SHOTGUN_PUMP = 0x31B7B9FE;

}  // namespace gr::weapon_hash

namespace gr::bone_id {

inline constexpr int SKEL_HEAD = 21030;
inline constexpr int SKEL_NECK0 = 14283;
inline constexpr int SKEL_SPINE6 = 14416;
inline constexpr int SKEL_SPINE3 = 14413;
inline constexpr int SKEL_SPINE0 = 14410;
inline constexpr int SKEL_PELVIS = 56200;
inline constexpr int SKEL_L_THIGH = 65478;
inline constexpr int SKEL_R_THIGH = 6884;
inline constexpr int SKEL_L_CALF = 55120;
inline constexpr int SKEL_R_CALF = 43312;
inline constexpr int SKEL_L_UPPERARM = 37873;
inline constexpr int SKEL_R_UPPERARM = 46065;
inline constexpr int SKEL_L_FOREARM = 53675;
inline constexpr int SKEL_R_FOREARM = 54187;

}  // namespace gr::bone_id

// eExplosionTag, from the native database's ADD_EXPLOSION notes (and femga's
// graphics/explosions/README.md).
namespace gr::explosion_tag {

inline constexpr int EXP_TAG_DYNAMITE = 25;

}  // namespace gr::explosion_tag
