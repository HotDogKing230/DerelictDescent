#include <stdio.h>
#include <algorithm>
#include "pm_app_api.h"

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
    ============================================================
    DERELICT DESCENT - A TURN-BASED SURVIVAL ROGUELIKE FOR POCKETMAGE
    ============================================================

    8x8 rooms
    SD-card room persistence
    Four room exits
    Full simulation state stored per tile
    PocketMage app

    SD layout:

        /oni/
            world.dat
            rooms/
                x_y.dat

    Example:

        /oni/rooms/0_0.dat
        /oni/rooms/1_0.dat
        /oni/rooms/0_1.dat

    Room coordinates are signed.

    Current room is kept in RAM.
    Other rooms live on SD.

    ============================================================
*/


// ============================================================
// CONFIGURATION
// ============================================================

static constexpr uint8_t ROOM_W = 8;
static constexpr uint8_t ROOM_H = 8;

static constexpr uint16_t ROOM_VERSION = 2;

static constexpr char WORLD_FILE[] =
"/oni/world.dat";

static constexpr char ROOM_DIRECTORY[] =
"/oni/rooms/";


// ============================================================
// RUNTIME
// ============================================================

static constexpr uint32_t POLL_INTERVAL_MS = 10;

static constexpr int K_EXIT_KEY = 12;      // OS exit key ("_EXIT_")

static constexpr int K_ESC_KEY = 27;       // ESC from a USB keyboard


// ============================================================
// SIMULATION BALANCE
// ============================================================

static constexpr int HEAT_CONDUCT_DIV = 100;       // orthogonal conduction

static constexpr int HEAT_RELAX_DIV = 32;          // pull toward ambient

static constexpr int ELECTRICAL_HEAT_DIV = 40;     // mW per 0.01 C per tick

static constexpr int GAS_DIFFUSE_DIV = 8;          // orthogonal gas diffusion

static constexpr int MIN_TEMPERATURE_C100 = -5000; // -50.00 C

static constexpr int MAX_TEMPERATURE_C100 = 40000; // 400.00 C


// ============================================================
// MATERIALS
// ============================================================

enum Material : uint8_t
{
    MAT_EMPTY = 0,

    MAT_STONE,
    MAT_DIRT,
    MAT_WOOD,
    MAT_IRON,
    MAT_COPPER,

    MAT_WATER,
    MAT_ICE,

    MAT_OXYGEN,
    MAT_NITROGEN,
    MAT_CO2,
    MAT_HYDROGEN,
    MAT_HELIUM,
    MAT_METHANE,
    MAT_STEAM,
    MAT_CANISTER,
    MAT_REACTOR,
    MAT_GARDEN,
    MAT_RECYCLER,
    MAT_SOLAR,
    MAT_BATTERY,
    MAT_FABRICATOR,
    MAT_WALL
};


// ============================================================
// RUN BALANCE
// ============================================================

static constexpr int16_t REACTOR_ROOM_X = 4;

static constexpr int16_t REACTOR_ROOM_Y = 0;

static constexpr int TANK_MAX = 100;

static constexpr int TANK_START = 100;

static constexpr int TANK_DRAIN_IDLE = 1;      // percent per turn

static constexpr int TANK_DRAIN_HAZARD = 4;    // percent per turn in a hazard

static constexpr int TANK_CANISTER = 50;       // percent restored per canister

static constexpr int TURN_PHYSICS_TICKS = 3;   // physics steps folded into a turn

static constexpr int32_t HOT_TEMPERATURE_C100 = 6000;   // 60.00 C

static constexpr int32_t COLD_TEMPERATURE_C100 = 0;     // 0.00 C

static constexpr uint16_t LOW_OXYGEN = 120;             // 12.0%

static constexpr int16_t NORMAL_AMBIENT_C100 = 2000;

static constexpr int16_t HOT_AMBIENT_C100 = 7000;

static constexpr int16_t COLD_AMBIENT_C100 = -1000;

static constexpr uint8_t OUTCOME_RUNNING = 0;

static constexpr uint8_t OUTCOME_WON = 1;

static constexpr uint8_t OUTCOME_LOST = 2;

static constexpr uint32_t OUTCOME_HOLD_MS = 4000;


// ============================================================
// ROOM KIND
// ============================================================

enum RoomKind : uint8_t
{
    ROOM_SALVAGE = 0,
    ROOM_FLOODED,
    ROOM_HOT,
    ROOM_COLD,
    ROOM_TOXIC,
    ROOM_ELECTRIFIED,
    ROOM_REACTOR
};


// ============================================================
// GAS
// ============================================================

struct Gas
{
    uint16_t oxygen;
    uint16_t nitrogen;
    uint16_t co2;

    uint16_t hydrogen;
    uint16_t helium;
    uint16_t methane;
    uint16_t steam;
};


// ============================================================
// ELECTRICITY
// ============================================================

struct Electrical
{
    uint8_t enabled;

    uint16_t voltage_mV;
    uint16_t resistance_mOhm;

    uint16_t current_mA;
    uint16_t power_mW;
};


// ============================================================
// TILE
// ============================================================

struct Tile
{
    uint8_t material;

    /*
        Mass stored in grams.

        This avoids floats in the save file.
    */
    uint32_t mass_g;

    /*
        Temperature in 1/100 C.

        2000 = 20.00 C
    */
    int32_t temperature_c100;

    Gas gas;

    Electrical electricity;

    uint8_t flags;
};


// ============================================================
// ROOM HEADER
// ============================================================

struct RoomHeader
{
    uint32_t magic;

    uint16_t version;

    int16_t x;
    int16_t y;

    /*
        Temperature the hull leaks toward, in 1/100 C. Set per room kind, so
        hot rooms stay hot and cold rooms stay cold. 0 is treated as "unset"
        and reads back as the normal ambient.
    */

    int16_t ambient_c100;

    uint32_t simulationTick;
    uint32_t runSeed;

    uint32_t checksum;
};


// ============================================================
// COMPLETE ROOM
// ============================================================

struct Room
{
    RoomHeader header;

    Tile tiles[ROOM_H][ROOM_W];
};


// ============================================================
// WORLD STATE
// ============================================================

struct WorldState
{
    int16_t roomX;
    int16_t roomY;

    uint8_t playerX;
    uint8_t playerY;

    uint32_t globalTick;

    /*
        Personal O2 reserve, 0..TANK_MAX. It bleeds down every tick and faster
        inside a hazard, and canisters top it up. At zero the run is lost.
    */

    uint16_t tank;

    /*
        OUTCOME_RUNNING while the run is live, OUTCOME_WON on the reactor,
        OUTCOME_LOST when the tank empties.
    */

    uint8_t outcome;

    // Run-specific seed makes fresh expeditions generate different rooms.
    uint32_t runSeed;
    uint16_t stone;
    uint16_t scrap;
    uint16_t water;
    uint16_t food;
    uint16_t power;
    uint8_t buildSelection;
};


// ============================================================
// GLOBAL GAME STATE
// ============================================================

static Room currentRoom;

static WorldState worldState;

static bool redrawRequested = true;

/*
    The screens are only pushed when the visible frame actually changes. The
    signature is a hash of everything that is drawn, so a tick that only nudges
    hidden simulation state costs no refresh.
*/

static bool frameDrawn = false;

static uint32_t lastFrameSignature = 0;

/*
    The briefing is shown on launch and again from the help key.
*/

static bool showIntro = true;


// ============================================================
// DISPLAY CONSTANTS
// ============================================================

/*
    The OS drives the panel at rotation 3, so the drawing space is 320 x 240
    (landscape) even though the panel is 240 x 320 natively. These match the
    OS layout module: kEinkWidth 320, kEinkHeight 240, kEinkStatusH 26.

    Tiles are 26 px so the 8 wide room fits the 214 px content region
    (8 * 26 = 208). The column to the right of the map holds the readouts and
    the 26 px band along the bottom holds the control legend.
*/

static constexpr int PANEL_WIDTH = 320;

static constexpr int PANEL_HEIGHT = 240;

static constexpr int STATUS_BAND_HEIGHT = 26;

