#include "trace.hpp"

#include "mission.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

namespace fod {

bool ReplayTrace::load(const char* path, std::string& error, ArenaExtent extent) {
    const std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path, "r"), &std::fclose);
    if (!file) { error = "TRACE NOT FOUND"; return false; }
    char line[512]{};
    if (!std::fgets(line, sizeof(line), file.get())) {
        error = "TRACE SCHEMA";
        return false;
    }
    line[std::strcspn(line, "\r\n")] = '\0';
    const bool freeForAll = std::strcmp(line, "FODTRACE 2") == 0;
    const bool campaign = std::strcmp(line, "FODTRACE 3") == 0;
    if (!campaign && !freeForAll && std::strcmp(line, "FODTRACE 1") != 0) {
        error = "TRACE SCHEMA";
        return false;
    }
    ReplayTrace next;
    unsigned seed = 0, flags = 0, weapon = 0, deathTick = 0;
    const auto inYard = [extent](Vec3 p) {
        return std::abs(p.x) <= extent.x + .4f && std::abs(p.z) <= extent.z + .4f && p.y >= 0 && p.y <= 20;
    };
    const auto badStart = [&] {
        error = "TRACE INITIAL STATE";
        return false;
    };
    if (!std::fgets(line, sizeof(line), file.get())) return badStart();
    if (campaign) {
        char start[24]{}, extra;
        if (std::sscanf(line, "S %u %d %23s %c", &seed, &next.difficulty_, start, &extra) != 3) return badStart();
        if (std::strcmp(start, "breakout") == 0) next.campaignStart_ = CampaignStart::Breakout;
        else if (std::strcmp(start, "landing-zone") == 0) next.campaignStart_ = CampaignStart::LandingZone;
        else if (std::strcmp(start, "timberline") == 0) next.campaignStart_ = CampaignStart::Timberline;
        else if (std::strcmp(start, "array") == 0) next.campaignStart_ = CampaignStart::Array;
        else if (std::strcmp(start, "array-finale") == 0) next.campaignStart_ = CampaignStart::ArrayFinale;
        else return badStart();
    } else {
        auto& f = next.fixture_;
        // Older traces end at the streak field. Check the end of either form.
        int end = 0;
        const int fields = std::sscanf(line, "S %u %d %u %f %f %f %f %f %f %f %f %f %u %d %d %d %d %n%u %n",
                                       &seed, &next.difficulty_, &flags, &f.player.x, &f.player.y, &f.player.z, &f.yaw,
                                       &f.pitch, &f.rival.x, &f.rival.y, &f.rival.z, &f.rivalYaw,
                                       &weapon, &f.clip, &f.playerScore, &f.rivalScore, &f.streak, &end, &deathTick, &end);
        if (fields < 17 || line[end] != '\0' ||
            weapon > 2 || flags > (freeForAll ? 7u : 15u) || !inYard(f.player) || !inYard(f.rival) ||
            !std::isfinite(f.yaw) || !std::isfinite(f.pitch) || !std::isfinite(f.rivalYaw) ||
            f.clip < -1 || f.clip > 30 || f.playerScore < 0 || f.playerScore > 9 ||
            f.rivalScore < 0 || f.rivalScore > 9 || f.streak < 0 || f.streak > 9) return badStart();
        f.killPlayerAtTick = deathTick;
        f.weapon = static_cast<Weapon>(weapon);
        f.freezeRival = flags & 1;
        f.airstrikeReady = flags & 2;
        f.apacheReady = flags & 4;
        next.opponent_ = freeForAll ? Opponent::FreeForAll : flags & 8 ? Opponent::Hanz : Opponent::IPwndUrMom;
    }
    if (next.difficulty_ < 1 || next.difficulty_ > 10) return badStart();
    next.seed_ = seed;

    size_t row = 2;
    if (freeForAll) {
        ++row;
        auto& fixture = next.fixture_;
        char extra;
        if (!std::fgets(line, sizeof(line), file.get()) ||
            std::sscanf(line, "B %f %f %f %f %d %c", &fixture.secondRival.x, &fixture.secondRival.y,
                        &fixture.secondRival.z, &fixture.secondRivalYaw, &fixture.secondRivalScore, &extra) != 5 ||
            !inYard(fixture.secondRival) || !std::isfinite(fixture.secondRivalYaw) ||
            fixture.secondRivalScore < 0 || fixture.secondRivalScore > 9) {
            error = "TRACE SECOND RIVAL";
            return false;
        }
    }
    const auto invalid = [&] {
        error = "TRACE RECORD " + std::to_string(row);
        return false;
    };
    while (std::fgets(line, sizeof(line), file.get())) {
        ++row;
        if (line[0] == '#') continue;
        if (line[0] == 'T') {
            Input input;
            unsigned held, pressed;
            char extra;
            if (next.inputs_.size() >= 12000 ||
                std::sscanf(line, "T %f %f %f %f %u %u %c", &input.moveX, &input.moveY, &input.lookX, &input.lookY,
                            &held, &pressed, &extra) != 6 ||
                !(std::abs(input.moveX) <= 1 && std::abs(input.moveY) <= 1 &&
                  std::abs(input.lookX) <= 2 * pi && std::abs(input.lookY) <= 2 * pi) ||
                ((held | pressed) & ~((1u << 16) - 1))) return invalid();
            input.held = held;
            input.pressed = pressed;
            next.inputs_.push_back(input);
        } else if ((campaign || freeForAll) && line[0] == 'W') {
            unsigned count;
            char extra;
            if (std::sscanf(line, "W %u %c", &count, &extra) != 1 || !count || count > 12000 - next.inputs_.size()) {
                return invalid();
            }
            next.inputs_.insert(next.inputs_.end(), count, Input{});
        } else if (!campaign && !freeForAll && line[0] == 'R') {
            Draw draw;
            unsigned tick = 0;
            char extra;
            if (next.draws_.size() >= 30000 ||
                std::sscanf(line, "R %u %39s %lf %c", &tick, draw.site.data(), &draw.value, &extra) != 3 ||
                tick > next.inputs_.size() || !(draw.value >= 0 && draw.value < 1) ||
                (!next.draws_.empty() && tick < next.draws_.back().tick)) return invalid();
            draw.tick = tick;
            next.draws_.push_back(draw);
        } else {
            return invalid();
        }
    }
    if (std::ferror(file.get()) || next.inputs_.empty() || deathTick > next.inputs_.size()) return invalid();
    *this = std::move(next);
    error.clear();
    return true;
}

