#pragma once

#include "campaign_dialogue.hpp"
#include "game_bot.hpp"
#include "mission.hpp"

#include <array>
#include <vector>

namespace fod::mission_detail {

struct MissionHost {
    virtual ~MissionHost() = default;
    virtual void playSound(Sound id, float volume, bool positional, Vec3 at) = 0;
    virtual void setCampaignVoice(CampaignVoiceCue cue) = 0;
    virtual void damagePlayer(float amount, Vec3 from, bool partner) = 0;
    virtual void hitWorld(const game_detail::ShotHit& hit, Vec3 point) = 0;
    virtual void updateActorFrame(Actor& actor) = 0;
    virtual void refillPlayer(bool newMatch) = 0;
    virtual void equipSniper() = 0;
    virtual void useMissionScene(ArenaScene) {}
    virtual void transferPlayer(Vec3 position, float yaw) = 0;
};

using Line = CampaignLine;

// Radio lines interrupt by priority. Urgent lines react to the moment; orders and story wait
// their turn. Chatter goes stale quickly and orders more slowly.
enum RadioPriority : uint8_t { RadioChatter = 0, RadioOrder = 1, RadioStory = 2, RadioUrgent = 3 };
enum class MissionStart : uint8_t { SingleMission, Campaign, NextChapter, Retry, ExtractionReplay, ArrayGroundReplay, ScoutReplay };

struct MissionPartner {
    Snapshot* view = nullptr;
    bool fired = false;
    bool stabilizing = false;
};

// Route from the live player's position, which can be flush against cover.
// The nav grid uses a wider margin when it bakes edges between its own nodes.
constexpr float operationPlayerClearance = .39f;
constexpr int arrayBreachSquad = 4;

class MissionController {
public:
    MissionController(const Assets& assets, const game_detail::World& world, game_detail::Navigation& nav);

    void start(uint32_t seed, int difficulty, MissionStart mode, Snapshot& snap,
               Vec3& playerPosOut, float& playerYawOut, MissionHost& host);
    void seedExtractionReplay();
    // Arrange the end of the previous mission so NextChapter starts this one with Nora alive.
    void seedChapterReplay(MissionChapter chapter, bool noraAlive = true, bool legacyArray = false);
    // Arrange a checkpoint at The Array's rear door, so a Retry starts with the beam already live.
    void seedFinaleReplay();
    void step(Snapshot& snap, const Input& input, double dt, bool playerFired, MissionHost& host,
              MissionPartner partner = {});

    // Resolve a player weapon shot against mission actors. Returns true if it struck a mission actor.
    bool resolvePlayerShot(Vec3 origin, Vec3 dir, float damage, float worldDist, Snapshot& snap,
                           MissionHost& host, Vec3& hitPointOut);

    // Resolve Apache or Airstrike damage against active aliens.
    int nearestAlienForApache(Vec3 gunPos) const;
    bool apacheShotAlien(int index, Vec3 gunPos, Vec3 dir, float worldDist, float damage,
                         Snapshot& snap, MissionHost& host, Vec3& hitPointOut);
    void explodeAliens(Vec3 center, float radius, Snapshot& snap, MissionHost& host);