static constexpr int CONTENT_HEIGHT =
PANEL_HEIGHT - STATUS_BAND_HEIGHT;

static constexpr int TILE_PIXELS = 26;

static constexpr int MAP_X = 0;
static constexpr int MAP_Y = 0;

static constexpr int MAP_WIDTH =
ROOM_W * TILE_PIXELS;

static constexpr int MAP_HEIGHT =
ROOM_H * TILE_PIXELS;

static_assert(
    MAP_HEIGHT <= CONTENT_HEIGHT,
    "map overflows the content region above the legend band"
);

/*
    Readout column, to the right of the map.
*/

static constexpr int INFO_X = MAP_WIDTH + 12;

static constexpr int INFO_Y = 16;

static constexpr int INFO_LINE = 16;

/*
    Control legend band, along the bottom.
*/

static constexpr int STATUS_Y =
PANEL_HEIGHT - STATUS_BAND_HEIGHT;


// ============================================================
// ROOM MAGIC
// ============================================================

static constexpr uint32_t ROOM_MAGIC =
0x44445231;   // "DDR1"


// ============================================================
// UTILITY
// ============================================================

static void requestRedraw()
{
    redrawRequested = true;
}


static uint32_t nowMillis()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return static_cast<uint32_t>(
        ts.tv_sec * 1000u + ts.tv_nsec / 1000000u
    );
}


// ============================================================
// DEFAULT TILE
// ============================================================

static Tile makeEmptyTile()
{
    Tile t{};

    t.material = MAT_EMPTY;

    t.mass_g = 0;

    t.temperature_c100 = NORMAL_AMBIENT_C100;

    /*
        Atmosphere:

        O2 = 21%
        N2 = 79%

        Stored as thousandths.
    */

    t.gas.oxygen = 210;
    t.gas.nitrogen = 790;

    return t;
}


// ============================================================
// DEFAULT ROOM
// ============================================================

/*
    The room kind is a pure function of the coordinates, so a room always
    looks the same across sessions and matches whatever was saved for it.
*/

static RoomKind roomKind(int16_t x, int16_t y)
{
    if (x == REACTOR_ROOM_X && y == REACTOR_ROOM_Y)
        return ROOM_REACTOR;

    if (x == 0 && y == 0)
        return ROOM_SALVAGE;

    const uint32_t h =
        (static_cast<uint32_t>(x + 101) * 73856093u) ^
        (static_cast<uint32_t>(y + 53) * 19349663u) ^
        worldState.runSeed;

    switch (h % 5u)
    {
    case 0: return ROOM_FLOODED;
    case 1: return ROOM_HOT;
    case 2: return ROOM_TOXIC;
    case 3: return ROOM_ELECTRIFIED;
    default: return ROOM_COLD;
    }
}


static int16_t roomAmbient(RoomKind kind)
{
    switch (kind)
    {
    case ROOM_HOT:  return HOT_AMBIENT_C100;
    case ROOM_COLD: return COLD_AMBIENT_C100;
    default:        return NORMAL_AMBIENT_C100;
    }
}


static void generateRoom(
    Room& room,
    int16_t x,
    int16_t y)
{
    memset(&room, 0, sizeof(Room));

    room.header.magic = ROOM_MAGIC;
    room.header.version = ROOM_VERSION;

    room.header.x = x;
    room.header.y = y;

    const RoomKind kind =
        roomKind(x, y);

    room.header.ambient_c100 =
        roomAmbient(kind);

    room.header.simulationTick = 0;
    room.header.runSeed = worldState.runSeed;

    for (uint8_t ty = 0; ty < ROOM_H; ty++)
    {
        for (uint8_t tx = 0; tx < ROOM_W; tx++)
        {
            room.tiles[ty][tx] =
                makeEmptyTile();

            room.tiles[ty][tx].temperature_c100 =
                room.header.ambient_c100;
        }
    }


    /*
        ROOM WALLS

        # # # # X # # #
        #             #
        #             #
        X             X
        #             #
        #             #
        #             #
        # # # # X # # #

        X = exit
    */

    for (uint8_t xPos = 0;
        xPos < ROOM_W;
        xPos++)
    {
        room.tiles[0][xPos].material =
            MAT_STONE;

        room.tiles[0][xPos].mass_g =
            10000;

        room.tiles[ROOM_H - 1][xPos].material =
            MAT_STONE;

        room.tiles[ROOM_H - 1][xPos].mass_g =
            10000;
    }


    for (uint8_t yPos = 0;
        yPos < ROOM_H;
        yPos++)
    {
        room.tiles[yPos][0].material =
            MAT_STONE;

        room.tiles[yPos][0].mass_g =
            10000;

        room.tiles[yPos][ROOM_W - 1].material =
            MAT_STONE;

        room.tiles[yPos][ROOM_W - 1].mass_g =
            10000;
    }


    /*
        Open the four doors.
    */

    const uint8_t cx = ROOM_W / 2;
    const uint8_t cy = ROOM_H / 2;

    room.tiles[0][cx] =
        makeEmptyTile();

    room.tiles[ROOM_H - 1][cx] =
        makeEmptyTile();

    room.tiles[cy][0] =
        makeEmptyTile();

    room.tiles[cy][ROOM_W - 1] =
        makeEmptyTile();


    /*
        Decorate the room by kind. Every non-reactor room carries a canister;
        hazards are arranged so the door-to-door crossing stays possible.
    */

    switch (kind)
    {
    case ROOM_SALVAGE:
        room.tiles[2][2].material = MAT_CANISTER;
        room.tiles[2][2].mass_g = 1000;

        room.tiles[5][5].material = MAT_CANISTER;
        room.tiles[5][5].mass_g = 1000;
        break;

    case ROOM_FLOODED:
        for (uint8_t ty = 4; ty < ROOM_H - 1; ty++)
        {
            for (uint8_t tx = 1; tx < ROOM_W - 1; tx++)
            {
                room.tiles[ty][tx].material = MAT_WATER;
                room.tiles[ty][tx].mass_g = 1000;
            }
        }

        room.tiles[1][2].material = MAT_CANISTER;
        room.tiles[1][2].mass_g = 1000;
        break;

    case ROOM_HOT:
        room.tiles[3][3].material = MAT_STONE;
        room.tiles[3][3].mass_g = 25000;

        room.tiles[4][5].material = MAT_CANISTER;
        room.tiles[4][5].mass_g = 1000;
        break;

    case ROOM_COLD:
        room.tiles[4][2].material = MAT_ICE;
        room.tiles[4][2].mass_g = 1000;

        room.tiles[3][3].material = MAT_CANISTER;
        room.tiles[3][3].mass_g = 1000;
        break;

    case ROOM_TOXIC:
        for (uint8_t ty = 1; ty < ROOM_H - 1; ty++)
        {
            for (uint8_t tx = 1; tx < ROOM_W - 1; tx++)
            {
                room.tiles[ty][tx].gas.oxygen = 40;
                room.tiles[ty][tx].gas.co2 = 900;
            }
        }

        room.tiles[3][3].material = MAT_CANISTER;
        room.tiles[3][3].mass_g = 1000;
        break;

    case ROOM_ELECTRIFIED:
        for (uint8_t ty = 1; ty < ROOM_H - 1; ty++)
        {
            for (uint8_t tx = 1; tx < ROOM_W - 1; tx++)
            {
                if (((ty + tx) & 1u) == 0)
                {
                    room.tiles[ty][tx].electricity.enabled = 1;
                    room.tiles[ty][tx].electricity.voltage_mV = 12000;
                    room.tiles[ty][tx].electricity.resistance_mOhm = 6000;
                }
            }
        }

        room.tiles[1][2].material = MAT_CANISTER;
        room.tiles[1][2].mass_g = 1000;
        break;

    case ROOM_REACTOR:
        room.tiles[cy][cx].material = MAT_REACTOR;
        room.tiles[cy][cx].mass_g = 10000;
        break;
    }

    // Salvage nodes give long expeditions a renewable source of construction
    // metal to discover. Keep them off the central door-to-door route.
    if (kind != ROOM_REACTOR && kind != ROOM_SALVAGE)
    {
        room.tiles[2][2].material = MAT_IRON;
        room.tiles[2][2].mass_g = 4000;
        room.tiles[5][5].material = MAT_COPPER;
        room.tiles[5][5].mass_g = 3000;
    }
}