bool ReplayTrace::supply(void* data, uint32_t tick, const char* site, double& value) {
    auto& self = *static_cast<ReplayTrace*>(data);
    while (self.firstDraw_ < self.draws_.size() && self.draws_[self.firstDraw_].tick < tick) ++self.firstDraw_;
    for (size_t i = self.firstDraw_; i < self.draws_.size() && self.draws_[i].tick == tick; ++i) {
        auto& d = self.draws_[i];
        if (!d.used && std::strcmp(d.site.data(), site) == 0) {
            d.used = true;
            value = d.value;
            return true;
        }
    }
    ++self.missingDraws_;
    return false;
}

void ReplayTrace::begin(App& app) {
    at_ = firstDraw_ = missingDraws_ = 0;
    for (auto& draw : draws_) draw.used = false;
    app.game.setRandomHook(draws_.empty() ? nullptr : &supply, this);
    if (campaignStart_ == CampaignStart::Breakout) app.game.startMission(seed_, difficulty_);
    else if (campaignStart_ == CampaignStart::LandingZone) app.game.startExtractionReplay(seed_, difficulty_);
    else if (campaignStart_ == CampaignStart::Timberline) app.game.startChapterReplay(seed_, difficulty_, MissionChapter::Timberline);
    else if (campaignStart_ == CampaignStart::Array) app.game.startChapterReplay(seed_, difficulty_, MissionChapter::Array);
    else if (campaignStart_ == CampaignStart::ArrayFinale) app.game.startFinaleReplay(seed_, difficulty_);
    else {
        app.game.start(seed_, difficulty_, opponent_);
        app.game.setupFixture(fixture_);
    }
    app.audio.setVolume(app.settings().volume);
    app.audio.stopSpeech();
    app.audio.pauseSpeech(false);
    app.routeAudio();
}

