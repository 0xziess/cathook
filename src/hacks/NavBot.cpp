#include "common.hpp"
#include "navparser.hpp"
#include "NavBot.hpp"
#include "PlayerTools.hpp"
#include "Aimbot.hpp"
#include "FollowBot.hpp"

namespace hacks::tf2::NavBot
{
// -Rvars-
static settings::Bool enabled("navbot.enabled", "false");
static settings::Bool stay_near("navbot.stay-near", "true");
static settings::Bool heavy_mode("navbot.other-mode", "false");
static settings::Bool spy_mode("navbot.spy-mode", "false");
static settings::Bool get_health("navbot.get-health-and-ammo", "true");
static settings::Float jump_distance("navbot.autojump.trigger-distance", "300");
static settings::Bool autojump("navbot.autojump.enabled", "false");
static settings::Bool primary_only("navbot.primary-only", "true");
static settings::Int spy_ignore_time("navbot.spy-ignore-time", "5000");

// -Forward declarations-
bool init(bool first_cm);
static bool navToSniperSpot();
static bool stayNear();
static bool getHealthAndAmmo();
static void autoJump();
static void updateSlot();
using task::current_task;

// -Variables-
static std::vector<std::pair<CNavArea *, Vector>> sniper_spots;
// How long should the bot wait until pathing again?
static Timer wait_until_path{};
// Time before following target cloaked spy again
static std::array<Timer, 33> spy_cloak{};
// What is the bot currently doing
namespace task
{
task current_task;
}
constexpr bot_class_config DIST_SPY{ 300.0f, 500.0f, 1000.0f };
constexpr bot_class_config DIST_OTHER{ 100.0f, 200.0f, 300.0f };
constexpr bot_class_config DIST_SNIPER{ 1000.0f, 1500.0f, 3000.0f };

static void CreateMove()
{
    if (CE_BAD(LOCAL_E) || !LOCAL_E->m_bAlivePlayer() || !LOCAL_E->m_bAlivePlayer())
        return;
    if (!init(false))
        return;
    if (!nav::ReadyForCommands || current_task == task::followbot)
        wait_until_path.update();
    else
        current_task = task::none;

    if (autojump)
        autoJump();
    if (primary_only)
        updateSlot();

    if (get_health)
        if (getHealthAndAmmo())
            return;
    // Try to stay near enemies to increase efficiency
    if ((stay_near || heavy_mode || spy_mode) && current_task != task::followbot)
        if (stayNear())
            return;
    // We don't have anything else to do. Just nav to sniper spots.
    if (navToSniperSpot())
        return;
    // Uhh... Just stand around I guess?
}

bool init(bool first_cm)
{
    static bool inited = false;
    if (first_cm)
        inited = false;
    if (!enabled)
        return false;
    if (!nav::prepare())
        return false;
    if (!inited)
    {
        sniper_spots.clear();
        // Add all sniper spots to vector
        for (auto &area : nav::navfile->m_areas)
        {
            for (auto hide : area.m_hidingSpots)
                if (hide.IsGoodSniperSpot() || hide.IsIdealSniperSpot() || hide.IsExposed())
                    sniper_spots.emplace_back(&area, hide.m_pos);
        }
        inited = true;
    }
    return true;
}

static bool navToSniperSpot()
{
    // Don't path if you already have commands. But also don't error out.
    if (!nav::ReadyForCommands || current_task != task::none)
        return true;
    // Wait arround a bit before pathing again
    if (!wait_until_path.check(2000))
        return false;
    // Max 10 attempts
    for (int attempts = 0; attempts < 10; attempts++)
    {
        // Get a random sniper spot
        auto random = select_randomly(sniper_spots.begin(), sniper_spots.end());
        // Check if spot is considered safe (no sentry, no sticky)
        if (!nav::isSafe(random.base()->first))
            continue;
        // Try to nav there
        if (nav::navTo(random.base()->second, 5, true, true, false))
        {
            current_task = task::sniper_spot;
            return true;
        }
    }
    return false;
}

static std::pair<CachedEntity *, float> getNearestPlayerDistance()
{
    float distance         = FLT_MAX;
    CachedEntity *best_ent = nullptr;
    for (int i = 1; i < g_IEngine->GetMaxClients(); i++)
    {
        CachedEntity *ent = ENTITY(i);
        if (CE_GOOD(ent) && ent->m_bAlivePlayer() && ent->m_bEnemy() && g_pLocalPlayer->v_Origin.DistTo(ent->m_vecOrigin()) < distance && player_tools::shouldTarget(ent) && VisCheckEntFromEnt(LOCAL_E, ent))
        {
            if (hacks::shared::aimbot::ignore_cloak && IsPlayerInvisible(ent))
            {
                spy_cloak[i].update();
                continue;
            }
            if (!spy_cloak[i].check(*spy_ignore_time))
                continue;
            distance = g_pLocalPlayer->v_Origin.DistTo(ent->m_vecOrigin());
            best_ent = ent;
        }
    }
    return { best_ent, distance };
}

namespace stayNearHelpers
{
// Check if the location is close enough/far enough and has a visual to target
static bool isValidNearPosition(Vector vec, Vector target, const bot_class_config &config)
{
    vec.z += 40;
    target.z += 40;
    float dist = vec.DistTo(target);
    if (dist < config.min || dist > config.max)
        return false;
    if (!IsVectorVisible(vec, target, true))
        return false;
    return true;
}

// Returns true if began pathing
static bool stayNearPlayer(CachedEntity *&ent, const bot_class_config &config, CNavArea *&result)
{
    // Get some valid areas
    std::vector<CNavArea *> areas;
    for (auto &area : nav::navfile->m_areas)
    {
        if (!isValidNearPosition(area.m_center, ent->m_vecOrigin(), config))
            continue;
        areas.push_back(&area);
    }
    if (areas.empty())
        return false;

    const Vector ent_orig = ent->m_vecOrigin();
    // Area dist to target should be as close as possible to config.preferred
    std::sort(areas.begin(), areas.end(), [&](CNavArea *a, CNavArea *b) { return std::abs(a->m_center.DistTo(ent_orig) - config.preferred) < std::abs(b->m_center.DistTo(ent_orig) - config.preferred); });

    size_t size = 20;
    if (areas.size() < size)
        size = areas.size();

    // Get some areas that are close to the player
    std::vector<CNavArea *> preferred_areas(areas.begin(), areas.end());
    preferred_areas.resize(size / 2);
    if (preferred_areas.empty())
        return false;
    std::sort(preferred_areas.begin(), preferred_areas.end(), [](CNavArea *a, CNavArea *b) { return a->m_center.DistTo(g_pLocalPlayer->v_Origin) < b->m_center.DistTo(g_pLocalPlayer->v_Origin); });

    preferred_areas.resize(size / 4);
    if (preferred_areas.empty())
        return false;
    for (auto &i : preferred_areas)
    {
        if (nav::navTo(i->m_center, 7, true, false))
        {
            result       = i;
            current_task = task::stay_near;
            return true;
        }
    }

    for (size_t attempts = 0; attempts < size / 4; attempts++)
    {
        auto it = select_randomly(areas.begin(), areas.end());
        if (nav::navTo((*it.base())->m_center, 7, true, false))
        {
            result       = *it.base();
            current_task = task::stay_near;
            return true;
        }
    }
    return false;
}

// Loop thru all players and find one we can path to
static bool stayNearPlayers(const bot_class_config &config, CachedEntity *&result_ent, CNavArea *&result_area)
{
    std::vector<CachedEntity *> players;
    for (int i = 1; i < g_IEngine->GetMaxClients(); i++)
    {
        CachedEntity *ent = ENTITY(i);
        if (CE_BAD(ent) || !ent->m_bAlivePlayer() || !ent->m_bEnemy() || !player_tools::shouldTarget(ent))
            continue;
        if (hacks::shared::aimbot::ignore_cloak && IsPlayerInvisible(ent))
        {
            spy_cloak[i].update();
            continue;
        }
        if (!spy_cloak[i].check(*spy_ignore_time))
            continue;
        players.push_back(ent);
    }
    if (players.empty())
        return false;
    std::sort(players.begin(), players.end(), [](CachedEntity *a, CachedEntity *b) { return a->m_vecOrigin().DistTo(g_pLocalPlayer->v_Origin) < b->m_vecOrigin().DistTo(g_pLocalPlayer->v_Origin); });
    for (auto player : players)
    {
        if (stayNearPlayer(player, config, result_area))
        {
            result_ent = player;
            return true;
        }
    }
    return false;
}
} // namespace stayNearHelpers

// Main stay near function
static bool stayNear()
{
    static CachedEntity *last_target = nullptr;
    static CNavArea *last_area       = nullptr;

    // What distances do we have to use?
    const bot_class_config *config;
    if (spy_mode)
    {
        config = &DIST_SPY;
    }
    else if (heavy_mode)
    {
        config = &DIST_OTHER;
    }
    else
    {
        config = &DIST_SNIPER;
    }

    // Check if someone is too close to us and then target them instead
    std::pair<CachedEntity *, float> nearest = getNearestPlayerDistance();
    if (nearest.first && nearest.first != last_target && nearest.second < config->min)
        if (stayNearHelpers::stayNearPlayer(nearest.first, *config, last_area))
        {
            last_target = nearest.first;
            return true;
        }

    if (current_task == task::stay_near)
    {
        static Timer invalid_area_time{};
        static Timer invalid_target_time{};
        // Do we already have a stay near target? Check if its still good.
        if (CE_GOOD(last_target))
            invalid_target_time.update();
        else
            invalid_area_time.update();
        // Check if we still have LOS and are close enough/far enough
        if (CE_GOOD(last_target) && stayNearHelpers::isValidNearPosition(last_area->m_center, last_target->m_vecOrigin(), *config))
            invalid_area_time.update();

        if (CE_GOOD(last_target) && (!last_target->m_bAlivePlayer() || !last_target->m_bEnemy() || !player_tools::shouldTarget(last_target) || !spy_cloak[last_target->m_IDX].check(*spy_ignore_time) || (hacks::shared::aimbot::ignore_cloak && IsPlayerInvisible(last_target))))
        {
            if (hacks::shared::aimbot::ignore_cloak && IsPlayerInvisible(last_target))
                spy_cloak[last_target->m_IDX].update();
            nav::clearInstructions();
            current_task = task::none;
        }
        else if (invalid_area_time.test_and_set(300))
        {
            current_task = task::none;
        }
        else if (invalid_target_time.test_and_set(5000))
        {
            current_task = task::none;
        }
    }
    // Are we doing nothing? Check if our current location can still attack our
    // last target
    if (current_task == task::none && CE_GOOD(last_target) && last_target->m_bAlivePlayer() && last_target->m_bEnemy())
    {
        if (hacks::shared::aimbot::ignore_cloak && IsPlayerInvisible(last_target))
            spy_cloak[last_target->m_IDX].update();
        if (spy_cloak[last_target->m_IDX].check(*spy_ignore_time))
        {
            if (stayNearHelpers::isValidNearPosition(g_pLocalPlayer->v_Origin, last_target->m_vecOrigin(), *config))
                return true;
            // If not, can we try pathing to our last target again?
            if (stayNearHelpers::stayNearPlayer(last_target, *config, last_area))
                return true;
        }
        last_target = nullptr;
    }

    static Timer wait_until_stay_near{};
    if (current_task == task::stay_near)
    {
        return true;
    }
    else if (wait_until_stay_near.test_and_set(1000))
    {
        // We're doing nothing? Do something!
        return stayNearHelpers::stayNearPlayers(*config, last_target, last_area);
    }
    return false;
}

static inline bool hasLowAmmo()
{
    if (CE_BAD(LOCAL_W))
        return false;
    int *weapon_list = (int *) ((uint64_t)(RAW_ENT(LOCAL_E)) + netvar.hMyWeapons);
    if (!weapon_list)
        return false;
    if (g_pLocalPlayer->holding_sniper_rifle && CE_INT(LOCAL_E, netvar.m_iAmmo + 4) <= 5)
        return true;
    for (int i = 0; weapon_list[i]; i++)
    {
        int handle = weapon_list[i];
        int eid    = handle & 0xFFF;
        if (eid >= 32 && eid <= HIGHEST_ENTITY)
        {
            IClientEntity *weapon = g_IEntityList->GetClientEntity(eid);
            if (weapon and re::C_BaseCombatWeapon::IsBaseCombatWeapon(weapon) && re::C_TFWeaponBase::UsesPrimaryAmmo(weapon) && !re::C_TFWeaponBase::HasPrimaryAmmo(weapon))
                return true;
        }
    }
    return false;
}

static bool getHealthAndAmmo()
{
    static Timer health_ammo_timer{};
    if (!health_ammo_timer.test_and_set(2000))
        return false;
    if (current_task == task::health && static_cast<float>(LOCAL_E->m_iHealth()) / LOCAL_E->m_iMaxHealth() >= 0.64f)
    {
        nav::clearInstructions();
        current_task = task::none;
    }
    if (current_task == task::health)
        return true;

    if (static_cast<float>(LOCAL_E->m_iHealth()) / LOCAL_E->m_iMaxHealth() < 0.64f)
    {
        std::vector<Vector> healthpacks;
        for (int i = 1; i < HIGHEST_ENTITY; i++)
        {
            CachedEntity *ent = ENTITY(i);
            if (CE_BAD(ent))
                continue;
            if (ent->m_ItemType() != ITEM_HEALTH_SMALL && ent->m_ItemType() != ITEM_HEALTH_MEDIUM && ent->m_ItemType() != ITEM_HEALTH_LARGE)
                continue;
            healthpacks.push_back(ent->m_vecOrigin());
        }
        std::sort(healthpacks.begin(), healthpacks.end(), [](Vector &a, Vector &b) { return g_pLocalPlayer->v_Origin.DistTo(a) < g_pLocalPlayer->v_Origin.DistTo(b); });
        for (auto &pack : healthpacks)
        {
            if (nav::navTo(pack, 10, true, false))
            {
                current_task = task::health;
                return true;
            }
        }
    }

    if (current_task == task::ammo && !hasLowAmmo())
    {
        nav::clearInstructions();
        current_task = task::none;
        return false;
    }
    if (current_task == task::ammo)
        return true;
    if (hasLowAmmo())
    {
        std::vector<Vector> ammopacks;
        for (int i = 1; i < HIGHEST_ENTITY; i++)
        {
            CachedEntity *ent = ENTITY(i);
            if (CE_BAD(ent))
                continue;
            if (ent->m_ItemType() != ITEM_AMMO_SMALL && ent->m_ItemType() != ITEM_AMMO_MEDIUM && ent->m_ItemType() != ITEM_AMMO_LARGE)
                continue;
            ammopacks.push_back(ent->m_vecOrigin());
        }
        std::sort(ammopacks.begin(), ammopacks.end(), [](Vector &a, Vector &b) { return g_pLocalPlayer->v_Origin.DistTo(a) < g_pLocalPlayer->v_Origin.DistTo(b); });
        for (auto &pack : ammopacks)
        {
            if (nav::navTo(pack, 9, true, false))
            {
                current_task = task::ammo;
                return true;
            }
        }
    }
    return false;
}

static void autoJump()
{
    static Timer last_jump{};
    if (!last_jump.test_and_set(200))
        return;

    if (getNearestPlayerDistance().second <= *jump_distance)
        current_user_cmd->buttons |= IN_JUMP | IN_DUCK;
}

enum slots
{
    primary   = 1,
    secondary = 2,
    melee     = 3
};
static int GetBestSlot()
{

    switch (g_pLocalPlayer->clazz)
    {
    case tf_scout:
        return primary;
    case tf_heavy:
        return primary;
    case tf_medic:
        return secondary;
    case tf_spy:
        return primary;
    default:
    {
        float nearest_dist = getNearestPlayerDistance().second;
        if (nearest_dist > 400)
            return primary;
        else
            return secondary;
    }
    }
    return primary;
}

static void updateSlot()
{
    static Timer slot_timer{};
    if (!slot_timer.test_and_set(300))
        return;
    if (CE_GOOD(LOCAL_E) && CE_GOOD(LOCAL_W) && !g_pLocalPlayer->life_state)
    {
        IClientEntity *weapon = RAW_ENT(LOCAL_W);
        // IsBaseCombatWeapon()
        if (re::C_BaseCombatWeapon::IsBaseCombatWeapon(weapon))
        {
            int slot    = re::C_BaseCombatWeapon::GetSlot(weapon);
            int newslot = GetBestSlot();
            if (slot != newslot - 1)
                g_IEngine->ClientCmd_Unrestricted(format("slot", newslot).c_str());
        }
    }
}

static InitRoutine runinit([]() { EC::Register(EC::CreateMove, CreateMove, "navbot", EC::early); });

void change(settings::VariableBase<bool> &, bool)
{
    nav::clearInstructions();
}

static InitRoutine routine([]() { enabled.installChangeCallback(change); });

struct Posinfo
{
    float x;
    float y;
    float pitch;
    float yaw;
    bool usepitch;
    bool active;
};
struct MapPosinfo
{
    Posinfo spot;
    std::string lvlname;
};

static Posinfo to_path{};
static std::vector<MapPosinfo> oob_list;
void OutOfBoundsrun(const CCommand &args)
{
    if (CE_BAD(LOCAL_E) || !LOCAL_E->m_bAlivePlayer())
        return;
    // Need atleast 3 arguments (x, y, yaw)
    if (args.ArgC() < 2)
    {
        std::string lvlname = g_IEngine->GetLevelName();
        std::vector<Posinfo> potential_spots{};
        for (auto &i : oob_list)
        {
            if (lvlname.find(i.lvlname) != lvlname.npos)
                potential_spots.push_back(i.spot);
        }
        Posinfo best_spot{};
        float best_score = FLT_MAX;
        for (auto &i : potential_spots)
        {
            Vector pos  = { i.x, i.y, 0.0f };
            float score = pos.AsVector2D().DistTo(LOCAL_E->m_vecOrigin().AsVector2D());
            if (score < best_score)
            {
                if (IsVectorVisible(g_pLocalPlayer->v_Eye, { pos.x, pos.y, g_pLocalPlayer->v_Eye.z }, true))
                {
                    best_spot  = i;
                    best_score = score;
                }
            }
        }
        to_path = best_spot;
        if (!to_path.active)
            logging::Info("No valid spots found nearby!");
        return;
    }
    if (args.ArgC() < 4)
    {
        logging::Info("Usage:");
        logging::Info("cat_outofbounds x y Yaw (example: cat_outofbounds 511.943848 2783.968750 7.6229) or");
        logging::Info("cat_outofbounds x y Pitch Yaw (example: cat_outofbounds 511.943848 2783.968750 7.6229 89.936729)");
        return;
    }
    bool usepitch = false;
    // Use pitch too
    if (args.ArgC() > 4)
        usepitch = true;
    float x, y, yaw, pitch;
    // Failsafe
    try
    {
        x   = std::stof(args.Arg(1));
        y   = std::stof(args.Arg(2));
        yaw = std::stof(args.Arg(3));
        if (usepitch)
        {
            pitch = std::stof(args.Arg(3));
            yaw   = std::stof(args.Arg(4));
        }
    }
    catch (std::invalid_argument)
    {
        logging::Info("Invalid argument! (Malformed input?)\n");
        logging::Info("Usage:");
        logging::Info("cat_outofbounds x y Yaw (example: cat_outofbounds 511.943848 2783.968750 7.6229) or");
        logging::Info("cat_outofbounds x y Yaw Pitch (example: cat_outofbounds 511.943848 2783.968750 7.6229 89.936729)");
        return;
    }
    // Assign all the values
    to_path.x        = x;
    to_path.y        = y;
    to_path.yaw      = yaw;
    to_path.pitch    = pitch;
    to_path.usepitch = usepitch;
    to_path.active   = true;
}

int getCarriedBuilding()
{
    if (CE_BYTE(LOCAL_E, netvar.m_bCarryingObject))
        return HandleToIDX(CE_INT(LOCAL_E, netvar.m_hCarriedObject));
    for (int i = 1; i < MAX_ENTITIES; i++)
    {
        auto ent = ENTITY(i);
        if (CE_BAD(ent) || ent->m_Type() != ENTITY_BUILDING)
            continue;
        if (HandleToIDX(CE_INT(ent, netvar.m_hBuilder)) != LOCAL_E->m_IDX)
            continue;
        if (!CE_BYTE(ent, netvar.m_bPlacing))
            continue;
        return i;
    }
    return -1;
}

static CatCommand Outofbounds{ "outofbounds", "Out of bounds", OutOfBoundsrun };

static Timer timeout{};
static float yaw_offset = 0.0f;
void oobcm()
{
    if (to_path.active)
    {
        if (CE_GOOD(LOCAL_E) && LOCAL_E->m_bAlivePlayer())
        {
            Vector topath = { to_path.x, to_path.y, LOCAL_E->m_vecOrigin().z };
            if (LOCAL_E->m_vecOrigin().AsVector2D().DistTo(topath.AsVector2D()) <= 0.01f || timeout.test_and_set(10000))
            {
                if (LOCAL_E->m_vecOrigin().AsVector2D().DistTo(topath.AsVector2D()) <= 0.01f)
                {
                    if (re::C_BaseCombatWeapon::GetSlot(RAW_ENT(LOCAL_W)) != 5)
                    {
                        yaw_offset     = 0.0f;
                        to_path.active = false;
                        if (to_path.usepitch)
                            current_user_cmd->viewangles.x = to_path.pitch;
                        current_user_cmd->viewangles.y = to_path.yaw;
                        logging::Info("Arrived at the destination! offset: %f %f", fabsf(LOCAL_E->m_vecOrigin().x - topath.x), fabsf(LOCAL_E->m_vecOrigin().y - topath.y));
                    }
                    else
                    {
                        timeout.update();
                        if (to_path.usepitch)
                            current_user_cmd->viewangles.x = to_path.pitch;
                        current_user_cmd->viewangles.y = to_path.yaw;
                        int carried_build              = getCarriedBuilding();
                        if (carried_build == -1)
                        {
                            logging::Info("No building held");
                            return;
                        }
                        auto ent = ENTITY(carried_build);
                        if (CE_BAD(ent))
                        {
                            logging::Info("No Building held");
                            to_path.active = false;
                            return;
                        }
                        else
                        {
                            if (CE_BYTE(ent, netvar.m_bCanPlace))
                                current_user_cmd->buttons |= IN_ATTACK;
                            if (yaw_offset >= 0.1f)
                            {
                                logging::Info("Failed getting out of bounds, Yaw offset too large");
                                to_path.active = false;
                                return;
                            }
                            yaw_offset = -yaw_offset;
                            if (yaw_offset >= 0.0f)
                                yaw_offset += 0.0001f;
                            current_user_cmd->viewangles.y = to_path.yaw + yaw_offset;
                        }
                    }
                }
                else
                {
                    yaw_offset     = 0.0f;
                    to_path.active = false;
                    if (to_path.usepitch)
                        current_user_cmd->viewangles.x = to_path.pitch;
                    current_user_cmd->viewangles.y = to_path.yaw;
                    logging::Info("Timed out trying to get to spot");
                }
            }
            if (yaw_offset == 0.0f)
            {
                auto move                     = ComputeMovePrecise(LOCAL_E->m_vecOrigin(), topath);
                current_user_cmd->forwardmove = move.first;
                current_user_cmd->sidemove    = move.second;
            }
        }
    }
    else
        timeout.update();
}
#define OOB_ADD(x, y, yaw, pitch, name) (oob_list.push_back({ { x, y, yaw, pitch, true, true }, name }))
static InitRoutine oob([]() {
    // Badwater
    OOB_ADD(511.943848f, 2783.968750f, 7.622991f, 89.936729f, "pl_badwater");

    // Borneo
    OOB_ADD(-467.939911f, -6056.031250f, 9.259290f, 90.082581f, "pl_borneo");

    // Doublecross
    OOB_ADD(-1016.030029f, -2580.031982f, 9.347898f, 0.041826f, "ctf_doublecross");
    OOB_ADD(1016.001953f, 2580.053223f, 7.275527f, -179.931656f, "ctf_doublecross");

    // Egypt
    // Stage 1
    OOB_ADD(-1754.280f, -3344.04f, 39.20f, 0.04f, "cp_egypt");
    // Stage 2
    OOB_ADD(2919.968750f, 1999.951416f, 11.952104f, 0.053882f, "cp_egypt");
    OOB_ADD(87.946884f, 1885.851685f, 34.806473f, 89.951176f, "cp_egypt");
    // Stage 3
    OOB_ADD(1263.968750f, 4495.946289f, 7.465197f, 0.074329f, "cp_egypt");

    // Turbine
    // Red
    OOB_ADD(1992.028442f, 936.019775f, 0.272817f, -179.983673f, "ctf_turbine");
    OOB_ADD(1696.029175f, 1008.293091f, 35.000000f, -90.038498f, "ctf_turbine");
    OOB_ADD(1927.989624f, 936.019775f, 0.432120f, -0.026141f, "ctf_turbine");
    // Blue
    OOB_ADD(-1992.051514f, -936.055908f, -0.768594f, 0.064962f, "ctf_turbine");
    OOB_ADD(-1696.021606f, -1008.698181f, 35.000000f, 89.979446f, "ctf_turbine");
    OOB_ADD(-1927.023193f, -936.055847f, 2.673917f, 179.936523f, "ctf_turbine");

    // Swiftwater
    OOB_ADD(5543.948730f, -1527.988037f, 23.115799f, -0.012952f, "pl_swiftwater_final1");
    OOB_ADD(2636.031250f, -1126.089478f, 13.124457f, 179.843811f, "pl_swiftwater_final1");

    EC::Register(EC::CreateMove, oobcm, "OOB_CM");
});
} // namespace hacks::tf2::NavBot