// ============================================================
// SIMPLE CHECKSUM
// ============================================================

static uint32_t roomChecksum(
    const Room& room)
{
    const uint8_t* data =
        reinterpret_cast<const uint8_t*>(
            room.tiles);

    const size_t size =
        sizeof(room.tiles);

    uint32_t hash =
        2166136261UL;

    for (size_t i = 0;
        i < size;
        i++)
    {
        hash ^= data[i];

        hash *= 16777619UL;
    }

    return hash;
}


// ============================================================
// ROOM FILE NAME
// ============================================================

static void roomFilename(char* buf, size_t sz, int16_t x, int16_t y)
{
    snprintf(buf, sz, "%s%d_%d.dat", ROOM_DIRECTORY, x, y);
}


// ============================================================
// ENSURE GAME DIRECTORY
// ============================================================

static void ensureDirectories()
{
    pm_sd_mkdir("/oni");
    pm_sd_mkdir("/oni/rooms");

}


// ============================================================
// SAVE ROOM
// ============================================================

static bool saveRoom(
    const Room& room)
{
    char filename[48];
    roomFilename(filename, sizeof(filename), room.header.x, room.header.y);
    Room copy = room;
    copy.header.checksum = roomChecksum(copy);
    return pm_sd_write_binary_file(filename,
                                   reinterpret_cast<uint8_t*>(&copy),
                                   sizeof(Room));
}


// ============================================================
// LOAD ROOM
// ============================================================

static bool loadRoom(
    int16_t x,
    int16_t y)
{
    char filename[48];
    roomFilename(filename, sizeof(filename), x, y);
    uint32_t fileSize = (uint32_t)pm_sd_get_file_size(filename);
    if (fileSize != sizeof(Room)) {
        return false;
    }
    Room loaded{};
    bool success = pm_sd_read_binary_file(filename,
                                          reinterpret_cast<uint8_t*>(&loaded),
                                          sizeof(Room));


    if (!success)
        return false;

    if (loaded.header.magic != ROOM_MAGIC)
        return false;

    if (loaded.header.version != ROOM_VERSION)
        return false;

    if (loaded.header.x != x ||
        loaded.header.y != y ||
        loaded.header.runSeed != worldState.runSeed)
        return false;

    /*
        Verify checksum.

        Temporarily preserve stored checksum.
    */

    uint32_t stored =
        loaded.header.checksum;

    loaded.header.checksum = 0;

    uint32_t calculated =
        roomChecksum(loaded);

    loaded.header.checksum =
        stored;

    if (stored != calculated)
        return false;

    currentRoom = loaded;

    return true;
}


// ============================================================
// LOAD OR CREATE ROOM
// ============================================================

static void loadOrCreateRoom(
    int16_t x,
    int16_t y)
{
    if (!loadRoom(x, y))
    {
        generateRoom(
            currentRoom,
            x,
            y);

        saveRoom(currentRoom);
    }
}


// ============================================================
// SAVE WORLD
// ============================================================

static void saveWorld()
{
    pm_sd_write_binary_file(WORLD_FILE, reinterpret_cast<uint8_t*>(&worldState), sizeof(worldState));
}


// ============================================================
// LOAD WORLD
// ============================================================

static bool loadWorld()
{
    uint32_t size = (uint32_t)pm_sd_get_file_size(WORLD_FILE);
    if (size != sizeof(WorldState)) return false;
    return pm_sd_read_binary_file(WORLD_FILE, reinterpret_cast<uint8_t*>(&worldState), sizeof(WorldState));
}


// ============================================================
// LOAD INITIAL WORLD
// ============================================================

static void initializeWorld()
{
    ensureDirectories();

    if (!loadWorld())
    {
        worldState.roomX = 0;
        worldState.roomY = 0;

        worldState.playerX = 4;
        worldState.playerY = 4;

        worldState.globalTick = 0;

        worldState.tank = TANK_START;

        worldState.outcome = OUTCOME_RUNNING;
        worldState.runSeed = nowMillis() ^ 0xD35CE17u;
        worldState.stone = 12;
        worldState.scrap = 8;
        worldState.water = 12;
        worldState.food = 8;
        worldState.power = 8;
        worldState.buildSelection = MAT_GARDEN;

        saveWorld();
    }

    loadOrCreateRoom(
        worldState.roomX,
        worldState.roomY);

    requestRedraw();
}


// ============================================================
// NEW RUN
// ============================================================

/*
    Start a fresh descent: back to the landing room with a full tank. The room
    layout is deterministic, so only the world state needs resetting.
*/

static void newRun()
{
    worldState.roomX = 0;
    worldState.roomY = 0;

    worldState.playerX = 4;
    worldState.playerY = 4;

    worldState.globalTick = 0;

    worldState.tank = TANK_START;
    worldState.outcome = OUTCOME_RUNNING;
    worldState.runSeed = nowMillis() ^ (worldState.runSeed * 1664525u + 1013904223u);
    worldState.stone = 12;
    worldState.scrap = 8;
    worldState.water = 12;
    worldState.food = 8;
    worldState.power = 8;
    worldState.buildSelection = MAT_GARDEN;

    saveWorld();

    loadOrCreateRoom(
        worldState.roomX,
        worldState.roomY);

    pm_oled_sysmsg("NEW RUN", 1000);

    requestRedraw();
}


// ============================================================
// CHANGE ROOM
// ============================================================

/*
    Runs when the player arrives on a tile; defined with the other life-support
    helpers, after the simulation, so it needs a forward declaration here.
*/

static void enterTile();

static void advanceTurn();

static void changeRoom(
    int16_t newX,
    int16_t newY,
    uint8_t newPlayerX,
    uint8_t newPlayerY)
{
    /*
        Save the room we're leaving.
    */

    currentRoom.header.simulationTick =
        worldState.globalTick;

    saveRoom(currentRoom);


    /*
        Change coordinates.
    */

    worldState.roomX = newX;
    worldState.roomY = newY;

    worldState.playerX =
        newPlayerX;

    worldState.playerY =
        newPlayerY;


    /*
        Load or generate destination.
    */

    loadOrCreateRoom(
        newX,
        newY);


    enterTile();

    advanceTurn();

    saveWorld();
}


// ============================================================
// PLAYER MOVEMENT
// ============================================================

static void movePlayer(
    int dx,
    int dy)
{
    int x =
        worldState.playerX;

    int y =
        worldState.playerY;

    int nx = x + dx;
    int ny = y + dy;


    /*
        Normal movement.
    */

    if (nx >= 0 &&
        nx < ROOM_W &&
        ny >= 0 &&
        ny < ROOM_H)
    {
        Tile& destination =
            currentRoom.tiles[ny][nx];

        /*
            Stone is currently impassable.
        */

        if (destination.material == MAT_STONE || destination.material == MAT_WALL)
        {
            return;
        }

        worldState.playerX =
            nx;

        worldState.playerY =
            ny;

        enterTile();

        advanceTurn();

        return;
    }


    /*
        ROOM EXITS
    */

    const uint8_t cx =
        ROOM_W / 2;

    const uint8_t cy =
        ROOM_H / 2;


    /*
        NORTH
    */

    if (dy < 0 &&
        y == 0 &&
        x == cx)
    {
        changeRoom(
            worldState.roomX,
            worldState.roomY - 1,

            cx,
            ROOM_H - 1
        );

        return;
    }


    /*
        SOUTH
    */

    if (dy > 0 &&
        y == ROOM_H - 1 &&
        x == cx)
    {
        changeRoom(
            worldState.roomX,
            worldState.roomY + 1,

            cx,
            0
        );

        return;
    }


    /*
        WEST
    */

    if (dx < 0 &&
        x == 0 &&
        y == cy)
    {
        changeRoom(
            worldState.roomX - 1,
            worldState.roomY,

            ROOM_W - 1,
            cy
        );

        return;
    }


    /*
        EAST
    */

    if (dx > 0 &&
        x == ROOM_W - 1 &&
        y == cy)
    {
        changeRoom(
            worldState.roomX + 1,
            worldState.roomY,

            0,
            cy
        );

        return;
    }
}


