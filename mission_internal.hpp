#pragma once

#include "game_bot.hpp"
#include "mission.hpp"

#include <array>
#include <vector>

namespace fod::mission_detail {

struct MissionHost {
    virtual ~MissionHost() = default;
    virtual void playSound(Sound id, float volume, bool positional, Vec3 at) = 0;
    virtual void damagePlayer(float amount, Vec3 from) = 0;
    virtual void hitWorld(const game_detail::ShotHit& hit, Vec3 point) = 0;
    virtual void updateActorFrame(Actor& actor) = 0;
    virtual void refillPlayer(bool newMatch) = 0;
};

// Radio lines interrupt by priority. Urgent lines react to the moment; orders carry
// instructions and wait their turn; chatter is dropped if it goes stale in the queue.
enum RadioPriority : uint8_t { RadioChatter = 0, RadioOrder = 1, RadioUrgent = 2 };

class MissionController {
public:
    MissionController(const game_detail::World& world, game_detail::Navigation& nav);

    void start(uint32_t seed, int difficulty, bool fromCheckpoint, Snapshot& snap,
               Vec3& playerPosOut, float& playerYawOut, MissionHost& host);
    void step(Snapshot& snap, const Input& input, double dt, bool playerFired, MissionHost& host);

    // Resolve a player weapon shot against mission actors. Returns true if it struck a mission actor.
    bool resolvePlayerShot(Vec3 origin, Vec3 dir, float damage, float worldDist, Snapshot& snap,
                           MissionHost& host, Vec3& hitPointOut);

    // Resolve Apache or Airstrike damage against active aliens.
    int nearestAlienForApache(Vec3 gunPos) const;
    bool apacheShotAlien(int index, Vec3 gunPos, Vec3 dir, float worldDist, float damage,
                         Snapshot& snap, MissionHost& host, Vec3& hitPointOut);
    void explodeAliens(Vec3 center, float radius, Snapshot& snap, MissionHost& host);

    bool active() const { return state_.active; }
    bool canRetryCheckpoint() const { return checkpointStage_ > 0; }
    void publish(std::shared_ptr<const MissionSnapshot>& target);

private:
    struct Agent {
        bool used = false;
        MissionRole role = MissionRole::AlienTrooper;
        uint8_t variant = 0;
        uint8_t tacRole = 0; // 0=patrol, 1=suppressor, 2=flanker, 3=vanguard, 4=support
        int8_t sector = -1; // 0: Kitchen, 1: Garage, 2: Shed, -1: Breach/Extraction/Commander
        Actor actor{};
        Vec3 velocity{};
        Vec3 home{};
        Vec3 goal{};
        std::vector<int> path{};
        size_t pathIndex = 0;
        double repathAt = 0;
        double fireCooldown = 0;
        double burstPause = 0;
        int burstLeft = 0;
        double threatTimer = 0;
        double deathElapsed = 0;
        double footstep = 0;
        double sightTime = 0; // Continuous seconds with eyes on the player; early bursts walk in
        double spawnFx = 0;   // Drop-beam effect remaining after a reinforcement lands
        double dodgeUntil = 0;// Commander keeps stepping out of the player's line of fire until then
        double flinchUntil = 0;// Civilian shrugs off further player rounds until then
        double leapAt = -1;    // Seconds into a vault over the front wall, or -1
        Vec3 leapFrom{}, leapTo{};
        // Ray gun animation, charge-up, phase shield & suppression state
        float gunPitch = 0;
        float gunCharge = 0;
        float gunRecoil = 0;
        double chargeTime = 0;
        double chargeDuration = 0;
        double overchargeCooldown = 0;
        double staggerUntil = 0;
        float suppression = 0;
        float shield = 0;
        float maxShield = 0;
        double shieldRegenAt = 0;
        int8_t linkTarget = -1; // For Drones: slot of ground alien receiving shield link
        bool linked = false;    // For Troopers/Enforcers: boosted by a Drone
        bool overcharging = false;
        bool chargeSoundPlayed = false;
        // Tactics: a firing spot around the player (or a hiding spot), re-planned every few seconds.
        Vec3 tacGoal{}, planFrom{};
        bool hasGoal = false, hiding = false, wantsCover = false, hidOnce = false;
        double replanAt = 0, hideUntil = 0;
        float maxHealth = 100;
        float radar = 0;
        float hoverBase = 2.0f;
        float hoverPhase = 0;
        bool dead = false;
        bool rescued = false;
        bool alerted = false;
        bool seesPlayer = false;
        bool token = false;     // Holds one of the limited aimed-fire slots against the player
        bool following = false; // Commander is closing distance to the player
        bool boarded = false;   // Climbed aboard the evac bird
        uint8_t deathVariant = 0;
    };

    struct SectorState {
        bool spawned = false;
        bool triggered = false;
        bool resolved = false;
        bool warned = false; // Delta-1 has called out fire on this sector's civilian
        double clearTimer = 0;
        int civSlot = 1;
        Vec3 center{};
    };

    struct RadioLine {
        char speaker[20]{};
        char text[96]{};
        float duration = 0;
        uint8_t priority = RadioOrder;
        int8_t sector = -1; // Sector-specific lines are dropped once that sector resolves
        double queuedAt = 0;
    };

    // Progress carried across a checkpoint reload so results reflect the whole run.
    struct CheckpointStats {
        int saved = 0, lost = 0, kills = 0, headshots = 0;
        int interrupts = 0, shieldsBroken = 0, ambushKills = 0;
        float seconds = 0;
        std::array<uint8_t, 3> civilians{}; // 0 pinned, 1 rescued, 2 lost
    };