    float stickGain(const Snapshot& snap, bool dronesOnly = false) const;
    bool active() const { return state_.active; }
    bool canRetryCheckpoint() const { return checkpoint_.stage != MissionCheckpoint::None; }
    void publish(std::shared_ptr<const MissionSnapshot>& target);

private:
    struct Agent {
        bool used = false;
        MissionRole role = MissionRole::AlienTrooper;
        uint8_t variant = 0;
        uint8_t tacRole = 0; // 0=patrol, 1=suppressor, 2=flanker, 3=vanguard, 4=support
        int8_t sector = -1; // 0: East Ridge, 1: North Valley, 2: West Timberline, -1: outside the rescue sweep
        Actor actor{};
        Vec3 velocity{};
        bool pursuer = false;
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
        double spawnFx = 0;   // Time until the drop beam shuts off
        int8_t carrier = -1;  // Briar House craft projecting this arrival
        double dodgeUntil = 0;// Commander keeps stepping out of the player's line of fire until then
        double flinchUntil = 0;// Civilian shrugs off further player rounds until then
        double leapAt = -1;    // Seconds into a vault over the front wall, or -1
        Vec3 leapFrom{}, leapTo{};
        // Ray gun animation, charge-up, guard and suppression state
        float gunYaw = 0;
        float gunPitch = 0;
        float gunCharge = 0;
        float gunRecoil = 0;
        double chargeTime = 0;
        double chargeDuration = 0;
        double overchargeCooldown = 0;
        double staggerUntil = 0;
        float suppression = 0;
        float guard = 0, guardCapacity = 0, guardLift = 0;
        double guardDownUntil = 0;
        bool overcharging = false;
        bool chargedShot = false;
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
        CivilianOutcome civilian = CivilianOutcome::Pinned;
        bool alerted = false;
        bool seesPlayer = false;
        bool targetsPartner = false;
        bool token = false;     // Holds one of the limited aimed-fire slots against the player
        bool following = false; // Commander is closing distance to the player
        bool boarded = false;   // Climbed aboard the evac bird
        bool operationJammer = false;
        bool farJammer = false;   // The Array's last jammer holds the dish from across the lawn
        bool rallying = false;    // Ben runs to the player before heading for the clearing
        bool running = false;     // A survivor crossing the clearing to Dustoff
        int8_t squad = -1;        // Drops that landed together alert together
        double cargoDiversionUntil = 0;
        double holdUntil = 0;     // Walt waits for a scanner to turn away
        double huntAt = 0;        // An unaware drop goes looking for the transmitter after this
        uint8_t deathVariant = 0;
#ifndef ESP_PLATFORM
        NpcMotion motion{};
        Vec3 animationPosition{};
        float animationYaw = 0, aimPitch = 0;
        bool shouldAim = false;
#endif

        bool isDead() const { return role == MissionRole::Civilian ? civilian == CivilianOutcome::Lost : dead; }
    };

    struct SectorState {
        bool spawned = false;
        bool resolved = false;
        bool warned = false; // Delta-1 has called out fire on this sector's civilian
        uint8_t coverLeg = 0;
        float held = 0;      // Seconds Walt has waited at this cover point
        int civSlot = 1;
        Vec3 center{};
    };

    struct RadioLine {
        CampaignVoiceCue cue;
        float duration = 0;
        uint8_t priority = RadioOrder;
        int8_t sector = -1; // Sector-specific lines are dropped once that sector resolves
        double queuedAt = 0;
    };

    // Progress carried across a checkpoint reload so results reflect the whole run.
    struct CheckpointStats {
        struct Civilian {
            CivilianOutcome outcome = CivilianOutcome::Pinned;
            Vec3 position{};
            float health = 0;
            uint8_t coverLeg = 0;
        };
        MissionCheckpoint stage = MissionCheckpoint::None;
        int kills = 0, criticalKills = 0;
        int interrupts = 0, shieldsBroken = 0, ambushKills = 0;
        float seconds = 0;
        std::array<Civilian, 3> civilians{};
        bool receiverDown = false;
        uint8_t retreatLeg = 0;
        Vec3 operationSpawn{};
        OperationStage operationStage = OperationStage::Travel;
        uint8_t calls = 0;
        Vec3 firstMark{};
        bool clearingBurned = false;
        bool scoutPort = false;
        bool arrayRepeater = false;
    };

    // Timberline and The Array. The view in state_.operation carries what the HUD and renderer need.
    struct OperationProgress {
        Vec3 shipVelocity{}, cruiserShift{};
        float shipHeight = 30.f;
        double settled = 0;        // Seconds the ship has hovered over its target
        double stageTime = 0;
        double pickupTime = 0;
        double mothershipTime = 0;
        double spotterReadyAt = 0;
        double nextDropAt = 0;
        double nextJammerAt = 0;
        double lineAt = 0;
        int drops = 0;             // Reinforcements dropped this stage
        int cue = 0;               // Story beats delivered this stage
        int pursuersSent = 0;
        int lastPursuitDrop = -1;
        Vec3 pursuitTrail{};
        bool jammerSent = false;   // Timberline: the hijack drone for the third call
        bool harasserSent = false; // Timberline: the drone that harries the fourth call
        bool hijacked = false;     // The drop came down on the emitter instead of the marked spot
        bool finalJammer = false;  // The Array: the last jammer has appeared
        bool warnedDish = false;
        bool reuniteCalled = false;
        Vec3 dropPoint{}, firstMark{};
        // A drop lands what fits in the alien slots; the rest follow as bodies clear.
        int squadCall = 0, squadNext = 0;
        bool squadAware = false;
    };