// ============================================================
// GAS SIMULATION
// ============================================================

/*
    Diffuse one gas component across the room.

    Each tile exchanges with its four orthogonal neighbours from a snapshot of
    the previous tick, so the result does not depend on iteration order. Stone
    acts as a wall: gas neither enters it nor leaves it.
*/

static void diffuseGas(
    uint16_t Gas::* field)
{
    static uint16_t before[ROOM_H][ROOM_W];

    for (uint8_t y = 0; y < ROOM_H; y++)
    {
        for (uint8_t x = 0; x < ROOM_W; x++)
        {
            before[y][x] =
                currentRoom.tiles[y][x].gas.*field;
        }
    }

    for (uint8_t y = 0; y < ROOM_H; y++)
    {
        for (uint8_t x = 0; x < ROOM_W; x++)
        {
            Tile& tile =
                currentRoom.tiles[y][x];

            if ((tile.material == MAT_STONE || tile.material == MAT_WALL))
            {
                tile.gas.*field = 0;
                continue;
            }

            const int32_t self =
                before[y][x];

            int32_t value = self;


            if (x > 0 &&
                (currentRoom.tiles[y][x - 1].material != MAT_STONE && currentRoom.tiles[y][x - 1].material != MAT_WALL))
            {
                value += (before[y][x - 1] - self) /
                    GAS_DIFFUSE_DIV;
            }

            if (x + 1 < ROOM_W &&
                (currentRoom.tiles[y][x + 1].material != MAT_STONE && currentRoom.tiles[y][x + 1].material != MAT_WALL))
            {
                value += (before[y][x + 1] - self) /
                    GAS_DIFFUSE_DIV;
            }

            if (y > 0 &&
                (currentRoom.tiles[y - 1][x].material != MAT_STONE && currentRoom.tiles[y - 1][x].material != MAT_WALL))
            {
                value += (before[y - 1][x] - self) /
                    GAS_DIFFUSE_DIV;
            }

            if (y + 1 < ROOM_H &&
                (currentRoom.tiles[y + 1][x].material != MAT_STONE && currentRoom.tiles[y + 1][x].material != MAT_WALL))
            {
                value += (before[y + 1][x] - self) /
                    GAS_DIFFUSE_DIV;
            }


            if (value < 0)
                value = 0;
            else if (value > 0xFFFF)
                value = 0xFFFF;


            tile.gas.*field =
                static_cast<uint16_t>(value);
        }
    }
}


static void simulateGas()
{
    diffuseGas(&Gas::oxygen);
    diffuseGas(&Gas::nitrogen);
    diffuseGas(&Gas::co2);
    diffuseGas(&Gas::hydrogen);
    diffuseGas(&Gas::helium);
    diffuseGas(&Gas::methane);
    diffuseGas(&Gas::steam);
}


// ============================================================
// LIQUID SIMULATION
// ============================================================

static void simulateWater()
{
    /*
        Bottom-to-top update.

        Water tries to fall first.
    */

    for (int y = ROOM_H - 2;
        y >= 1;
        y--)
    {
        for (int x = 1;
            x < ROOM_W - 1;
            x++)
        {
            Tile& source =
                currentRoom.tiles[y][x];

            if (source.material !=
                MAT_WATER)
            {
                continue;
            }

            if (source.mass_g == 0)
                continue;


            Tile& below =
                currentRoom.tiles[y + 1][x];


            /*
                Fall downward.
            */

            if (below.material ==
                MAT_EMPTY &&
                below.mass_g == 0)
            {
                uint32_t amount =
                    std::min(
                        source.mass_g,
                        (uint32_t)100
                    );

                source.mass_g -= amount;

                below.material =
                    MAT_WATER;

                below.mass_g += amount;

                below.temperature_c100 =
                    source.temperature_c100;

                continue;
            }


            /*
                Spread left.
            */

            if (x > 1)
            {
                Tile& left =
                    currentRoom.tiles[y][x - 1];

                if (left.material ==
                    MAT_EMPTY)
                {
                    uint32_t amount =
                        std::min(
                            source.mass_g,
                            (uint32_t)25
                        );

                    source.mass_g -= amount;

                    left.material =
                        MAT_WATER;

                    left.mass_g += amount;

                    left.temperature_c100 =
                        source.temperature_c100;
                }
            }


            /*
                Spread right.
            */

            if (x < ROOM_W - 2)
            {
                Tile& right =
                    currentRoom.tiles[y][x + 1];

                if (right.material ==
                    MAT_EMPTY)
                {
                    uint32_t amount =
                        std::min(
                            source.mass_g,
                            (uint32_t)25
                        );

                    source.mass_g -= amount;

                    right.material =
                        MAT_WATER;

                    right.mass_g += amount;

                    right.temperature_c100 =
                        source.temperature_c100;
                }
            }
        }
    }
}


// ============================================================
// ELECTRICITY
// ============================================================

static void simulateElectricity()
{
    for (uint8_t y = 0;
        y < ROOM_H;
        y++)
    {
        for (uint8_t x = 0;
            x < ROOM_W;
            x++)
        {
            Electrical& e =
                currentRoom.tiles[y][x]
                .electricity;

            if (!e.enabled)
                continue;

            if (e.resistance_mOhm == 0)
                continue;


            /*
                I = V / R

                mA = mV / ohms

                Since resistance is stored
                in milliohms:

                    mA =
                        mV * 1000 / mOhm
            */

            e.current_mA =
                (
                    static_cast<uint32_t>(
                        e.voltage_mV
                        ) * 1000
                    ) /
                e.resistance_mOhm;


            /*
                P = V * I

                mW =
                    mV * mA / 1000
            */

            e.power_mW =
                (
                    static_cast<uint32_t>(
                        e.voltage_mV
                        ) *
                    e.current_mA
                    ) / 1000;


            /*
                Convert electrical power into heat.

                The rate is exaggerated for gameplay, but it is finite: the
                temperature solver relaxes every tile toward ambient, so a
                circuit settles at a steady temperature instead of running away.
            */

            currentRoom.tiles[y][x]
                .temperature_c100 +=
                e.power_mW / ELECTRICAL_HEAT_DIV;
        }
    }
}


// ============================================================
// TEMPERATURE
// ============================================================

/*
    Solve one diffusion step for temperature.

    Every tile reads its four orthogonal neighbours from a snapshot of the
    previous tick, so the result does not depend on iteration order. A pull
    toward ambient gives the world a heat sink, and the result is clamped to a
    sane range so a stuck source cannot overflow the temperature field.
*/

static void simulateTemperature()
{
    static int32_t before[ROOM_H][ROOM_W];

    /*
        The hull leaks toward the room's ambient, not a global constant, so a
        hot room stays hot and a cold room stays cold.
    */

    int32_t ambient =
        currentRoom.header.ambient_c100;

    if (ambient == 0)
        ambient = NORMAL_AMBIENT_C100;

    for (uint8_t y = 0; y < ROOM_H; y++)
    {
        for (uint8_t x = 0; x < ROOM_W; x++)
        {
            before[y][x] =
                currentRoom.tiles[y][x].temperature_c100;
        }
    }

    for (uint8_t y = 0; y < ROOM_H; y++)
    {
        for (uint8_t x = 0; x < ROOM_W; x++)
        {
            const int32_t self =
                before[y][x];

            int32_t value = self;


            if (x > 0)
            {
                value += (before[y][x - 1] - self) /
                    HEAT_CONDUCT_DIV;
            }

            if (x + 1 < ROOM_W)
            {
                value += (before[y][x + 1] - self) /
                    HEAT_CONDUCT_DIV;
            }

            if (y > 0)
            {
                value += (before[y - 1][x] - self) /
                    HEAT_CONDUCT_DIV;
            }

            if (y + 1 < ROOM_H)
            {
                value += (before[y + 1][x] - self) /
                    HEAT_CONDUCT_DIV;
            }


            value += (ambient - self) /
                HEAT_RELAX_DIV;


            if (value < MIN_TEMPERATURE_C100)
                value = MIN_TEMPERATURE_C100;
            else if (value > MAX_TEMPERATURE_C100)
                value = MAX_TEMPERATURE_C100;


            currentRoom.tiles[y][x].temperature_c100 =
                value;
        }
    }
}


