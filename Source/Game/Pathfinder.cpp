#include "Pathfinder.hpp"
#include "Items.hpp"
#include "World.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <queue>

namespace Pathfinder {
    namespace {
        constexpr int STEP = 10;          /* one plain tile */
        constexpr int DOOR_COST = 15;     /* doors/portals work, but walking past them is preferred */
        constexpr int PAIN_COST = 200;    /* fire, stings, burning tiles: only if there is no other way */

        bool InBounds(const WorldState& world, int x, int y) noexcept {
            return x >= 0 && y >= 0 && x < static_cast<int>(world.width) && y < static_cast<int>(world.height);
        }

        bool Listed(const std::vector<uint32_t>& list, uint32_t userID) {
            return std::find(list.begin(), list.end(), userID) != list.end();
        }

        /* Entrances and the like: open to the public, or nobody locked the
           spot (then anyone may pass), or the lock lets us in. */
        bool HasAccess(const WorldState& world, int x, int y, const WorldTile& tile, uint32_t userID) {
            if (tile.Has(TileFlags::IS_OPEN_TO_PUBLIC)) return true;
            if (!world.WorldLock() && !tile.Has(TileFlags::HAS_PARENT)) return true;
            return world.Permission(static_cast<uint32_t>(x), static_cast<uint32_t>(y), userID) != TilePermission::None;
        }

        int Hazard(const ItemDB::Item& item, const WorldTile& tile) {
            using namespace ItemDB;
            switch (item.type) {
            case Type::DEADLY: case Type::TOGGLEABLE_DEADLY: case Type::ACID: return -1;
            case Type::FIRE_PAIN: case Type::STING_PAIN: return PAIN_COST;
            case Type::DOOR: case Type::PORTAL: case Type::MAIN_DOOR: return DOOR_COST;
            default: return tile.Has(TileFlags::ON_FIRE) ? PAIN_COST : STEP;
            }
        }
    }

    int EnterCost(const WorldState& world, int x, int y, int dx, int dy, uint32_t userID) {
        using namespace ItemDB;
        if (!InBounds(world, x, y)) return -1;
        const WorldTile* tile = world.Tile(x, y);
        if (!tile) return -1;
        if (tile->fg == 0) return tile->Has(TileFlags::ON_FIRE) ? PAIN_COST : STEP;

        /* Unknown item (or no items.dat): solid, never guess a way through. */
        const Item* item = ItemDB::Find(tile->fg);
        if (!item) return -1;
        const int cost = Hazard(*item, *tile);
        if (cost < 0) return -1;

        /* Entrances whose access list lives in the tile itself. */
        if (const auto* vip = tile->As<TileExtra::VipEntrance>()) {
            return vip->ownerUID == userID || Listed(vip->accessUIDs, userID) ? cost : -1;
        }
        if (const auto* friends = tile->As<TileExtra::FriendsEntrance>()) {
            return friends->ownerUID == userID || Listed(friends->allowedUIDs, userID) ? cost : -1;
        }
        if (const auto* cloud = tile->As<TileExtra::StormyCloud>()) {
            return cloud->isSolid == 0 ? cost : -1;
        }

        const bool on = tile->Has(TileFlags::IS_ON);
        switch (item->collision) {
        case Collision::NO_COLLISION:
        case Collision::STEP_ON:
            return cost;
        case Collision::ON_TOP:     /* platform: solid only from above */
            return dy == 1 ? -1 : cost;
        case Collision::IF_ACCESS:  /* entrance */
        case Collision::BALLOON_WARZ_TEAM:
        case Collision::IF_GUILD:
            return HasAccess(world, x, y, *tile, userID) ? cost : -1;
        case Collision::TOGGLE:     /* e.g. a switchable door: passable while switched on */
            return on ? cost : -1;
        case Collision::HORIZONTAL: /* one-way sideways; flipped tiles point the other way */
            if (tile->Has(TileFlags::FLIPPED_X)) return dx == 1 ? -1 : cost;
            return dx == -1 ? -1 : cost;
        case Collision::VERTICAL:   /* one-way, no going up through it */
            return dy == -1 ? -1 : cost;
        case Collision::ACTIVATE:   /* solid while activated */
            return on ? -1 : cost;
        case Collision::IF_VIP:     /* VIP entrance without its extra data: assume closed */
        case Collision::FULL:
        case Collision::ADVENTURE_ITEM:
        default:
            return -1;
        }
    }