    static Vec3 mapPoint(float x, float y, float height = 0.f);
    static Vec3 rayGunMuzzle(const Agent& a);
    void resetAll();
    void setRadio(const char* speaker, const char* line, float duration, MissionHost& host, bool chirp = true,
                  uint8_t priority = RadioOrder, int sector = -1);
    void clearRadioQueue() { radioQueued_ = 0; }
    void showRadio(const RadioLine& line, MissionHost& host, bool chirp);
    void pumpRadio(MissionHost& host);
    void setObjective(const char* mainText, const char* subText);
    void setTacticalCallout(const char* text, uint16_t color, float duration = 1.6f);
    void addTracer(Vec3 from, Vec3 to, uint16_t color, TracerStyle style = TracerStyle::Ballistic, float life = 1.f);
    void updateDroneLinks(double dt);
    void updateMistDensity(double dt);
    void spawnCommander(Vec3 pos, float yaw);
    void spawnCivilian(int slot, int sector, Vec3 pos, float yaw, uint8_t outfit);
    int spawnAlien(MissionRole role, int sector, Vec3 pos, float yaw, uint8_t variant, int difficulty);
    int dropAlien(MissionRole role, Vec3 pos, uint8_t variant, const Snapshot& snap, MissionHost& host);
    void spawnOverwatchWave(int difficulty);
    void spawnSectorWave(int sector, int difficulty);
    void spawnExtractionWave(const Snapshot& snap, MissionHost& host);
    bool spawnReinforcement(const Snapshot& snap, MissionHost& host);
    void saveCheckpoint(int stage);
    void enterOverwatch(Snapshot& snap, MissionHost& host, bool urgent);
    void enterIncoming(MissionHost& host);
    bool vaultWall(Agent& a, double dt, MissionHost& host);
    void roar(Vec3 at, MissionHost& host);
    void tacticalMove(Agent& a, int slot, const Snapshot& snap, double dt);
    int pickPosition(const Agent& a, int slot, const Snapshot& snap, bool hide) const;
    void beginExfil(MissionHost& host);
    void writeEpilogue();
    bool survivorAlive(int sector) const;
    void updateExfil(Snapshot& snap, const Input& input, double dt, MissionHost& host);
    void enterExtraction(Snapshot& snap, MissionHost& host);

    void updateIntro(Snapshot& snap, const Input& input, double dt, MissionHost& host);
    void updateOverwatch(Snapshot& snap, double dt, bool playerFired, MissionHost& host);
    void updateCommander(Snapshot& snap, double dt, MissionHost& host);
    void updateCivilian(int slot, Snapshot& snap, double dt);
    void updateAlien(int slot, Snapshot& snap, double dt, bool playerFired, MissionHost& host);
    void assignAttackTokens(const Snapshot& snap);
    void poseAlien(Agent& a) const;
    void poseCivilian(Agent& c) const;
    void hurtPlayer(float amount, Vec3 from, const Snapshot& snap, MissionHost& host);
    void emitScannerBeam(const Agent& a);
    void steerAlongPath(Agent& agent, Vec3 target, float speed, double dt, bool flying);
    void damageAgent(int slot, float amount, bool headshot, bool fromPlayer, Snapshot& snap, MissionHost& host);
    int livingAliens(int sectorFilter = -2) const;
    void checkPhaseTransitions(Snapshot& snap, double dt, MissionHost& host);
    void updateWaypoint(const Snapshot& snap);

    const game_detail::World& world_;
    game_detail::Navigation& nav_;
    game_detail::Random rng_;
    MissionSnapshot state_{};
    std::shared_ptr<MissionSnapshot> published_;
    std::array<Agent, MissionSnapshot::maxActors> agents_{};
    std::array<SectorState, 3> sectors_{};
    std::array<RadioLine, 4> radioQueue_{};
    int radioQueued_ = 0;
    double radioShownAt_ = 0;
    uint8_t radioPriority_ = RadioChatter;
    int difficulty_ = defaultDifficulty;
    // Checkpoint progress survives start() so a reload can resume; a fresh start clears it.
    int checkpointStage_ = 0;
    int retries_ = 0;
    CheckpointStats checkpoint_{};
    int introCue_ = 0;
    bool introSkipped_ = false;
    bool wakeSounds_ = false;
    bool overwatchHintSent_ = false;
    bool overwatchNudgeSent_ = false;
    bool shedSniperHintSent_ = false;
    bool regroupBriefSent_ = false;
    bool victoryAnnounced_ = false;
    bool incomingArrived_ = false;
    bool lzCallSent_ = false;
    int pendingFlankers_ = 0;
    bool flankersCalled_ = false;
    bool wallCalled_ = false;
    bool overchargeWarned_ = false;
    bool droneLinkWarned_ = false;
    bool interruptPraised_ = false;
    bool mistRemarked_ = false;
    double resupplyReadyAt_ = 0;
    double roarReadyAt_ = 0; // Roars are spaced so a pack never drowns out everything else
    bool plannedThisTick_ = false; // Spread position searches across ticks
    bool exfil_ = false;
    int exfilCue_ = 0;
    double exfilTime_ = 0;
    double liftAt_ = 0;
    int evacBeat_ = 0;
    int reinforcements_ = 0;
    int streetPatrols_ = 0;
    double nextPatrolAt_ = 0;
    double introTime_ = 0;
    double phaseTime_ = 0;
    double elapsed_ = 0;
    double heartbeatAt_ = 0;
    double failDelay_ = 0;
    double incomingArrivedAt_ = 0;
    double evacTimer_ = 0;
    double evacDuration_ = 1;
    double nextReinforceAt_ = 0;
    double graceUntil_ = 0;
    double graceReadyAt_ = 0;
    float baseWakeYaw_ = pi;
};

} // namespace fod::mission_detail