    Vec3 gunGrip(const Agent& a) const;
    Vec3 rayGunMuzzle(const Agent& a) const;
    void launchPlasma(Vec3 origin, Vec3 dir, bool charged, float damage, int ally, float range,
                      const Snapshot& snap, float barrel = 0.f);
    void updatePlasma(Snapshot& snap, const Snapshot* partner, float dt, MissionHost& host);
    void resetAll();
    void setRadio(Line line, float duration, MissionHost& host, bool chirp = true,
                  uint8_t priority = RadioOrder, int sector = -1);
    void setSignalHint(const char* hint);
    void clearRadioQueue() { radioQueued_ = 0; }
    void discardQueuedRadio(Line line);
    bool radioSpeaking() const {
        return state_.radioTimer > 0 && elapsed_ - radioShownAt_ < radioReadSeconds_;
    }
    void dismissRadio(MissionHost& host);
    void showRadio(const RadioLine& line, MissionHost& host);
    void pumpRadio(MissionHost& host);
    void setObjective(const char* mainText, const char* subText);
    void setTacticalCallout(const char* text, uint16_t color, float duration = 1.6f);
    void addTracer(Vec3 from, Vec3 to, uint16_t color, TracerStyle style = TracerStyle::Ballistic, float life = 1.f);
    bool guardActive(const Agent& a) const;
    float guardShot(const Agent& a, Vec3 origin, Vec3 direction) const;
    float droneShot(const Agent& drone, Vec3 origin, Vec3 direction, float reach) const;
    void moveBriarFleet(double dt);
    void stageFleetDrop(Agent& a, MissionHost& host, int carrier = -1);
    void updateMistDensity(double dt);
    void spawnCommander(Vec3 pos, float yaw);
    void spawnCivilian(int slot, int sector, Vec3 pos, float yaw, uint8_t outfit);
    struct AlienGroup {
        int lead = -1, escort = -1;
        explicit operator bool() const { return lead >= 0; }
    };
    template <typename Configure>
    void configureAliens(AlienGroup group, const Configure& configure) {
        if (group) configure(agents_[group.lead]);
        if (group.escort >= 0) configure(agents_[group.escort]);
    }
    int aliensPerSpawn() const {
#ifdef FOD_LOCAL_DUEL
        if (coOp_) return 2;
#endif
        return 1;
    }
    size_t alienSlotLimit() const { return MissionSnapshot::firstAlien + MissionSnapshot::soloAlienSlots * aliensPerSpawn(); }
    size_t freeAlienSlots() const;
    Vec3 escortPosition(Vec3 pos, float yaw, MissionRole role) const;
    int spawnSingleAlien(MissionRole role, int sector, Vec3 pos, float yaw, uint8_t variant, int difficulty);
    AlienGroup spawnAlien(MissionRole role, int sector, Vec3 pos, float yaw, uint8_t variant, int difficulty);
    AlienGroup dropAlien(MissionRole role, Vec3 pos, uint8_t variant, const Snapshot& snap, MissionHost& host, int carrier = -1);
    void spawnOverwatchWave(int difficulty);
    void spawnSectorWave(int sector, MissionHost& host);
    void spawnExtractionWave(const Snapshot& snap, MissionHost& host);
    bool spawnReinforcement(const Snapshot& snap, MissionHost& host);
    void saveCheckpoint(MissionCheckpoint stage);
    void restoreCheckpointStats();
    void enterOverwatch(Snapshot& snap, MissionHost& host, bool urgent);
    float scannerYaw(const Agent& a) const;
    void enterUplink(MissionHost& host);
    void enterIncoming(MissionHost& host);
    bool vaultWall(Agent& a, double dt, MissionHost& host);
    void roar(Vec3 at, MissionHost& host);
    void tacticalMove(Agent& a, int slot, const Snapshot& snap, double dt);
    int pickPosition(const Agent& a, int slot, const Snapshot& snap, bool hide) const;
    void finishBoarding();
    void refreshCivilianTotals();
    void writeEpilogue();
    bool survivorAlive(int sector) const;
    void updateExfil(Snapshot& snap, const Input& input, double dt, MissionHost& host);
    void enterExtraction(Snapshot& snap, MissionHost& host);

