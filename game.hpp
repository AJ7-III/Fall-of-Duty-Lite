#pragma once

#include "types.hpp"

#include <array>
#include <cstddef>
#include <memory>

namespace fod {

struct Assets;
struct MissionSnapshot;
enum class MissionChapter : uint8_t;
enum class Match : uint8_t { Start, Playing, Paused, Results };
enum class Device : uint8_t { None, Laptop, Transmitter };
enum class Weapon : uint8_t { Mp44, Sniper, Pistol };
enum class WeaponState : uint8_t { Idle, Cooldown, Bolt, Reload };
enum class EventKind : uint8_t {
    Fire, ReloadStart, ReloadRound, ReloadEnd, BoltStart, BoltEnd, Swap, Dry,
    WorldHit, GlassHit, GlassBreak, RivalHit, RivalKill, PlayerHit, PlayerDeath,
    PlayerRespawn, RivalRespawn, BotFire, Uav, AirstrikeReady, LaptopOpen,
    LaptopClose, StrikeMark, StrikeImpact, ApacheReady, ApacheLaunch, ApacheAttack,
    MatchEnd, Sound, BotKill
};

struct Event {
    EventKind kind = EventKind::Fire;
    uint32_t tick = 0;
    uint32_t surface = 0;
    int value = 0;
    Vec3 position{};
    Sound sound = Sound::Impact;
    float volume = 1;
    bool positional = false;
    // Rival slots are 0 and 1. The player and uncredited deaths use -1.
    int8_t rival = -1, attacker = -1;
};

struct Camera {
    Vec3 position{-13.5f, 1.7f, -13.5f};
    float yaw = pi / 4, pitch = 0, fov = 70 * pi / 180;
};

struct Actor {
    Vec3 position{};
    Vec3 motion{}; // Smoothed travel velocity for posing. Collision uses the actual velocity.
    float yaw = 0, pitch = 0, phase = 0, speed = 0, fall = 0, health = 100;
    float frameBlend = 0;
    uint16_t frame = 0, nextFrame = 0;
    uint8_t fallVariant = 0;
    bool visible = true;

    void trackMotion(Vec3 velocity, float amount) {
        motion += (velocity - motion) * amount;
    }
};

struct Jet {
    Vec3 position{};
    float yaw = 0;
    bool visible = false;
};

struct Tracer {
    Vec3 start{}, end{};
    float time = 0;
};

struct KillNotice {
    float remaining = 0;
    // Match events use -1 for the player and 0 and 1 for the rivals.
    int8_t attacker = -1, victim = -1;
};

struct Snapshot {
    Camera camera{};
    Actor player{}, rival{};
    Vec3 playerVelocity{}, apachePosition{};
    Vec3 weaponPosition{}, weaponRotation{}, tracerStart{}, tracerEnd{};
    Vec3 viewmodelSway{};
    std::array<Jet, 3> jets{};
    float playerYaw = pi / 4, playerPitch = 0, eyeHeight = 1.7f;
    float apacheYaw = 0, apachePitch = 0, apacheRoll = 0;
    float ads = 0, recoil = 0, reload = 0, bolt = 0, lower = 0;
    float weaponAnimationTime = 0, reloadDuration = 0;
    float landing = 0; // Viewmodel compression on contact with the ground.
    float sprintPose = 0, sprintPhase = 0;
    float weaponYawLag = 0, weaponPitchLag = 0;
    float cursorX = 0, cursorZ = 0, deviceAmount = 0, damage = 0;
    float hitMarker = 0, killConfirm = 0, muzzle = 0, scope = 0, tracer = 0, damageFromYaw = 0, uavSeconds = 0;
    double scaledTime = 0, realTime = 0, scaledDelta = 0, timeScale = 1;
    uint32_t tick = 0;
    uint64_t brokenGlass = 0;
    int playerScore = 0, rivalScore = 0, streak = 0, clip = 30, reserve = 90, difficulty = defaultDifficulty;
    Match match = Match::Start;
    Weapon weapon = Weapon::Mp44;
    WeaponState weaponState = WeaponState::Idle;
    Device device = Device::None;
    bool airstrikeReady = false, apacheReady = false, apacheActive = false;
    bool radarContact = false, strikeInbound = false;
    bool lethalMarker = false, headshot = false;
    bool emptyReload = false;
    bool dead = false, grounded = true, crouch = false, prone = false, sprint = false, alternateRival = false;
    Actor secondRival{};
    std::array<Tracer, 2> rivalTracers{};
    KillNotice killNotice{};
    int secondRivalScore = 0;
    bool freeForAll = false, secondRadarContact = false;
    std::shared_ptr<const MissionSnapshot> mission;

#ifdef FOD_LOCAL_DUEL
    float rivalHeightScale = 1.f;
    bool localDuel = false;
#endif

    int leadingRivalScore() const { return rivalScore > secondRivalScore ? rivalScore : secondRivalScore; }
};

struct Fixture {
    Vec3 player{-13.5f, 0, -13.5f};
    Vec3 rival{13.5f, 0, 13.5f};
    float yaw = pi / 4, pitch = 0, rivalYaw = -3 * pi / 4;
    int clip = -1, playerScore = 0, rivalScore = 0, streak = 0;
    uint32_t killPlayerAtTick = 0;
    Weapon weapon = Weapon::Mp44;
    bool freezeRival = false, airstrikeReady = false, apacheReady = false;
    Vec3 secondRival{-13.5f, 0, 13.5f};
    float secondRivalYaw = 3 * pi / 4;
    int secondRivalScore = 0;
    bool freezeSecondRival = false;
};

class Game {
public:
    explicit Game(const Assets& assets, uint32_t voiceSeed = 0);
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
    const Snapshot& state() const;
    const Event* events() const;
    size_t eventCount() const;
    void start(uint32_t seed, int difficulty, bool alternateRival = false);
    void start(uint32_t seed, int difficulty, Opponent opponent);
    void startMission(uint32_t seed, int difficulty, bool fromCheckpoint = false);
    void startCampaign(uint32_t seed, int difficulty);
    bool advanceCampaign(uint32_t seed, int difficulty);
    void startExtractionReplay(uint32_t seed, int difficulty);
    // Begin a later campaign mission directly, with Nora alive, for replays and qualification.
    void startChapterReplay(uint32_t seed, int difficulty, MissionChapter chapter);
    // Begin The Array at the rear door with the mothership's beam under the player's pointer.
    void startFinaleReplay(uint32_t seed, int difficulty);
    bool isMission() const;
    bool canRetryCheckpoint() const;
    void pause();
    // Use the trigger state sampled when the platform accepts Resume.
    void resume(bool triggerHeld = true);
    void quit();
    void setDifficulty(int difficulty);
    void setupFixture(const Fixture& fixture);
    void step(const Input& input, double realDelta = tickSeconds);
#ifdef FOD_LOCAL_DUEL
    void startLocalDuel(uint32_t seed);
    void stepLocalDuel(std::array<Input, 2> inputs);
    void resumeLocalDuel();
    const Snapshot& localView(int slot) const;
    // Use the player-side fields of each fixture after starting a local duel.
    void setupLocalDuelFixture(const std::array<Fixture, 2>& fixtures);
#endif
    // Fixtures can inject the browser's tagged gameplay draw stream. Return false to
    // fall back to the native PRNG. This stays out of live controller operation.
    using RandomHook = bool (*)(void*, uint32_t tick, const char* site, double& value);
    void setRandomHook(RandomHook hook, void* data);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fod