    Result Find(const WorldState& world, Point from, Point to, uint32_t userID, std::vector<Point>& path) {
        path.clear();
        if (!world.loaded || world.width == 0 || world.height == 0) return Result::NoWorld;
        if (!InBounds(world, from.x, from.y) || !InBounds(world, to.x, to.y)) return Result::OutOfBounds;
        if (from == to) {
            path.push_back(from);
            return Result::Found;
        }
        if (EnterCost(world, to.x, to.y, 0, 0, userID) < 0) return Result::TargetBlocked;

        const int width = static_cast<int>(world.width);
        const size_t count = static_cast<size_t>(world.width) * world.height;
        std::vector<int> cost(count, INT_MAX);
        std::vector<int> parent(count, -1);
        std::vector<uint8_t> closed(count, 0);

        struct Entry { int f, h, index; };
        /* Lowest f first; on a tie the one closer to the goal. */
        const auto later = [](const Entry& a, const Entry& b) { return a.f != b.f ? a.f > b.f : a.h > b.h; };
        std::priority_queue<Entry, std::vector<Entry>, decltype(later)> open(later);

        const auto heuristic = [&](int x, int y) { return (std::abs(x - to.x) + std::abs(y - to.y)) * STEP; };
        const int start = from.y * width + from.x;
        const int goal = to.y * width + to.x;

        cost[static_cast<size_t>(start)] = 0;
        open.push({ heuristic(from.x, from.y), heuristic(from.x, from.y), start });

        constexpr std::array<Point, 4> DIRECTIONS{ { { 1, 0 }, { -1, 0 }, { 0, -1 }, { 0, 1 } } };
        while (!open.empty()) {
            const Entry current = open.top();
            open.pop();
            if (current.index == goal) break;
            if (closed[static_cast<size_t>(current.index)]) continue;
            closed[static_cast<size_t>(current.index)] = 1;

            const int cx = current.index % width, cy = current.index / width;
            for (const Point d : DIRECTIONS) {
                const int nx = cx + d.x, ny = cy + d.y;
                if (!InBounds(world, nx, ny)) continue;
                const int next = ny * width + nx;
                if (closed[static_cast<size_t>(next)]) continue;

                const int step = EnterCost(world, nx, ny, d.x, d.y, userID);
                if (step < 0) continue;
                const int g = cost[static_cast<size_t>(current.index)] + step;
                if (g >= cost[static_cast<size_t>(next)]) continue;

                cost[static_cast<size_t>(next)] = g;
                parent[static_cast<size_t>(next)] = current.index;
                const int h = heuristic(nx, ny);
                open.push({ g + h, h, next });
            }
        }

        if (parent[static_cast<size_t>(goal)] == -1) return Result::NoPath;
        for (int i = goal; i != -1; i = parent[static_cast<size_t>(i)]) path.push_back({ i % width, i / width });
        std::reverse(path.begin(), path.end());
        return Result::Found;
    }

    Point TileOf(float x, float y) noexcept {
        /* The avatar's centre, not its top-left corner. */
        return { static_cast<int>(std::floor((x + 10.0f) / 32.0f)), static_cast<int>(std::floor((y + 15.0f) / 32.0f)) };
    }

    float PixelX(int tileX) noexcept { return static_cast<float>(tileX) * 32.0f; }
    float PixelY(int tileY) noexcept { return static_cast<float>(tileY) * 32.0f + 2.0f; }

    bool InReach(Point here, int x, int y, bool placesBlock) noexcept {
        const int dx = std::abs(x - here.x), dy = std::abs(y - here.y);
        return dx <= REACH && dy <= REACH && !(placesBlock && dx == 0 && dy == 0);
    }

    const char* Describe(Result result) noexcept {
        switch (result) {
        case Result::Found: return "path found";
        case Result::NoWorld: return "the world isn't loaded";
        case Result::OutOfBounds: return "that tile is outside the world";
        case Result::TargetBlocked: return "the target tile is solid or locked";
        case Result::NoPath: return "there is no open way there";
        }
        return "unknown";
    }
}