    void updateIntro(Snapshot& snap, const Input& input, double dt, MissionHost& host);
    void updateOverwatch(Snapshot& snap, double dt, bool playerFired, MissionHost& host);
    void updateCommander(Snapshot& snap, double dt, MissionHost& host);
    void updateCivilian(int slot, Snapshot& snap, double dt, MissionHost& host);
    void updateAlien(int slot, const Snapshot& snap, double dt, bool playerFired, MissionHost& host);
    void assignAttackTokens(const Snapshot& snap, const Snapshot* partner);
    void poseAlien(Agent& a) const;
    void poseCivilian(Agent& c) const;
    void hurtPlayer(float amount, Vec3 from, const Snapshot& snap, MissionHost& host, bool partner);
    void emitScannerBeam(const Agent& a);
    void steerAlongPath(Agent& agent, Vec3 target, float speed, double dt, bool flying);
    void damageAgent(int slot, float amount, bool critical, bool fromPlayer, Snapshot& snap, MissionHost& host,
                     bool guardHit = false);
    int livingAliens(int sectorFilter = -2) const;
    bool districtAlerted(int sector) const;
    int nextSector() const;
    void updateRescue(Snapshot& snap, MissionHost& host);
    void checkPhaseTransitions(Snapshot& snap, double dt, MissionHost& host);
    void updateWaypoint(const Snapshot& snap);
    // Mission 1: the dish pulse on North Avenue and Dustoff in the clearing.
    void updateDishPulse(double dt, MissionHost& host);
    bool signalThreat(const Agent& civ, Vec3 destination, int& shooter) const;
    Line survivorLine(int sector) const;
    void updateBenSignal(const Snapshot& snap, MissionHost& host);
    void beginDustoff(MissionHost& host);
    void updateBoarding(Snapshot& snap, double dt, MissionHost& host);
    void beginLift(const Snapshot& snap, MissionHost& host);
    // Timberline and The Array.
    void startOperation(Snapshot& snap, Vec3& position, float& yaw, MissionHost& host, bool retry);
    void startScoutOperation(Snapshot& snap, Vec3& position, float& yaw, MissionHost& host, bool retry);
    void enterScoutStage(OperationStage stage, MissionHost& host);
    void stepScout(Snapshot& snap, const Input& input, double dt, MissionHost& host, bool stabilizing);
    void stepSimulation(Snapshot& snap, const Input& input, double dt, bool playerFired,
                        MissionHost& host, MissionPartner partner);
#ifndef ESP_PLATFORM
    void updateNpcMotion(const Snapshot& snap, float dt);
#endif
    void publishScoutFleet();
    bool routeScoutCargo(const Snapshot& snap, Vec3 chute, MissionHost& host);
    void saveOperationCheckpoint(Vec3 position);
    void enterStage(OperationStage stage, MissionHost& host);
    void stepOperation(Snapshot& snap, double dt, MissionHost& host);
    void stepTimberline(Snapshot& snap, double dt, MissionHost& host);
    void updateTimberlineFlight(Snapshot& snap, const Input& input, double dt, MissionHost& host);
    void stepArray(Snapshot& snap, double dt, MissionHost& host);
    void calibrateArray(Snapshot& snap, double dt, MissionHost& host, bool noraHere);
    bool arrayCounterattack(Snapshot& snap, MissionHost& host, bool inSaddle);
    const char* timberLandingIssue(Vec3 aim, Vec3 player, bool noraHere) const;
    void updatePursuit(Snapshot& snap, MissionHost& host);
    void stepTimberlineRetreat(Snapshot& snap, MissionHost& host);
    void stepTimberlinePickup(Snapshot& snap, MissionHost& host);
    bool pointerGround(const Snapshot& snap, float range, bool generous, Vec3& out) const;
    void moveShip(Vec3 target, float speed, double dt);
    void publishFleet();
    int dropSquad(const Snapshot& snap, MissionHost& host);
    AlienGroup dropNear(Vec3 center, float nearRadius, float farRadius, MissionRole role, uint8_t variant, float height,
                        bool outdoors, const Snapshot& snap, MissionHost& host);
    int updateJammers(Vec3 center, const Snapshot& snap, MissionHost& host);
    void recallAliens();
    void failMission(const char* reason, MissionHost& host);
    void updateReunite(Snapshot& snap, double dt, MissionHost& host);
    void writeOperationEpilogue();