// ============================================================
// PHASE CHANGES
// ============================================================

static void simulatePhaseChanges()
{
    for (uint8_t y = 0;
        y < ROOM_H;
        y++)
    {
        for (uint8_t x = 0;
            x < ROOM_W;
            x++)
        {
            Tile& t =
                currentRoom.tiles[y][x];


            /*
                WATER -> STEAM
            */

            if (t.material ==
                MAT_WATER &&
                t.temperature_c100 >=
                10000)
            {
                /*
                    Convert 1% of water
                    into steam.
                */

                uint32_t amount =
                    std::max(
                        (uint32_t)1,
                        (uint32_t)(t.mass_g / 100)
                    );

                t.mass_g -= amount;

                t.gas.steam +=
                    amount;

                if (t.mass_g == 0)
                {
                    t.material =
                        MAT_EMPTY;
                }
            }


            /*
                ICE -> WATER
            */

            if (t.material ==
                MAT_ICE &&
                t.temperature_c100 > 0)
            {
                t.material =
                    MAT_WATER;
            }


            /*
                WATER -> ICE
            */

            if (t.material ==
                MAT_WATER &&
                t.temperature_c100 < 0)
            {
                t.material =
                    MAT_ICE;
            }
        }
    }
}


// ============================================================
// LIFE SUPPORT
// ============================================================

static bool isHazardousTile(const Tile& tile)
{
    if (tile.electricity.enabled)
        return true;

    if (tile.temperature_c100 >= HOT_TEMPERATURE_C100)
        return true;

    if (tile.temperature_c100 <= COLD_TEMPERATURE_C100)
        return true;

    if (tile.gas.oxygen < LOW_OXYGEN)
        return true;

    return false;
}


/*
    Bleed the personal O2 reserve for one turn. A hazard drains it faster and an
    empty reserve ends the run.
*/

static void drainTank()
{
    const Tile& here =
        currentRoom.tiles
        [worldState.playerY]
        [worldState.playerX];

    const int drain =
        isHazardousTile(here)
        ? TANK_DRAIN_HAZARD
        : TANK_DRAIN_IDLE;

    if (worldState.tank <= static_cast<uint16_t>(drain))
    {
        worldState.tank = 0;
        worldState.outcome = OUTCOME_LOST;
        return;
    }

    worldState.tank -= static_cast<uint16_t>(drain);
}


/*
    Run on the tile the player just entered: pick up a canister or reach the
    reactor.
*/

static void enterTile()
{
    Tile& here =
        currentRoom.tiles
        [worldState.playerY]
        [worldState.playerX];

    if (here.material == MAT_CANISTER)
    {
        worldState.tank = static_cast<uint16_t>(
            std::min<int>(
                TANK_MAX,
                worldState.tank + TANK_CANISTER
            )
        );

        here.material = MAT_EMPTY;
        here.mass_g = 0;

        pm_bz_play_jingle(0);
    }

    if (here.material == MAT_REACTOR)
    {
        worldState.outcome = OUTCOME_WON;
    }

    requestRedraw();
}


// ============================================================
// PHYSICS
// ============================================================

static void physicsStep()
{
    simulateElectricity();

    simulateTemperature();

    simulatePhaseChanges();

    simulateWater();

    simulateGas();

    currentRoom.header.simulationTick++;

    worldState.globalTick++;
}


// ============================================================
// TURN
// ============================================================

/*
    The game is turn-based so the panel only refreshes on a deliberate action.
    Every action folds a few physics steps into one turn and then drains the suit
    once, based on the tile the player ends the turn on.
*/


// ============================================================
// BUILDING, MINING, AND LIFE-SUPPORT ECONOMY
// ============================================================

static const char* buildName(uint8_t material)
{
    switch (material)
    {
    case MAT_WALL: return "wall";
    case MAT_GARDEN: return "garden";
    case MAT_RECYCLER: return "water recycler";
    case MAT_SOLAR: return "solar array";
    case MAT_BATTERY: return "battery";
    case MAT_FABRICATOR: return "fabricator";
    default: return "none";
    }
}

static void selectBuild(uint8_t material)
{
    worldState.buildSelection = material;
    char msg[48];
    snprintf(msg, sizeof(msg), "BUILD: %s", buildName(material));
    pm_oled_sysmsg(msg, 900);
    requestRedraw();
}

static void buildAtPlayer()
{
    Tile& t = currentRoom.tiles[worldState.playerY][worldState.playerX];
    const uint8_t b = worldState.buildSelection;

    if (t.material != MAT_EMPTY)
    {
        pm_oled_sysmsg("BUILD ON EMPTY FLOOR", 1000);
        return;
    }

    // Small, integer-only recipes suitable for the PDA.
    uint16_t stoneCost = 0, scrapCost = 0;
    switch (b)
    {
    case MAT_WALL:       stoneCost = 3; scrapCost = 1; break;
    case MAT_GARDEN:     scrapCost = 2; break;
    case MAT_RECYCLER:   scrapCost = 3; break;
    case MAT_SOLAR:      scrapCost = 2; break;
    case MAT_BATTERY:    scrapCost = 2; break;
    case MAT_FABRICATOR: stoneCost = 2; scrapCost = 4; break;
    default: return;
    }

    if (worldState.stone < stoneCost || worldState.scrap < scrapCost)
    {
        pm_oled_sysmsg("NEED MORE MATERIALS", 1000);
        return;
    }

    worldState.stone -= stoneCost;
    worldState.scrap -= scrapCost;
    t.material = b;
    t.mass_g = 1000;
    if (b == MAT_WALL)
    {
        t.gas.oxygen = 0; t.gas.nitrogen = 0; t.gas.co2 = 0;
        t.gas.hydrogen = 0; t.gas.helium = 0; t.gas.methane = 0; t.gas.steam = 0;
    }
    else if (b == MAT_GARDEN)
    {
        // Start with a little moisture in the growing medium.
        t.mass_g = 3000;
    }

    char msg[48];
    snprintf(msg, sizeof(msg), "BUILT %s", buildName(b));
    pm_oled_sysmsg(msg, 900);
    requestRedraw();
    saveRoom(currentRoom);
    saveWorld();
}

static void mineNearby()
{
    // Mine the first solid tile in N,E,S,W order. This keeps controls simple
    // on a tiny keyboard and makes mining walls usable without a cursor mode.
    static const int dx[4] = {0, 1, 0, -1};
    static const int dy[4] = {-1, 0, 1, 0};
    for (int i = 0; i < 4; ++i)
    {
        int x = static_cast<int>(worldState.playerX) + dx[i];
        int y = static_cast<int>(worldState.playerY) + dy[i];
        if (x < 0 || x >= ROOM_W || y < 0 || y >= ROOM_H) continue;
        Tile& t = currentRoom.tiles[y][x];
        if (t.material == MAT_STONE || t.material == MAT_WALL ||
            t.material == MAT_IRON || t.material == MAT_COPPER)
        {
            if (t.material == MAT_STONE || t.material == MAT_WALL)
                worldState.stone = std::min<uint16_t>(65535, worldState.stone + 2);
            else
                worldState.scrap = std::min<uint16_t>(65535, worldState.scrap + 2);

            t = makeEmptyTile();
            t.temperature_c100 = currentRoom.header.ambient_c100;
            pm_oled_sysmsg("MINED + MATERIALS", 800);
            requestRedraw();
            saveRoom(currentRoom);
            saveWorld();
            return;
        }
    }
    pm_oled_sysmsg("NO MINEABLE TILE NEARBY", 900);
}