bool ReplayTrace::step(App& app) {
    if (at_ >= inputs_.size()) return false;
    app.game.step(inputs_[at_++]);
    app.routeAudio();
    return true;
}

size_t ReplayTrace::unusedDraws() const {
    return size_t(std::count_if(draws_.begin(), draws_.end(), [](const Draw& draw) { return !draw.used; }));
}

void writeTraceTick(FILE* out, const App& app) {
    const auto& s = app.state();
    std::fprintf(out,
                 "{\"tick\":%lu,\"real\":%.9f,\"scaled\":%.9f,\"scale\":%.9f,\"position\":[%.6f,%.6f,%.6f],"
                 "\"rival\":[%.6f,%.6f,%.6f],\"camera\":[%.6f,%.6f,%.6f],\"yaw\":%.7f,\"pitch\":%.7f,\"eye\":%.7f,\"hp\":%.5f,\"rivalHp\":%.5f,"
                 "\"gun\":%d,\"gunState\":%d,\"clip\":%d,\"reserve\":%d,\"score\":[%d,%d],\"dead\":%d,"
                 "\"glass\":\"%016llx\",\"device\":%d,\"apache\":%d,",
                 static_cast<unsigned long>(s.tick), s.realTime, s.scaledDelta, s.timeScale, s.player.position.x,
                 s.player.position.y, s.player.position.z, s.rival.position.x, s.rival.position.y, s.rival.position.z,
                 s.camera.position.x, s.camera.position.y, s.camera.position.z, s.playerYaw, s.playerPitch, s.eyeHeight, s.player.health, s.rival.health, int(s.weapon),
                 int(s.weaponState), s.clip, s.reserve, s.playerScore, s.rivalScore, s.dead,
                 static_cast<unsigned long long>(s.brokenGlass), int(s.device), s.apacheActive);
    if (s.freeForAll) {
        const auto& second = s.secondRival;
        std::fprintf(out, "\"secondRival\":[%.6f,%.6f,%.6f],\"secondRivalHp\":%.5f,\"secondRivalScore\":%d,",
                     second.position.x, second.position.y, second.position.z, second.health, s.secondRivalScore);
    }
    if (s.mission && s.mission->active) {
        const auto& m = *s.mission;
        const auto plasma = std::count_if(m.plasma.begin(), m.plasma.end(), [](const PlasmaCloud& p) { return p.radius > 0; });
        std::fprintf(out, "\"mission\":{\"phase\":%d,\"checkpoint\":%d,\"civilians\":[%d,%d,%d],"
                     "\"boarded\":%d,\"aliens\":%d,\"plasma\":%d,\"shock\":%.6f,\"hold\":%.6f,\"perimeter\":%.2f,"
                     "\"stage\":%d,\"lock\":%.4f,\"dish\":%d,\"beam\":%.3f},",
                     int(m.phase), int(m.checkpointStage), int(m.actors[1].civilian), int(m.actors[2].civilian),
                     int(m.actors[3].civilian), m.civiliansBoarded, m.aliensRemaining, int(plasma),
                     m.plasmaShock, m.evacSeconds, m.holdRadius, int(m.operation.stage), m.operation.lock, int(m.dishLink),
                     m.operation.dishDistance);
    }
    std::fputs("\"events\":[", out);
    bool comma = false;
    for (size_t i = 0; i < app.game.eventCount(); ++i) {
        const auto& e = app.game.events()[i];
        if (e.kind == EventKind::Sound) continue;
        std::fprintf(out, "%s[%d,%lu,%d", comma ? "," : "", int(e.kind), static_cast<unsigned long>(e.surface), e.value);
        if (s.freeForAll) std::fprintf(out, ",%d,%d", int(e.rival), int(e.attacker));
        std::fputc(']', out);
        comma = true;
    }
    std::fputs("]}\n", out);
}

} // namespace fod