    struct PlasmaFlight {
        game_detail::ShotHit obstruction{};
        float remaining = 0, damage = 0;
        int ally = -1;
    };

    const Assets& assets_;
    const game_detail::World& world_;
    game_detail::Navigation& nav_;
    game_detail::Random rng_;
    MissionSnapshot state_{};
    std::array<PlasmaFlight, MissionSnapshot::maxPlasma> plasmaFlights_{};
    std::shared_ptr<MissionSnapshot> published_;
    std::array<Agent, MissionSnapshot::maxActors> agents_{};
#ifdef FOD_LOCAL_DUEL
    bool coOp_ = false;
#endif
    std::array<SectorState, 3> sectors_{};
    std::array<RadioLine, 4> radioQueue_{};
    int radioQueued_ = 0;
    Line radioLine_ = Line::None;
    double radioReadSeconds_ = 0;
    double radioShownAt_ = 0;
    uint8_t radioPriority_ = RadioChatter;
    int difficulty_ = defaultDifficulty;
    // Checkpoint progress survives start() so a reload can resume; a fresh start clears it.
    int retries_ = 0;
    CheckpointStats checkpoint_{};
    OperationProgress operation_{};
    struct ScoutProgress {
        Vec3 cargoFrom{}, cargoTo{};
        double cargoAt = 0, diversionUntil = 0, progress = 0;
        double landingAt = -1;
        bool port = false;
        Vec3 flightFrom{}, flightPosition{}, chaseHeading{};
        float flightLateral = 0.f;
    } scout_{};
    bool legacyArray_ = false;
    bool legacyScout_ = false;
    // Nora stayed behind at the end of Briar House. Later missions hear her voice instead of Delta-1's.
    bool noraAlive_ = true;
    bool signalEdge_ = false, signalHeld_ = false;
    double signalReadyAt_ = 0;
    double dishPulseAt_ = 0;
    bool dishTipSent_ = false;
    bool boardingCalled_ = false;
    double boardingCalledAt_ = 0;
    int boardingCue_ = 0;
    // Set by the later missions: where Delta-1 posts himself, where guidance points, where jammers go.
    bool commanderHolds_ = false;
    Vec3 commanderPost_{};
    bool operationHasGoal_ = false, operationGoalDirect_ = false;
    Vec3 operationGoal_{};
    Vec3 jammerPost_{};
    int introCue_ = 0;
    bool introSkipped_ = false;
    bool wakeSounds_ = false;
    bool overwatchNudgeSent_ = false;
    bool lzEnforcerLanded_ = false;
    int pendingFlankers_ = 0;
    bool flankersCalled_ = false;
    bool wallCalled_ = false;
    bool overchargeWarned_ = false;
    bool guardHintShown_ = false;
    bool interruptPraised_ = false;
    bool mistRemarked_ = false;
    double resupplyReadyAt_ = 0;
    double roarReadyAt_ = 0; // Roars are spaced so a pack never drowns out everything else
    bool plannedThisTick_ = false; // Spread position searches across ticks
    bool exfil_ = false;
    int exfilCue_ = 0;
    double exfilTime_ = 0;
    int evacBeat_ = 0;
    int reinforcements_ = 0;
    int streetPatrols_ = 0;
    int rescueObjective_ = -1;
    int activeSector_ = -1;
    double routeReadyAt_ = 0;
    Vec3 routeFrom_{}, routeGoal_{}, routeWaypoint_{};
    std::vector<int> route_;
    double nextPatrolAt_ = 0;
    double introTime_ = 0;
    double phaseTime_ = 0;
    double elapsed_ = 0;
    double heartbeatAt_ = 0;
    double failDelay_ = 0;
    double evacTimer_ = 0;
    double evacDuration_ = 1;
    double nextReinforceAt_ = 0;
    Camera liftCamera_{};  // Where the player stood as Dustoff lifted
    std::array<double, 2> graceUntil_{}, graceReadyAt_{};
    float baseWakeYaw_ = pi;
};

} // namespace fod::mission_detail