// Called once per player turn. Solar arrays generate power, recyclers make
// water from recovered humidity, gardens convert water/energy/CO2 into food
// and oxygen. Recycling is deliberately renewable but still power-limited.
static void simulateColony()
{
    for (uint8_t y = 0; y < ROOM_H; ++y)
        for (uint8_t x = 0; x < ROOM_W; ++x)
            if (currentRoom.tiles[y][x].material == MAT_SOLAR)
                worldState.power = std::min<uint16_t>(999, worldState.power + 2);

    for (uint8_t y = 0; y < ROOM_H; ++y)
    {
        for (uint8_t x = 0; x < ROOM_W; ++x)
        {
            Tile& t = currentRoom.tiles[y][x];
            if (t.material == MAT_RECYCLER && worldState.power >= 1)
            {
                --worldState.power;
                worldState.water = std::min<uint16_t>(999, worldState.water + 2);
            }
            else if (t.material == MAT_GARDEN && worldState.power >= 1 &&
                     worldState.water >= 1)
            {
                --worldState.power;
                --worldState.water;
                worldState.food = std::min<uint16_t>(999, worldState.food + 1);
                t.gas.oxygen = std::min<uint16_t>(1000, t.gas.oxygen + 18);
                t.gas.co2 = t.gas.co2 > 12 ? t.gas.co2 - 12 : 0;
            }
            else if (t.material == MAT_FABRICATOR && worldState.power >= 2 &&
                     worldState.stone >= 1)
            {
                worldState.power -= 2;
                --worldState.stone;
                ++worldState.scrap;
            }
            else if (t.material == MAT_BATTERY)
            {
                // A battery tile stores a small reserve abstractly as power.
                // Solar surplus remains available to the colony inventory.
            }
        }
    }
}

static void eatFood()
{
    // Food is consumed slowly; a small starter reserve prevents immediate
    // failure while encouraging gardens before the reserve runs out.
    if ((worldState.globalTick % 8u) == 0u)
    {
        if (worldState.food > 0)
            --worldState.food;
        else
        {
            worldState.tank = worldState.tank > 3 ? worldState.tank - 3 : 0;
            if (worldState.tank == 0) worldState.outcome = OUTCOME_LOST;
        }
    }
}

static void advanceTurn()
{
    if (worldState.outcome != OUTCOME_RUNNING)
        return;

    for (int i = 0; i < TURN_PHYSICS_TICKS; i++)
        physicsStep();

    simulateColony();
    eatFood();
    drainTank();

    currentRoom.header.simulationTick = worldState.globalTick;
    saveRoom(currentRoom);
    saveWorld();
    requestRedraw();
}


// ============================================================
// TILE SYMBOL
// ============================================================

static constexpr uint16_t GAS_NOTICE = 300;

static char tileSymbol(
    const Tile& tile)
{
    if ((tile.material == MAT_STONE || tile.material == MAT_WALL))
        return '#';

    if (tile.material == MAT_CANISTER)
        return 'o';

    if (tile.material == MAT_REACTOR)
        return 'R';

    if (tile.material == MAT_GARDEN)
        return 'g';

    if (tile.material == MAT_RECYCLER)
        return 'r';

    if (tile.material == MAT_SOLAR)
        return 's';

    if (tile.material == MAT_BATTERY)
        return 'b';

    if (tile.material == MAT_FABRICATOR)
        return 'f';

    if (tile.material == MAT_WALL)
        return '#';

    if (tile.material == MAT_WOOD)
        return 'W';

    if (tile.material == MAT_IRON)
        return 'I';

    if (tile.material == MAT_COPPER)
        return 'C';

    if (tile.material == MAT_WATER)
        return '~';

    if (tile.material == MAT_ICE)
        return '*';


    /*
        Hazard markers take precedence over the atmosphere symbol so the danger
        is visible before the player steps onto it.
    */

    if (tile.electricity.enabled)
        return 'e';

    if (tile.temperature_c100 >= HOT_TEMPERATURE_C100)
        return '!';

    if (tile.temperature_c100 <= COLD_TEMPERATURE_C100)
        return 'x';

    if (tile.gas.oxygen < LOW_OXYGEN)
        return 'x';


    /*
        Floor reads as a plain dot unless a gas is clearly elevated, so the
        normal oxygen/nitrogen atmosphere stays quiet and only real hazards get
        a mark.
    */

    if (tile.gas.co2 > GAS_NOTICE)
        return 'C';

    if (tile.gas.methane > GAS_NOTICE)
        return 'M';

    if (tile.gas.steam > GAS_NOTICE)
        return 'S';

    if (tile.gas.hydrogen > GAS_NOTICE)
        return 'H';

    if (tile.gas.helium > GAS_NOTICE)
        return 'h';

    return '.';
}


// ============================================================
// MATERIAL NAME
// ============================================================

static const char* materialName(uint8_t material)
{
    switch (material)
    {
    case MAT_EMPTY:    return "air";
    case MAT_STONE:    return "stone";
    case MAT_DIRT:     return "dirt";
    case MAT_WOOD:     return "wood";
    case MAT_IRON:     return "iron";
    case MAT_COPPER:   return "copper";
    case MAT_WATER:    return "water";
    case MAT_ICE:      return "ice";
    case MAT_OXYGEN:   return "oxygen";
    case MAT_NITROGEN: return "nitrogen";
    case MAT_CO2:      return "CO2";
    case MAT_HYDROGEN: return "hydrogen";
    case MAT_HELIUM:   return "helium";
    case MAT_METHANE:  return "methane";
    case MAT_STEAM:    return "steam";
    case MAT_CANISTER: return "canister";
    case MAT_REACTOR:  return "reactor";
    default:           return "?";
    }
}


// ============================================================
// E-INK DRAWING ADAPTER
// ============================================================

/*
    Thin wrappers over the pm_eink and pm_text primitives so the
    game code keeps its own drawing vocabulary. pm_eink_rect
    takes two corners, so the wrappers convert from the
    top-left-plus-size form used here.
*/

static void PM_DrawRect(
    int x,
    int y,
    int w,
    int h,
    bool filled)
{
    pm_eink_rect(
        x,
        y,
        x + w - 1,
        y + h - 1,
        filled,
        true
    );
}


static void PM_DrawText(int x, int y, const char* text)
{
    pm_text(PM_TARGET_EINK, x, y, text, PM_STYLE_MONO);
}


static void PM_DrawTextTiny(int x, int y, const char* text)
{
    pm_text(PM_TARGET_EINK, x, y, text, PM_STYLE_TINY);
}


/*
    The OLED is 256x32. It carries a persistent four-line HUD: location and the
    distance to the reactor, the life-support reserves, then the controls. It is
    the quick-glance readout; the e-ink column carries the detail.
*/

static int roomDistance()
{
    int dx =
        worldState.roomX - REACTOR_ROOM_X;

    int dy =
        worldState.roomY - REACTOR_ROOM_Y;

    if (dx < 0)
        dx = -dx;

    if (dy < 0)
        dy = -dy;

    return dx + dy;
}


static void drawOledHud()
{
    pm_text_color(PM_TARGET_OLED, 1);

    /*
        oledWord with an empty word clears and commits the framebuffer; no
        bare OLED clear primitive is exported.
    */

    pm_oled_oled_word("", false, false, "");

    char line[64];

    snprintf(
        line,
        sizeof(line),
        "DERELICT  R%d,%d  REACTOR %d",
        worldState.roomX,
        worldState.roomY,
        roomDistance()
    );

    pm_text(PM_TARGET_OLED, 2, 6, line, PM_STYLE_TINY);


    const Tile& playerTile =
        currentRoom
            .tiles
            [worldState.playerY]
            [worldState.playerX];

    const int32_t t100 =
        playerTile.temperature_c100;

    const int32_t magnitude =
        t100 < 0 ? -t100 : t100;

    snprintf(
        line,
        sizeof(line),
        "TANK %u%%  O2 %u  T %s%d.%dC",
        worldState.tank,
        playerTile.gas.oxygen,
        t100 < 0 ? "-" : "",
        static_cast<int>(magnitude / 100),
        static_cast<int>((magnitude % 100) / 10)
    );

    pm_text(PM_TARGET_OLED, 2, 13, line, PM_STYLE_TINY);


    if (worldState.outcome == OUTCOME_WON)
    {
        pm_text(
            PM_TARGET_OLED,
            2,
            20,
            "REACTOR RESTARTED!",
            PM_STYLE_TINY
        );
    }
    else if (worldState.outcome == OUTCOME_LOST)
    {
        pm_text(
            PM_TARGET_OLED,
            2,
            20,
            "SUIT FAILURE - O2 OUT",
            PM_STYLE_TINY
        );
    }
    else if (isHazardousTile(playerTile))
    {
        pm_text(
            PM_TARGET_OLED,
            2,
            20,
            "!! HAZARD  TANK DRAINING",
            PM_STYLE_TINY
        );
    }
    else
    {
        pm_text(
            PM_TARGET_OLED,
            2,
            20,
            "WASD move  SPACE wait  H help",
            PM_STYLE_TINY
        );
    }


    pm_text(
        PM_TARGET_OLED,
        2,
        27,
        "N new run  Q save  ESC quit",
        PM_STYLE_TINY
    );

    pm_oled_send();
}


