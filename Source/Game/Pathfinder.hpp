#pragma once

#include <cstdint>
#include <vector>

class WorldState;

/* @info: A* over a parsed world (tile units), ported from AquaProxy.
   Passability is decided per step from the tile's collision type and its
   current state, so closed doors, entrances we have no access to, VIP/friends
   entrances, one-way blocks and platforms are honoured. Without items.dat
   every foreground block counts as solid (only air is walkable). */
namespace Pathfinder {
    struct Point {
        int x = 0, y = 0;
        bool operator==(const Point&) const = default;
    };

    enum class Result : uint8_t { Found, NoWorld, OutOfBounds, TargetBlocked, NoPath };

    /* @important: the game lets the player punch / place / wrench only the 5x5
       square around itself (2 tiles on either axis). The server flags anything
       further as a cheat and bans for it, so the bot never sends it. */
    inline constexpr int REACH = 2;

    /* Cost of stepping onto (x, y) while moving by (dx, dy); -1 = blocked.
       (dx, dy) = (0, 0) skips the direction rules (one-way blocks, platforms). */
    int EnterCost(const WorldState& world, int x, int y, int dx, int dy, uint32_t userID);

    /* Route from `from` to `to`, both ends included. The start tile is never
       checked, so a path can begin inside an entrance we are standing in. */
    Result Find(const WorldState& world, Point from, Point to, uint32_t userID, std::vector<Point>& path);

    /* Avatar position in pixels <-> tile. */
    Point TileOf(float x, float y) noexcept;
    /* Where a walk step puts the avatar (what Mori and AquaProxy send). */
    float PixelX(int tileX) noexcept;
    float PixelY(int tileY) noexcept;

    /* Within the 5x5 reach around `here`. A block can't be placed on our own
       tile (a background can). */
    bool InReach(Point here, int x, int y, bool placesBlock) noexcept;

    const char* Describe(Result result) noexcept;
}