// ============================================================
// DRAW ROOM
// ============================================================

static void drawRoom()
{
    /*
        Clear display and draw text as black ink; the e-ink default foreground is
        white, which is invisible on the white panel.
    */

    pm_eink_clear();

    pm_text_color(PM_TARGET_EINK, 0);


    /*
        Draw 8x8 map.
    */

    for (uint8_t y = 0;
        y < ROOM_H;
        y++)
    {
        for (uint8_t x = 0;
            x < ROOM_W;
            x++)
        {
            const Tile& tile =
                currentRoom.tiles[y][x];


            int px =
                MAP_X +
                x * TILE_PIXELS;

            int py =
                MAP_Y +
                y * TILE_PIXELS;


            /*
                Draw tile outline.
            */

            PM_DrawRect(
                px,
                py,
                TILE_PIXELS,
                TILE_PIXELS,
                false
            );


            /*
                Draw solid tiles.
            */

            if (tile.material == MAT_STONE || tile.material == MAT_WALL)
            {
                PM_DrawRect(
                    px + 2,
                    py + 2,
                    TILE_PIXELS - 4,
                    TILE_PIXELS - 4,
                    true
                );
            }
            else
            {
                /*
                    Gas/liquid symbol.
                */

                char symbol[2] = { tileSymbol(tile), '\0' };

                const int symbolWidth =
                    pm_text_width(
                        PM_TARGET_EINK,
                        symbol,
                        PM_STYLE_MONO
                    );

                PM_DrawText(
                    px + (TILE_PIXELS - symbolWidth) / 2,
                    py + 19,
                    symbol
                );
            }
        }
    }


    /*
        Player.
    */

    int playerPX =
        MAP_X +
        worldState.playerX *
        TILE_PIXELS;

    int playerPY =
        MAP_Y +
        worldState.playerY *
        TILE_PIXELS;


    PM_DrawRect(
        playerPX + (TILE_PIXELS - 16) / 2,
        playerPY + (TILE_PIXELS - 16) / 2,
        16,
        16,
        true
    );


    /*
        Readout column, to the right of the map.
    */

    PM_DrawRect(
        MAP_WIDTH + 2,
        0,
        1,
        MAP_HEIGHT,
        true
    );


    const Tile& playerTile =
        currentRoom
            .tiles
            [worldState.playerY]
            [worldState.playerX];

    char status[48];

    int infoY = INFO_Y;

    PM_DrawText(INFO_X, infoY, "DERELICT");
    infoY += INFO_LINE;


    snprintf(
        status,
        sizeof(status),
        "R %d,%d",
        worldState.roomX,
        worldState.roomY
    );

    PM_DrawText(INFO_X, infoY, status);
    infoY += INFO_LINE;


    snprintf(
        status,
        sizeof(status),
        "POS %u,%u",
        worldState.playerX,
        worldState.playerY
    );

    PM_DrawText(INFO_X, infoY, status);
    infoY += INFO_LINE;


    snprintf(
        status,
        sizeof(status),
        "REACTOR %d",
        roomDistance()
    );

    PM_DrawText(INFO_X, infoY, status);
    infoY += INFO_LINE;


    snprintf(
        status,
        sizeof(status),
        "TANK %u%%",
        worldState.tank
    );

    PM_DrawText(INFO_X, infoY, status);
    infoY += INFO_LINE;


    snprintf(
        status,
        sizeof(status),
        "O2 %u",
        playerTile.gas.oxygen
    );

    PM_DrawText(INFO_X, infoY, status);
    infoY += INFO_LINE;


    {
        const int32_t t100 =
            playerTile.temperature_c100;

        const int32_t magnitude =
            t100 < 0 ? -t100 : t100;

        snprintf(
            status,
            sizeof(status),
            "T %s%d.%dC",
            t100 < 0 ? "-" : "",
            static_cast<int>(magnitude / 100),
            static_cast<int>((magnitude % 100) / 10)
        );

        PM_DrawText(INFO_X, infoY, status);
        infoY += INFO_LINE;
    }


    snprintf(
        status,
        sizeof(status),
        "P %umW",
        playerTile.electricity.power_mW
    );

    PM_DrawText(INFO_X, infoY, status);
    infoY += INFO_LINE;


    PM_DrawText(
        INFO_X,
        infoY,
        materialName(playerTile.material)
    );


    /*
        26 px legend band along the bottom, matching the OS status band height.
    */

    PM_DrawRect(
        0,
        STATUS_Y,
        PANEL_WIDTH,
        1,
        true
    );


    PM_DrawTextTiny(
        2,
        STATUS_Y + 9,
        "X mine  B build  1-6 choose machine  F eat"
    );


    if (worldState.outcome == OUTCOME_WON)
    {
        PM_DrawTextTiny(2, STATUS_Y + 21, "REACTOR RESTARTED  -  you made it out");
    }
    else if (worldState.outcome == OUTCOME_LOST)
    {
        PM_DrawTextTiny(2, STATUS_Y + 21, "SUIT FAILURE  -  O2 depleted");
    }
    else if (isHazardousTile(playerTile))
    {
        PM_DrawTextTiny(2, STATUS_Y + 21, "!! HAZARD  -  tank draining");
    }
    else
    {
        {
            char inv[100];
            snprintf(inv, sizeof(inv), "S%u M%u W%u F%u P%u  BUILD:%s",
                     worldState.stone, worldState.scrap, worldState.water,
                     worldState.food, worldState.power, buildName(worldState.buildSelection));
            PM_DrawTextTiny(2, STATUS_Y + 21, inv);
        }
    }


    drawOledHud();


    /*
        Push the frame to the panel. The render wrapper only reaches here when
        the visible frame changed, so no throttle is needed.
    */

    pm_eink_refresh();
}


// ============================================================
// BRIEFING
// ============================================================

static void PM_CenterText(int y, const char* text, int style)
{
    const int w =
        pm_text_width(PM_TARGET_EINK, text, style);

    pm_text(
        PM_TARGET_EINK,
        (PANEL_WIDTH - w) / 2,
        y,
        text,
        style
    );
}


/*
    The launch briefing. It states the premise, the goal and every control so a
    player who has never seen the app knows what to do.
*/

static void drawIntro()
{
    pm_eink_clear();

    pm_text_color(PM_TARGET_EINK, 0);

    PM_CenterText(34, "DERELICT DESCENT", PM_STYLE_HEADING3);

    pm_eink_rect(70, 42, PANEL_WIDTH - 70, 42, true, true);

    PM_CenterText(64, "Your ship's reactor is dead and", PM_STYLE_BODY);
    PM_CenterText(80, "the air is going with it.", PM_STYLE_BODY);

    PM_CenterText(98, "Mine walls, build rooms, grow food.", PM_STYLE_BODY);
    PM_CenterText(114, "Gardens make oxygen; solar makes power.", PM_STYLE_BODY);

    PM_CenterText(138, "WASD move SPACE wait X mine", PM_STYLE_TINY);
    PM_CenterText(150, "1-6 select build, B build here", PM_STYLE_TINY);
    PM_CenterText(162, "F eat food  N new run  Q save", PM_STYLE_TINY);

    PM_CenterText(184, "g garden r recycler s solar b battery", PM_STYLE_TINY);
    PM_CenterText(196, "f fabricator # wall o O2 canister", PM_STYLE_TINY);
    PM_CenterText(214, "Water recycles; keep power and food flowing.", PM_STYLE_TINY);

    PM_CenterText(238, "PRESS ANY KEY", PM_STYLE_CAPTION);

    pm_eink_refresh();

    pm_oled_oled_word("DERELICT", true, false, "press any key");
}


// ============================================================
// RENDER GATE
// ============================================================

static uint32_t mixSignature(uint32_t h, uint32_t v)
{
    h ^= v;

    h *= 16777619u;

    return h;
}


/*
    Hash of everything the screens actually show. A turn that only nudges hidden
    simulation state leaves this unchanged, so no refresh is issued.
*/

static uint32_t frameSignature()
{
    uint32_t h = 2166136261u;

    for (uint8_t y = 0; y < ROOM_H; y++)
    {
        for (uint8_t x = 0; x < ROOM_W; x++)
        {
            h = mixSignature(
                h,
                static_cast<uint8_t>(
                    tileSymbol(currentRoom.tiles[y][x])
                )
            );
        }
    }

    h = mixSignature(h, worldState.playerX);
    h = mixSignature(h, worldState.playerY);
    h = mixSignature(h, static_cast<uint16_t>(worldState.roomX));
    h = mixSignature(h, static_cast<uint16_t>(worldState.roomY));
    h = mixSignature(h, worldState.tank);
    h = mixSignature(h, worldState.outcome);
    h = mixSignature(h, worldState.stone);
    h = mixSignature(h, worldState.scrap);
    h = mixSignature(h, worldState.water);
    h = mixSignature(h, worldState.food);
    h = mixSignature(h, worldState.power);
    h = mixSignature(h, worldState.buildSelection);
    h = mixSignature(h, static_cast<uint32_t>(roomDistance()));

    const Tile& playerTile =
        currentRoom.tiles
        [worldState.playerY]
        [worldState.playerX];

    h = mixSignature(h, playerTile.gas.oxygen);
    h = mixSignature(h, playerTile.electricity.power_mW);
    h = mixSignature(
        h,
        static_cast<uint32_t>(playerTile.temperature_c100 + 5000)
    );
    h = mixSignature(h, playerTile.material);

    return h;
}


/*
    Draw only when the visible frame changes. This is the single point that
    decides whether the e-ink panel and the OLED are touched.
*/

static void renderFrame()
{
    const uint32_t signature =
        showIntro ? 0x1DEA0001u : frameSignature();

    if (frameDrawn && signature == lastFrameSignature)
    {
        redrawRequested = false;

        return;
    }

    lastFrameSignature = signature;

    frameDrawn = true;

    redrawRequested = false;

    if (showIntro)
        drawIntro();
    else
        drawRoom();
}


// ============================================================
// KEYBOARD
// ============================================================

static void handleGameKey(
    char key)
{
    switch (key)
    {
        /*
            Movement.
        */

    case 'w':
    case 'W':
        movePlayer(0, -1);
        break;


    case 's':
    case 'S':
        movePlayer(0, 1);
        break;


    case 'a':
    case 'A':
        movePlayer(-1, 0);
        break;


    case 'd':
    case 'D':
        movePlayer(1, 0);
        break;


        /*
            Wait one turn: time passes, nothing moves.
        */

    case ' ':
        advanceTurn();
        break;


    /*
        Show the briefing again.
    */

    case 'h':
    case 'H':
    case '?':

        showIntro = true;
        requestRedraw();

        break;


        /*
            Save current room.
        */

    case 'q':
    case 'Q':

        saveRoom(currentRoom);
        saveWorld();

        pm_oled_sysmsg(
            "GAME SAVED",
            1000
        );

        break;


        /*
            Restart the run.
        */

    case 'n':
    case 'N':

        newRun();

        break;


        /*
            Add water.
        */

    case 'l':
    case 'L':
    {
        Tile& t =
            currentRoom.tiles
            [worldState.playerY]
            [worldState.playerX];

        if (t.material ==
            MAT_EMPTY)
        {
            t.material =
                MAT_WATER;

            t.mass_g =
                1000;
        }

        requestRedraw();

        break;
    }


    /*
        Add oxygen.
    */

    case 'g':
    case 'G':
    {
        Tile& t =
            currentRoom.tiles
            [worldState.playerY]
            [worldState.playerX];

        t.gas.oxygen += 100;

        requestRedraw();

        break;
    }


    /*
        Add heat.
    */

    case 't':
    case 'T':
    {
        Tile& t =
            currentRoom.tiles
            [worldState.playerY]
            [worldState.playerX];

        t.temperature_c100 +=
            5000;

        requestRedraw();

        break;
    }


    /*
        Toggle electricity.
    */

    case 'e':
    case 'E':
    {
        Tile& t =
            currentRoom.tiles
            [worldState.playerY]
            [worldState.playerX];

        t.electricity.enabled =
            !t.electricity.enabled;

        t.electricity.voltage_mV =
            12000;

        t.electricity.resistance_mOhm =
            6000;

        requestRedraw();

        break;
    }


    case 'x':
    case 'X':
        mineNearby();
        break;

    case 'b':
    case 'B':
        buildAtPlayer();
        break;

    case '1': selectBuild(MAT_WALL); break;
    case '2': selectBuild(MAT_GARDEN); break;
    case '3': selectBuild(MAT_RECYCLER); break;
    case '4': selectBuild(MAT_SOLAR); break;
    case '5': selectBuild(MAT_BATTERY); break;
    case '6': selectBuild(MAT_FABRICATOR); break;

    case 'f':
    case 'F':
        if (worldState.food > 0)
        {
            --worldState.food;
            worldState.tank = std::min<uint16_t>(TANK_MAX, worldState.tank + 10);
            pm_oled_sysmsg("ATE FOOD +10 O2", 900);
            saveWorld();
            requestRedraw();
        }
        break;

    default:
        break;
    }
}


// ============================================================
// ENTRY POINT
// ============================================================

extern "C" int main(int argc, char** argv)
{
    const char* appName =
        argc > 0 && argv[0] ? argv[0] : "Derelict Descent";

    const char* elfPath =
        argc > 1 ? argv[1] : "";

    printf(
        "pocketmage app starting: %s (%s)\n",
        appName,
        elfPath
    );

    if (pm_app_abi() != PM_APP_API_ABI)
    {
        printf(
            "host ABI mismatch: app %u, host %u; this build is stale\n",
            PM_APP_API_ABI,
            static_cast<unsigned>(pm_app_abi())
        );

        pm_oled_sysmsg("SDK ABI mismatch", 2000);

        return 1;
    }

    printf(
        "built against sdk %s, host reports %s\n",
        PM_SDK_VERSION,
        pm_host_sdk_version()
    );

    pm_bz_play_jingle(0);

    initializeWorld();

    bool running = true;

    /*
        Track the first frame on which the run ends so the outcome can be shown
        briefly before the app exits.
    */

    uint8_t announcedOutcome = OUTCOME_RUNNING;

    uint32_t outcomeMillis = 0;

    while (running)
    {
        pm_kb_check_usbkb();

        const char key = pm_kb_read();

        if (key == K_EXIT_KEY || key == K_ESC_KEY)
        {
            running = false;
        }
        else if (key != 0 && pm_kb_accept_key())
        {
            if (showIntro)
            {
                showIntro = false;

                requestRedraw();
            }
            else
            {
                handleGameKey(key);
            }
        }

        const uint32_t now = nowMillis();

        if (worldState.outcome != OUTCOME_RUNNING &&
            announcedOutcome == OUTCOME_RUNNING)
        {
            announcedOutcome = worldState.outcome;

            outcomeMillis = now;

            pm_bz_play_jingle(
                worldState.outcome == OUTCOME_WON ? 0 : 1
            );

            requestRedraw();
        }

        if (redrawRequested)
        {
            renderFrame();
        }

        if (announcedOutcome != OUTCOME_RUNNING &&
            now - outcomeMillis >= OUTCOME_HOLD_MS)
        {
            running = false;
        }

        usleep(POLL_INTERVAL_MS * 1000);
    }

    saveRoom(currentRoom);
    saveWorld();

    pm_oled_sysmsg("GAME SAVED", 1000);

    pm_bz_play_jingle(1);

    return 0;
}


