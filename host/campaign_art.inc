// Desktop campaign meshes share the compact animation and collision coordinates.
struct CharacterDesign {
    uint16_t skin, shadow, hair, jacket, seam, pants, gear, accent;
    float height, build, stride, jaw, nose;
};

constexpr CharacterDesign campaignDesigns[]{
    // Ben: chambray work shirt and canvas vest.
    {rgb(223, 172, 136), rgb(179, 118, 91), rgb(62, 39, 26), rgb(73, 118, 142),
     rgb(38, 67, 79), rgb(72, 69, 63), rgb(124, 97, 58), rgb(220, 172, 93), .99f, .96f, 1.05f, .97f, .107f},
    // Nora: burgundy field jacket, pale jersey, glasses and tied hair.
    {rgb(226, 171, 142), rgb(179, 112, 91), rgb(66, 36, 26), rgb(133, 53, 71),
     rgb(83, 35, 52), rgb(55, 70, 88), rgb(199, 185, 149), rgb(225, 162, 94), .95f, .93f, .93f, .77f, .106f},
    // Walt: waxed work coat, tobacco corduroys and a gray mustache.
    {rgb(218, 163, 124), rgb(168, 106, 79), rgb(145, 141, 124), rgb(156, 122, 70),
     rgb(98, 71, 45), rgb(71, 66, 50), rgb(71, 82, 62), rgb(227, 184, 106), .94f, 1.10f, .78f, 1.03f, .124f},
    // Delta-1 and the two co-op operatives have distinct uniforms and headgear.
    {rgb(165, 118, 86), rgb(115, 70, 54), rgb(56, 48, 42), rgb(98, 111, 77),
     rgb(63, 74, 54), rgb(67, 77, 64), rgb(62, 69, 58), rgb(219, 152, 67), 1.02f, 1.10f, .88f, 1.07f, .116f},
    {rgb(219, 165, 129), rgb(162, 105, 81), rgb(71, 51, 38), rgb(147, 128, 88),
     rgb(93, 82, 59), rgb(102, 106, 82), rgb(105, 100, 70), rgb(235, 186, 100), 1.02f, 1.07f, 1.04f, 1.05f, .112f},
    {rgb(156, 110, 87), rgb(115, 74, 60), rgb(48, 37, 31), rgb(65, 89, 106),
     rgb(43, 61, 72), rgb(63, 72, 77), rgb(65, 76, 76), rgb(159, 201, 210), .98f, 1.01f, 1.03f, .89f, .111f}
};
static_assert(std::size(campaignDesigns) == size_t(CampaignHuman::Count));
static_assert(mission_detail::ben == int(CampaignHuman::Ben) && mission_detail::nora == int(CampaignHuman::Nora) &&
              mission_detail::walt == int(CampaignHuman::Walt));

uint16_t campaignMaterial(Assets& assets, uint16_t color, bool emissive = false) {
    const auto id = uint16_t(assets.materials.size());
    assets.materials.push_back({color, -1, 255, false, emissive});
    return id;
}

uint16_t campaignFabric(Assets& assets, uint16_t color) {
    constexpr int size = 64;
    Texture texture{size, size, {}};
    texture.pixels.reserve(size * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int thread = x % 4 == 0 ? 4 : y % 4 == 2 ? -4 : 0;
            const int fade = int(grain(x / 4, y / 3)) / 3 + tileNoise(x, y, size, 8) / 2 - 6;
            const int shade = thread + fade;
            texture.pixels.push_back(duskTint(color, shade, shade, shade));
        }
    }
    const auto id = campaignMaterial(assets, color);
    assets.materials[id].texture = int16_t(assets.textures.size());
    assets.textures.push_back(std::move(texture));
    return id;
}

struct CharacterMaterials {
    uint16_t skin, shadow, hair, jacket, seam, pants, gear, accent;
    uint16_t boot, eye, iris, metal;
};

void writeCampaignHead(CreatureWriter& w, const HumanPose& pose, CampaignHuman person,
                       const CharacterDesign& design, const CharacterMaterials& m) {
    const Vec3 up = pose.headUp, forwardDir = normalized(cross(Vec3{1, 0, 0}, up));
    const Vec3 side = cross(up, forwardDir);
    const Vec3 head = pose.head;
    const auto at = [&](float x, float y, float z) { return head + side * x + up * y + forwardDir * z; };
    const bool nora = person == CampaignHuman::Nora;
    const bool walt = person == CampaignHuman::Walt;
    const bool delta = person == CampaignHuman::Delta;
    const bool lead = person == CampaignHuman::Lead;
    const bool partner = person == CampaignHuman::Partner;

    // The jaw and cheekbones carry most of the likeness. Ease the planes around the skull.
    struct FaceRing { float y, width, depth, center; };
    const FaceRing rings[]{
        {-.119f, .046f * design.jaw, .049f, .020f}, {-.095f, .062f * design.jaw, .068f, .015f},
        {-.056f, .080f, .075f, .005f}, {-.018f, .089f, .084f, 0}, {.018f, .087f, .083f, -.004f},
        {.054f, .085f, .089f, -.007f}, {.100f, .070f, .078f, -.013f}, {.128f, .030f, .037f, -.015f}
    };
    constexpr int sides = 16;
    const auto ring = [&](const FaceRing& r, int i) {
        const float angle = 2.f * pi * (i % sides) / sides;
        return at(r.width * std::sin(angle), r.y, r.center + r.depth * std::cos(angle));
    };
    ++w.part;
    for (int i = 0; i < sides; ++i) {
        for (size_t r = 1; r < std::size(rings); ++r) {
            const auto& lo = rings[r - 1];
            const auto& hi = rings[r];
            w.quad(ring(lo, i), ring(lo, i + 1), ring(hi, i + 1), ring(hi, i), head,
                   r == 1 ? m.shadow : m.skin);
        }
        w.tri(ring(rings[0], i), ring(rings[0], i + 1), at(0, -.122f, .018f), head, m.shadow);
        w.tri(ring(rings[7], i), ring(rings[7], i + 1), at(0, .137f, -.015f), head, m.skin);
    }

    // Nose bridge and nostrils sit between the eye sockets and the upper lip.
    const Vec3 tip = at(0, -.039f, design.nose);
    const Vec3 bridge = at(0, .028f, .087f);
    const Vec3 septum = at(0, -.055f, .086f);
    ++w.part;
    for (const float s : {-1.f, 1.f}) {
        const Vec3 wing = at(s * (walt ? .020f : .015f), -.045f, .092f);
        const Vec3 sideWall = at(s * .020f, -.013f, .081f);
        w.quad(bridge, sideWall, wing, tip, head, m.skin);
        w.tri(wing, septum, tip, head, m.shadow);
        const auto socket = [&](float x, float y, float z) { return at(s * x, y, z); };
        const Vec3 inner = socket(.024f, .010f, .087f), outer = socket(.067f, .014f, .064f);
        const Vec3 lower = socket(.043f, .002f, .079f), upper = socket(.043f, .019f, .080f);
        w.quad(inner, lower, outer, upper, head, m.eye);
        w.quad(socket(.037f, .010f, .083f), socket(.043f, .003f, .082f),
               socket(.050f, .012f, .080f), socket(.043f, .018f, .083f), head, m.iris);
        w.quad(socket(.022f, .026f, .087f), socket(.042f, .034f, .085f),
               socket(.074f, .031f, .065f), socket(.045f, .027f, .084f), head, m.hair);
        w.tube({at(s * .081f, -.040f, -.003f), at(s * .096f, -.017f, -.008f), at(s * .093f, .014f, -.009f)},
               {.011f, .016f, .012f}, {.013f, .012f, .012f}, 8, side, m.skin);
        if (nora) {
            // Small, thin spectacle rims, following the curve of the cheeks.
            const Vec3 a = socket(.017f, .030f, .102f), b = socket(.066f, .032f, .081f);
            const Vec3 c = socket(.063f, -.002f, .083f), d = socket(.022f, -.003f, .100f);
            for (const auto& edge : {std::array{a,b}, std::array{b,c}, std::array{c,d}, std::array{d,a}})
                w.tube({edge[0], edge[1]}, {.0034f, .0034f}, {.0034f, .0034f}, 4, up, m.iris);
            w.tube({b, at(s * .097f, .023f, -.010f)}, {.0031f, .0031f}, {.0031f, .0031f}, 4, up, m.iris);
        }
    }
    w.quad(at(-.027f, -.067f, .083f), at(0, -.072f, .092f), at(.027f, -.067f, .083f),
           at(0, -.063f, .094f), head, m.shadow);
    if (nora) {
        w.tube({at(-.017f, .028f, .102f), at(0, .032f, .107f), at(.017f, .028f, .102f)},
               {.003f, .003f, .003f}, {.003f, .003f, .003f}, 4, up, m.iris);
    }
    if (walt) {
        w.quad(at(-.032f, -.059f, .088f), at(-.015f, -.050f, .097f), at(.015f, -.050f, .097f),
               at(.032f, -.059f, .088f), head, m.hair);
    }

    // A separately shaped cap or hair silhouette identifies each character from behind.
    if (delta || lead) {
        w.tube({at(0, .031f, -.018f), at(0, .094f, -.020f), at(0, .143f, -.019f), at(0, .164f, -.019f)},
               {.092f, .113f, .091f, .042f}, {.101f, .121f, .090f, .056f}, 12, side, m.gear, false);
        w.tube({at(0, .055f, -.015f), at(0, .072f, -.019f)}, {.105f, .109f}, {.120f, .122f}, 12, side, m.seam, false);
        for (const float s : {-1.f, 1.f}) {
            w.tube({at(s * .088f, .034f, -.032f), at(s * .112f, .026f, -.033f)},
                   {.038f, .032f}, {.035f, .029f}, 8, up, m.boot);
            w.tube({at(s * .098f, .020f, -.017f), at(s * .076f, -.074f, .046f)},
                   {.011f, .009f}, {.010f, .009f}, 5, up, m.gear);
        }
        if (delta) {
            w.tube({at(-.11f, .023f, -.006f), at(-.100f, -.034f, .064f), at(-.049f, -.051f, .100f)},
                   {.007f, .006f, .012f}, {.007f, .006f, .012f}, 6, up, m.boot);
        }
    } else if (walt || partner) {
        const float brim = walt ? .13f : .155f;
        w.tube({at(0, .052f, -.009f), at(0, .090f, -.014f), at(0, .134f, -.018f), at(0, .145f, -.012f)},
               {.098f, .108f, .080f, .029f}, {.095f, .109f, .078f, .042f}, 12, side, m.gear, false);
        w.quad(at(-.077f, .063f, .063f), at(.077f, .063f, .063f), at(.064f, .053f, brim),
               at(-.064f, .053f, brim), at(0, .030f, .105f), m.seam);
        w.quad(at(-.066f, .051f, brim), at(.066f, .051f, brim), at(.064f, .057f, brim),
               at(-.064f, .057f, brim), head, m.gear);
    } else {
        // The asymmetric hairline sits lower at the temples and over the nape.
        ++w.part;
        const auto strand = [&](int i, float radius, float y) {
            const float a = 2.f * pi * float(i % sides) / sides;
            const float front = std::cos(a);
            return at(std::sin(a) * radius, y + .062f * front + .013f * std::sin(a + .55f),
                      -.015f + front * radius * 1.04f);
        };
        for (int i = 0; i < sides; ++i) {
            const Vec3 a = strand(i, .093f, -.007f), b = strand(i + 1, .093f, -.007f);
            const Vec3 c = strand(i + 1, .084f, .076f), d = strand(i, .084f, .076f);
            w.quad(a, b, c, d, head, m.hair);
            w.tri(d, c, at(.012f, nora ? .149f : .143f, -.021f), head, m.hair);
        }
        if (nora) {
            w.tube({at(.024f, .035f, -.089f), at(.048f, -.045f, -.125f), at(.065f, -.135f, -.133f),
                    at(.074f, -.187f, -.090f)}, {.049f, .042f, .032f, 0}, {.035f, .033f, .023f, 0}, 9, side, m.hair);
            w.tube({at(.036f, .022f, -.103f), at(.043f, .009f, -.108f)},
                   {.051f, .049f}, {.039f, .038f}, 8, side, m.seam, false);
        }
    }
}

struct CampaignRig {
    HumanPose joints{};
    std::array<float, 2> bootRoll{};
    float twist = 0;
};

Vec3 campaignKnee(Vec3 hip, Vec3 ankle, float side, float crouch) {
    constexpr float thigh = .456f, shin = .432f;
    const Vec3 segment = ankle - hip;
    const float distance = std::clamp(length(segment), .06f, thigh + shin - .001f);
    const Vec3 line = normalized(segment);
    const Vec3 forwardDir{side * crouch * .17f, 0, 1};
    const Vec3 bend = normalized(forwardDir - line * dot(forwardDir, line));
    const float a = (thigh * thigh + distance * distance - shin * shin) / (2.f * distance);
    return hip + line * a + bend * std::sqrt(std::max(0.f, thigh * thigh - a * a));
}

CampaignRig campaignRig(unsigned slot, CampaignHuman person, const CharacterDesign& design) {
    CampaignRig result;
    auto& [limbs, pelvis, waist, chest, shoulders, neck, head, headUp] = result.joints;
    const bool military = person >= CampaignHuman::Delta;
    const bool walking = slot >= campaignWalk && slot < campaignRun;
    const bool running = slot >= campaignRun && slot < campaignCover;
    const bool moving = walking || running;
    const bool cover = !military && slot >= campaignCover && slot < campaignFlinch;
    const bool flinch = !military && slot == campaignFlinch;
    const bool aim = person == CampaignHuman::Delta && slot == campaignAim;
    const float phase = moving ? float(slot - (running ? campaignRun : campaignWalk)) / campaignGaitPoses
                               : slot < campaignWalk ? float(slot) / campaignIdlePoses : .25f;
    const float cycle = phase * 2.f * pi;
    const float swing = std::sin(cycle), load = std::cos(cycle);
    const float breath = moving ? 0.f : (.5f - .5f * load);
    const float forwardLean = running ? .185f : walking ? .075f : aim ? .055f : cover ? .23f : flinch ? .071f : 0.f;
    const float weight = moving ? .025f * swing : cover ? .045f : .035f * swing;
    const float peek = cover ? .115f * float(slot - campaignCover) : 0.f;
    const float leadFoot = person == CampaignHuman::Partner ? -.094f : military ? .102f : .055f;
    const float stride = moving ? (running ? 1.f : 0.f) : 0.f;
    float hipY = cover ? .405f + peek * .23f : flinch ? .775f : running ? .84f : walking ? .878f : .914f;
    hipY += moving ? .024f * std::cos(cycle * 2.f - .73f) : breath * .013f;
    pelvis = {weight, hipY, running ? .066f : cover ? -.13f : 0.f};

    for (size_t l = 0; l < limbs.size(); ++l) {
        const float side = l ? 1.f : -1.f;
        const auto foot = moving ? gaitFoot(phase + float(l) * .5f, stride) : FootCycle{side * leadFoot, 0.f, 0.f};
        const float roll = foot.roll;
        const float rock = std::sin(std::abs(roll)) * (roll < 0.f ? .174f : .073f);
        const float footX = side * (cover ? .162f : military ? .145f : .115f);
        const float footZ = cover ? side * .058f + .023f : foot.z;
        auto& leg = limbs[l];
        leg.ankle = {footX, .078f + foot.lift + rock, footZ};
        result.bootRoll[l] = roll;
        const Vec3 toe{0, std::sin(roll), std::cos(roll)};
        const Vec3 footUp{0, toe.z, -toe.y};
        leg.toe = leg.ankle + toe * .173f - footUp * .048f;
        // Limit pelvis height against the available leg length before solving either knee.
        const Vec3 horizontal{leg.ankle.x - pelvis.x - side * .108f, 0.f, leg.ankle.z - pelvis.z};
        const float ceiling = leg.ankle.y + std::sqrt(std::max(.20f, .881f * .881f - dot(horizontal, horizontal)));
        hipY = std::min(hipY, ceiling);
    }
    pelvis.y = hipY;
    waist = pelvis + Vec3{0, cover ? .144f : .164f, forwardLean * .26f};
    chest = pelvis + Vec3{-.37f * weight, cover ? .355f : .402f + .013f * breath, forwardLean * .66f};
    shoulders = chest + Vec3{-.28f * weight, cover ? .121f : .136f, forwardLean * .21f};
    neck = shoulders + Vec3{0, .108f, forwardLean * .18f};
    head = neck + Vec3{-.22f * weight, .115f + .006f * breath, forwardLean * .14f};
    headUp = normalized({moving ? swing * .040f : -.029f * breath, 1.f,
                         cover ? .77f - .35f * peek : flinch ? .26f : forwardLean * .35f});
    result.twist = moving ? .105f * swing * design.stride * (military ? .64f : 1.f)
                         : cover ? -.22f * peek : flinch ? .175f : .032f * swing;

    for (size_t l = 0; l < limbs.size(); ++l) {
        const float s = l ? 1.f : -1.f;
        auto& limb = limbs[l];
        limb.hip = pelvis + Vec3{s * .108f, -.017f, moving ? -.044f * swing * s : 0.f};
        limb.knee = campaignKnee(limb.hip, limb.ankle, s, cover ? 1.f : .24f);
        limb.shoulder = shoulders + Vec3{s * .206f, s * swing * (moving ? .014f : .008f), 0};
        if (cover) {
            limb.elbow = limb.shoulder + Vec3{s * (.145f - peek * .18f), .035f, .151f};
            limb.wrist = head + Vec3{s * (.106f + peek * .15f), .079f - peek * .30f, .050f + peek * .16f};
        } else if (flinch) {
            limb.elbow = limb.shoulder + Vec3{s * .104f, -.154f, .098f};
            limb.wrist = chest + Vec3{s * .088f, .167f - float(l) * .085f, .19f};
        } else if (military) {
            const float dip = running ? .070f : walking ? .021f : 0.f;
            const float bob = moving ? .021f * load : .011f * breath;
            limb.elbow = chest + Vec3{s * .236f, -.168f + (aim ? .062f : 0.f), l ? .021f : .109f};
            limb.wrist = chest + Vec3{l ? (aim ? .102f : .138f) : (aim ? .049f : -.031f),
                                      -.067f - dip + bob + (aim ? .167f : 0.f), l ? .237f : .448f};
        } else {
            const float pendulum = moving ? swing * s * design.stride : s * swing * .085f;
            const float armSwing = pendulum * (running ? .155f : .112f);
            limb.elbow = limb.shoulder + Vec3{s * .046f, running ? -.241f : -.274f, armSwing - .042f};
            const float rise = running ? .069f + pendulum * .083f : .017f * pendulum;
            limb.wrist = limb.elbow + Vec3{-s * .038f, -.220f + rise + (running ? .23f : 0.f),
                                            .093f + armSwing * .69f + (running ? .145f : 0.f)};
        }
        limb.hand = limb.wrist + normalized(limb.wrist - limb.elbow) * (military ? .069f : .084f);
    }
    return result;
}

Vec3 campaignTurn(Vec3 p, Vec3 pivot, float angle) {
    const Vec3 v = p - pivot;
    const float c = std::cos(angle), s = std::sin(angle);
    return pivot + Vec3{c * v.x + s * v.z, v.y, c * v.z - s * v.x};
}

void writeCampaignPerson(CreatureWriter& w, const CampaignRig& rig, CampaignHuman person, const CharacterDesign& design,
                         const CharacterMaterials& m) {
    const Vec3 up{0, 1, 0}, right{1, 0, 0}, forwardDir{0, 0, 1};
    const auto& [limbs, pelvis, waist, chest, shoulders, neck, head, headUp] = rig.joints;
    const bool military = person >= CampaignHuman::Delta;
    const bool nora = person == CampaignHuman::Nora;
    const bool walt = person == CampaignHuman::Walt;
    const bool delta = person == CampaignHuman::Delta;
    const float coat = military || walt ? 1.12f : nora ? 1.02f : .96f;

    w.tube({pelvis - up * .051f, pelvis, waist}, {.143f, .170f, .148f}, {.102f, .115f, .106f}, 12, right, m.pants);
    const Vec3 hem = waist - up * (nora ? .112f : .071f);
    w.tube({hem, waist, chest, shoulders, neck}, {.161f * coat, .159f * coat, .199f * coat, .204f * coat, .079f},
           {.116f, .118f, .143f, .126f, .073f}, 12, right, m.jacket);
    w.tube({hem, hem + (waist - hem) * .37f}, {.166f * coat, .167f * coat}, {.119f, .120f}, 12, right, m.seam, false);
    w.tube({neck - up * .035f, neck + (head - neck) * .50f}, {.061f, .053f}, {.061f, .054f}, 10, right, m.skin);
    w.tube({shoulders + (neck - shoulders) * .52f, neck + up * .012f}, {.115f, .076f}, {.084f, .078f},
           10, right, nora ? m.gear : m.seam, false);

    const Vec3 front = chest + forwardDir * .149f, top = shoulders + forwardDir * .134f;
    const Vec3 lower = hem + forwardDir * .127f, back = chest - forwardDir * .149f;
    ++w.part;
    if (!military) {
        const float zipper = nora ? .032f : .007f;
        w.quad(lower - right * zipper, lower + right * zipper, top + right * .018f, top - right * .011f,
               chest, m.seam);
        for (const float s : {-1.f, 1.f}) {
            w.tri(top + right * (s * .100f), top + right * (s * .058f) + up * .058f,
                  front + right * (s * .029f), chest, nora ? m.gear : m.seam);
            const Vec3 pocket = front + Vec3{s * .109f, walt ? -.031f : -.097f, -.015f};
            w.quad(pocket + Vec3{-.057f, .025f, .002f}, pocket + Vec3{.057f, .027f, .002f},
                   pocket + Vec3{.051f, -.055f, -.003f}, pocket + Vec3{-.048f, -.058f, -.003f}, chest, m.seam);
            w.quad(pocket + Vec3{-.054f, .026f, .006f}, pocket + Vec3{.054f, .027f, .006f},
                   pocket + Vec3{.054f, .010f, .008f}, pocket + Vec3{-.054f, .009f, .008f}, chest, m.jacket);
        }
        if (person == CampaignHuman::Ben) {
            const Vec3 lining = forwardDir * .002f;
            for (const float s : {-1.f, 1.f}) {
                w.quad(top + Vec3{s * .073f, 0, -.006f}, top + Vec3{s * .170f, -.019f, -.021f},
                       front + Vec3{s * .178f, -.172f, -.023f}, front + Vec3{s * .080f, -.172f, .001f}, chest, m.gear);
                w.quad(back + Vec3{s * .085f, .105f, -.012f}, back + Vec3{s * .170f, .106f, .018f},
                       back + Vec3{s * .161f, -.16f, .019f}, back + Vec3{s * .081f, -.17f, .008f}, chest, m.gear);
                w.tri(front + Vec3{s * .062f, -.020f, 0} + lining, front + Vec3{s * .082f, .007f, 0} + lining,
                      front + Vec3{s * .082f, -.064f, 0} + lining, chest, m.accent);
            }
        }
    } else {
        // Plate carrier: stepped plates, load-bearing straps, radio and utility pockets.
        w.tube({hem + up * .027f, chest - up * .037f, shoulders - up * .058f},
               {.158f, .224f, .186f}, {.127f, .172f, .147f}, 12, right, m.gear, false);
        for (const float s : {-1.f, 1.f}) {
            w.quad(front + Vec3{s * .078f, -.100f, .031f}, front + Vec3{s * .139f, -.111f, .028f},
                   top + Vec3{s * .160f, .039f, -.022f}, top + Vec3{s * .091f, .046f, -.014f}, chest, m.seam);
            w.quad(back + Vec3{s * .078f, -.119f, -.028f}, back + Vec3{s * .138f, -.115f, -.026f},
                   shoulders + Vec3{s * .158f, .025f, -.097f}, shoulders + Vec3{s * .090f, .032f, -.091f}, chest, m.seam);
        }
        for (const float x : {-.132f, -.044f, .044f, .132f}) {
            const Vec3 p = front + Vec3{x, -.248f, .015f};
            w.tube({p, p + Vec3{0, .13f, .012f}, p + Vec3{0, .165f, .005f}},
                   {.038f, .043f, .040f}, {.026f, .034f, .031f}, 6, right, m.seam);
            ++w.part;
            w.quad(p + Vec3{-.038f, .121f, .043f}, p + Vec3{.038f, .121f, .043f},
                   p + Vec3{.038f, .137f, .043f}, p + Vec3{-.038f, .137f, .043f}, chest, m.gear);
        }
        const float pack = delta ? .12f : .074f;
        w.tube({back - up * .21f, back - forwardDir * pack + up * .014f, back - forwardDir * pack + up * .14f},
               {.132f, .162f, .126f}, {.041f, .080f, .047f}, 8, right, m.seam);
        w.tube({back + Vec3{-.146f, .081f, -.03f}, back + Vec3{-.153f, .233f, -.038f}},
               {.033f, .033f}, {.026f, .026f}, 6, right, m.boot);
        w.tube({back + Vec3{-.157f, .216f, -.039f}, back + Vec3{-.181f, delta ? .490f : .389f, -.063f}},
               {.009f, .004f}, {.009f, .004f}, 6, right, m.boot);
        ++w.part;
        w.quad(back + Vec3{-.072f, -.036f, -pack - .082f}, back + Vec3{.072f, -.036f, -pack - .082f},
               back + Vec3{.072f, -.009f, -pack - .082f}, back + Vec3{-.072f, -.009f, -pack - .082f},
               chest, m.accent);
    }

    for (int i = 0; i < 2; ++i) {
        const float s = i == 0 ? -1.f : 1.f;
        const auto& arm = limbs[i];
        const Vec3 cuff = arm.elbow + (arm.wrist - arm.elbow) * .88f;
        const Vec3 knuckle = mix(arm.wrist, arm.hand, .74f);
        const Vec3 wristDir = normalized(arm.hand - arm.wrist);
        const Vec3 across = normalized(cross(wristDir, std::abs(wristDir.y) > .85f ? forwardDir : up));
        const uint16_t hands = military ? m.boot : m.skin;
        w.group = uint8_t(i == 0 ? CampaignPart::LeftLeg : CampaignPart::RightLeg);
        w.tube({arm.hip + up * .032f, arm.hip, mix(arm.hip, arm.knee, .49f), arm.knee,
                mix(arm.knee, arm.ankle, .53f), arm.ankle},
               {.082f, .090f, .096f, .073f, .065f, .048f}, {.085f, .098f, .092f, .074f, .064f, .049f},
               10, forwardDir, m.pants);
        const Vec3 shoeDir{0, std::sin(rig.bootRoll[i]), std::cos(rig.bootRoll[i])};
        const Vec3 shoeUp{0, shoeDir.z, -shoeDir.y};
        const Vec3 sole = arm.toe - shoeUp * .019f;
        w.tube({arm.ankle - shoeDir * .059f - shoeUp * .024f, arm.ankle + shoeDir * .039f - shoeUp * .034f, arm.toe},
               {.044f, .047f, .034f}, {.053f, .065f, .051f}, 8, shoeUp, m.boot);
        w.tube({arm.ankle - shoeDir * .067f - shoeUp * .054f, arm.ankle + shoeDir * .045f - shoeUp * .055f, sole},
               {.017f, .019f, .012f}, {.056f, .068f, .053f}, 8, shoeUp, m.seam);
        if (military) {
            const Vec3 knee = arm.knee + forwardDir * .067f;
            w.tube({knee - up * .083f, knee, knee + up * .071f}, {.053f, .081f, .055f},
                   {.019f, .034f, .022f}, 8, right, m.gear);
        }
        w.group = 0;
        const Vec3 cap = arm.shoulder - right * (.095f * s) - up * .018f;
        w.tube({cap, arm.shoulder, mix(arm.shoulder, arm.elbow, .30f), arm.elbow, cuff, arm.wrist},
               {.040f * coat, .071f * coat, .076f * coat, .055f, .049f, .042f},
               {.044f * coat, .073f * coat, .071f, .054f, .047f, .041f}, 10, forwardDir, m.jacket);
        w.tube({cuff, arm.wrist}, {.049f, .046f}, {.047f, .044f}, 8, forwardDir, m.seam, false);
        w.tube({arm.wrist, knuckle}, {.034f, .039f}, {.023f, .023f}, 8, across, hands);
        for (const float f : {-1.5f, -.5f, .5f, 1.5f}) {
            const Vec3 base = knuckle + across * (f * .016f);
            w.tube({base, base + wristDir * (.024f - .005f * std::abs(f)), arm.hand + across * (f * .016f)},
                   {.010f, .009f, .007f}, {.009f, .008f, .006f}, 5, across, hands, false);
        }
        w.tube({arm.wrist + across * (.024f * s), knuckle + across * (.043f * s),
                arm.hand + across * (.030f * s)}, {.014f, .013f, .009f}, {.013f, .011f, .009f}, 6, across, hands, false);

        if (military) {
            const Vec3 armPatch = mix(arm.shoulder, arm.elbow, .39f) + right * (.075f * s);
            const Vec3 along = normalized(arm.shoulder - arm.elbow) * .044f;
            ++w.part;
            w.quad(armPatch - along - forwardDir * .035f, armPatch - along + forwardDir * .035f,
                   armPatch + along + forwardDir * .035f, armPatch + along - forwardDir * .035f, arm.elbow, m.accent);
        }
    }

    if (military) {
        const Vec3 grip = limbs[1].hand, fore = limbs[0].hand;
        const Vec3 axis = normalized(Vec3{fore.x - grip.x, .065f, fore.z - grip.z});
        const Vec3 receiver = grip + Vec3{0, .066f, 0};
        w.tube({receiver - axis * .167f, receiver - axis * .046f, receiver + axis * .145f, receiver + axis * .365f},
               {.044f, .056f, .052f, .031f}, {.042f, .044f, .040f, .028f}, 8, up, m.boot);
        w.tube({receiver + axis * .348f, receiver + axis * .565f, receiver + axis * .608f},
               {.021f, .020f, .034f}, {.021f, .020f, .034f}, 8, up, m.metal);
        w.tube({receiver - up * .007f, receiver + Vec3{0, -.114f, .027f}},
               {.036f, .029f}, {.026f, .022f}, 6, right, m.gear);
        const Vec3 sight = receiver + up * .051f;
        w.tube({sight - axis * .050f, sight + axis * .093f}, {.019f, .020f}, {.027f, .027f}, 8, up, m.gear);
    }
    w.group = uint8_t(CampaignPart::Head);
    writeCampaignHead(w, rig.joints, person, design, m);
}

void buildCampaignPeople(Assets& assets) {
    const auto plain = [&](uint16_t color) { return campaignMaterial(assets, color); };
    const auto boot = plain(rgb(43, 44, 43)), eye = plain(rgb(196, 186, 166));
    const auto iris = plain(rgb(39, 29, 27)), metal = plain(rgb(89, 95, 93));
    std::vector<unsigned> poses;
    poses.reserve(campaignPoseCount);
    for (size_t i = 0; i < assets.campaignPeople.size(); ++i) {
        const auto person = CampaignHuman(i);
        const unsigned poseCount = person < CampaignHuman::Delta ? campaignPoseCount
                                 : person == CampaignHuman::Delta ? campaignAim + 1 : campaignCover;
        poses.resize(poseCount);
        for (unsigned slot = 0; slot < poseCount; ++slot) poses[slot] = slot;
        const auto& d = campaignDesigns[i];
        const CharacterMaterials m{plain(d.skin), plain(d.shadow), plain(d.hair), campaignFabric(assets, d.jacket),
                                   plain(d.seam), campaignFabric(assets, d.pants), plain(d.gear), plain(d.accent),
                                   boot, eye, iris, metal};
        auto& model = assets.campaignPeople[i];
        buildPosedMesh(model.body, model.poses, poses,
                       [&](unsigned slot, std::vector<Vertex>& vertices, std::vector<Triangle>* triangles) {
            const auto rig = campaignRig(slot, person, d);
            CreatureWriter w(vertices, triangles);
            writeCampaignPerson(w, rig, person, d, m);
            const auto pose = [&](Vec3 p, bool leg) {
                if (!leg) {
                    const float weight = motionEase((p.y - rig.joints.pelvis.y) * 2.6f);
                    p = campaignTurn(p, rig.joints.pelvis, rig.twist * weight);
                }
                return Vec3{p.x * d.build, p.y * d.height, p.z};
            };
            for (auto& v : vertices)
                v.position = pose(v.position, v.v == float(CampaignPart::LeftLeg) || v.v == float(CampaignPart::RightLeg));
            model.neck[slot] = pose(rig.joints.neck, false);
        }, true);
        for (const auto part : {CampaignPart::LeftLeg, CampaignPart::RightLeg, CampaignPart::Head})
            model.regions[size_t(part)] = poseRegion(model.body, float(part));
        // Sample UVs from the standing pose so the weave stays with the actor through every animation.
        const auto& stand = model.poses.front();
        for (size_t v = 0; v < model.body.vertexCount(); ++v) {
            const Vec3 p = stand.posePositions[v];
            model.body.vertices[v].u = (p.x + p.z * .68f) * 4.f;
            model.body.vertices[v].v = p.y * 4.f;
        }
    }
}

void buildDesktopScout(Assets& assets) {
    const auto paint = [&](uint16_t color, bool lit = false) { return campaignMaterial(assets, color, lit); };
    const uint16_t keel = paint(rgb(41, 55, 67)), slate = paint(rgb(71, 100, 111));
    const uint16_t bronze = paint(rgb(151, 100, 65)), copper = paint(rgb(202, 145, 85));
    const uint16_t ceramic = paint(rgb(196, 191, 160)), inset = paint(rgb(24, 39, 49));
    const uint16_t glass = paint(rgb(38, 73, 90)), glassTop = paint(rgb(70, 115, 132));
    const uint16_t guide = paint(rgb(111, 226, 217), true), beacon = paint(rgb(250, 184, 92), true);
    const uint16_t driveLight = paint(rgb(173, 250, 236), true);
    const auto skin = paint(rgb(163, 170, 153));
    constexpr int textureSize = 128;
    Texture panels{textureSize, textureSize, {}};
    panels.pixels.reserve(textureSize * textureSize);
    for (int y = 0; y < textureSize; ++y) for (int x = 0; x < textureSize; ++x) {
        const int noise = int(grain(x, y)) / 2 + tileNoise(x, y, textureSize, 16) / 2;
        const int seam = x % 64 == 0 || y % 32 == 0 ? 21 : 0;
        const int brushed = (x + 3 * y) % 9 < 2 ? 3 : 0;
        const int light = noise - seam + brushed;
        panels.pixels.push_back(rgb(144 + light, 160 + light, 154 + light));
    }
    assets.materials[skin].texture = int16_t(assets.textures.size());
    assets.textures.push_back(std::move(panels));

    auto& mesh = assets.desktopScout;
    MeshWriter w{mesh};
    auto& landing = assets.desktopScoutGear;
    MeshWriter foot{landing};
    CreatureWriter landingParts(landing.vertices, &landing.triangles);
    constexpr std::array<Vec3, 18> profile{{
        {0, 0, 5.18f}, {1.27f, 0, 4.75f}, {2.47f, 0, 3.55f}, {3.63f, 0, 2.10f},
        {4.46f, 0, .82f}, {4.63f, 0, -.50f}, {4.17f, 0, -1.95f}, {3.19f, 0, -3.35f},
        {1.64f, 0, -4.91f}, {0, 0, -4.16f}, {-1.64f, 0, -4.91f}, {-3.19f, 0, -3.35f},
        {-4.17f, 0, -1.95f}, {-4.63f, 0, -.50f}, {-4.46f, 0, .82f}, {-3.63f, 0, 2.10f},
        {-2.47f, 0, 3.55f}, {-1.27f, 0, 4.75f}
    }};
    const auto point = [&](size_t i, float radius, float y) {
        return profile[i % profile.size()] * radius + Vec3{0, y, 0};
    };
    for (size_t i = 0; i < profile.size(); ++i) {
        const size_t j = i + 1;
        const auto strip = [&](float r0, float y0, float r1, float y1, uint16_t mat) {
            w.quad(point(i, r0, y0), point(i, r1, y1), point(j, r1, y1), point(j, r0, y0), mat);
        };
        w.tri({0, 1.22f, 0}, point(i, .41f, 1.18f), point(j, .41f, 1.18f), slate);
        strip(.41f, 1.18f, .65f, .95f, slate);
        strip(.65f, .95f, .91f, .43f, skin);
        strip(.91f, .43f, .98f, .27f, i % 3 ? ceramic : bronze);
        strip(.98f, .27f, 1.f, .045f, bronze);
        strip(1.f, .045f, .98f, -.18f, inset);
        strip(.98f, -.18f, .87f, -.38f, slate);
        strip(.87f, -.38f, .59f, -.87f, keel);
        strip(.59f, -.87f, .32f, -1.22f, slate);
        w.tri({0, -1.29f, 0}, point(j, .32f, -1.22f), point(i, .32f, -1.22f), keel);

        // The narrow perimeter channel is interrupted by structure between armor panels.
        if (i > 0 && i < profile.size() - 1) {
            const auto between = [&](float fraction, float radius, float y) {
                return mix(point(i, radius, y), point(j, radius, y), fraction);
            };
            const uint16_t light = i % 4 ? guide : beacon;
            w.quad(between(.18f, .996f, -.004f), between(.18f, .994f, -.058f),
                   between(.83f, .994f, -.058f), between(.83f, .996f, -.004f), light);
            w.quad(between(.05f, .72f, .816f), between(.045f, .90f, .463f),
                   between(.077f, .90f, .463f), between(.077f, .72f, .816f), bronze);
        }
    }

    // The forward viewport wraps around a low bridge blister with a pointed deck fairing.
    const Vec3 browL{-.77f, 1.51f, 1.19f}, browR{.77f, 1.51f, 1.19f};
    const Vec3 chinL{-.66f, 1.05f, 2.57f}, chinR{.66f, 1.05f, 2.57f};
    w.quad(chinL, chinR, browR, browL, glass);
    w.quad(browL, browR, {.57f, 1.55f, .29f}, {-.57f, 1.55f, .29f}, ceramic);
    w.quad(chinR, {1.05f, 1.072f, .65f}, {.57f, 1.55f, .29f}, browR, glassTop);
    w.quad({-1.05f, 1.072f, .65f}, chinL, browL, {-.57f, 1.55f, .29f}, glassTop);
    w.quad({-.57f, 1.55f, .29f}, {.57f, 1.55f, .29f}, {.72f, 1.18f, -.6f}, {-.72f, 1.18f, -.6f}, slate);
    w.tri(chinL, {0, .70f, 3.92f}, chinR, copper);
    for (const float x : {-.592f, -.217f, .217f, .592f}) {
        w.quad({x - .023f, 1.056f, 2.574f}, {x + .024f, 1.056f, 2.574f},
               {x + .029f, 1.518f, 1.194f}, {x - .029f, 1.518f, 1.194f}, bronze);
    }
    w.quad({-.602f, 1.138f, 2.347f}, {.603f, 1.138f, 2.347f}, {.620f, 1.16f, 2.307f},
           {-.620f, 1.16f, 2.307f}, glassTop);

    // Dorsal radiators and swept fins give the ship a distinct rear profile in the chase camera.
    CreatureWriter sculpt(mesh.vertices, &mesh.triangles);
    const Vec3 up{0, 1, 0}, right{1, 0, 0};
    const auto sweep = [&](Vec3 at) {
        for (int i = 0; i < 5; ++i) {
            const float z = at.z - float(i) * .209f;
            w.box({at.x - .32f, at.y, z - .092f}, {at.x + .32f, at.y + .035f, z + .032f}, inset);
            w.box({at.x - .275f, at.y + .031f, z - .053f}, {at.x + .275f, at.y + .060f, z + .012f}, bronze);
        }
    };
    for (const float s : {-1.f, 1.f}) {
        const Vec3 drive{mission_detail::scout::scoutDrive.x * s, mission_detail::scout::scoutDrive.y,
                         mission_detail::scout::scoutDrive.z};
        const auto pod = [&](float x, float y, float z) { return Vec3{drive.x + s * x, y, drive.z + z}; };
        sculpt.tube({pod(0, .36f, 3.65f), pod(0, .56f, 2.83f), pod(.07f, .53f, 1.73f),
                     pod(.07f, .48f, .88f), drive + Vec3{0, 0, .14f}},
                    {.29f, .53f, .57f, .60f, .56f}, {.23f, .39f, .49f, .52f, .49f}, 10, up, slate);
        sculpt.tube({pod(.05f, .47f, .95f), pod(.02f, .40f, .28f), drive},
                    {.61f, .59f, .49f}, {.53f, .53f, .45f}, 10, up, bronze, false);

        // Nested annuli read as an engine bell, with ribs silhouetted in the rear glow.
        const auto iris = [&](float radius, float depth, int i) {
            const float a = 2.f * pi * float(i % 12) / 12;
            return drive + Vec3{radius * std::sin(a), radius * .9f * std::cos(a), depth};
        };
        for (int i = 0; i < 12; ++i) {
            const auto beside = i + 1;
            w.quad(iris(.47f, -.02f, i), iris(.47f, -.02f, beside),
                   iris(.37f, -.033f, beside), iris(.37f, -.033f, i), copper);
            w.quad(iris(.37f, -.032f, i), iris(.37f, -.032f, beside),
                   iris(.31f, -.042f, beside), iris(.31f, -.042f, i), guide);
            w.tri(drive + Vec3{0, 0, -.012f}, iris(.31f, -.041f, i), iris(.31f, -.041f, beside),
                  i % 3 == 0 ? inset : driveLight);
        }
        sweep({drive.x, 1.208f, -.82f});
        const Vec3 root{s * 2.38f, .68f, -.53f}, finTip{s * 3.24f, 1.63f, -2.70f};
        const Vec3 finAft{s * 2.91f, .74f, -3.58f}, finToe{s * 2.14f, .81f, -2.51f};
        const Vec3 width{.125f * s, 0, 0};
        const auto face = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 center, uint16_t mat) { sculpt.tri(a, b, c, center, mat); };
        face(root, finTip, finAft, finToe, ceramic);
        face(root - width, finTip - width, finAft - width, finTip, slate);
        sculpt.quad(finTip, finTip - width, finAft - width, finAft, root, copper);
        sculpt.quad(root, root - width, finTip - width, finTip, finAft, bronze);
        sculpt.tube({finTip + Vec3{0, -.10f, .12f}, finTip - Vec3{0, .012f, .022f}},
                    {.032f, .038f}, {.032f, .038f}, 6, right, s < 0 ? beacon : guide);

        for (const float z : {-1.82f, 1.34f}) {
            const float x = s * (z > 0 ? 2.09f : 2.38f);
            landingParts.tube({{x, -.79f, z}, {x, -1.38f, z + .045f}},
                              {.172f, .139f}, {.167f, .139f}, 8, right, bronze);
            landingParts.tube({{x, -1.28f, z + .04f}, {x, -1.74f, z + .085f}},
                              {.119f, .094f}, {.119f, .100f}, 8, right, slate);
            foot.box({x - .213f, -1.80f, z - .218f}, {x + .213f, -1.72f, z + .34f}, keel);
        }
    }
    // The amber retrieval iris shares the in-game beam's emitter point.
    const Vec3 emitter = fleetHulls[2].emitter();
    for (int i = 0; i < 16; ++i) {
        const auto at = [&](float r, float h, int step) {
            const float a = (float(step % 16) + .5f) * pi / 8.f;
            return emitter + Vec3{std::cos(a) * r, h, std::sin(a) * r};
        };
        const auto strip = [&](float r0, float h0, float r1, float h1, uint16_t mat) {
            w.quad(at(r0, h0, i), at(r0, h0, i + 1), at(r1, h1, i + 1), at(r1, h1, i), mat);
        };
        strip(1.39f, .59f, 1.17f, .22f, i % 2 ? bronze : copper);
        strip(1.17f, .22f, .91f, .08f, ceramic);
        strip(.91f, .08f, .73f, .17f, inset);
        strip(.73f, .17f, .62f, .17f, beacon);
        w.tri(at(.62f, .17f, i), at(.62f, .17f, i + 1), emitter + Vec3{0, .12f, 0}, guide);
    }

    for (auto& v : mesh.vertices) v.u = v.v = 0;
    for (const auto& tri : mesh.triangles) {
        if (tri.material != skin) continue;
        for (const auto index : {tri.a, tri.b, tri.c}) {
            auto& v = mesh.vertices[index];
            v.u = v.position.x * .43f;
            v.v = v.position.z * .43f;
        }
    }
    finalizeProceduralMesh(mesh);
    for (auto& v : landing.vertices) v.u = v.v = 0;
    finalizeProceduralMesh(landing);
}

float cloudNoise(Vec3 p) {
    const int x = int(std::floor(p.x)), y = int(std::floor(p.y)), z = int(std::floor(p.z));
    const float u = smoothStep(p.x - x), v = smoothStep(p.y - y), w = smoothStep(p.z - z);
    const auto at = [](int a, int b, int c) {
        uint32_t hash = uint32_t(a) * 0x8da6b343u ^ uint32_t(b) * 0xd8163841u ^ uint32_t(c) * 0xcb1ab31fu;
        hash = (hash ^ (hash >> 13)) * 0x85ebca6bu;
        return float(hash >> 16) * (1.f / 65535.f);
    };
    const auto row = [&](int b, int c) {
        const float left = at(x, b, c), right = at(x + 1, b, c);
        return left + (right - left) * u;
    };
    const float back = row(y, z) * (1.f - v) + row(y + 1, z) * v;
    const float front = row(y, z + 1) * (1.f - v) + row(y + 1, z + 1) * v;
    return back + (front - back) * w;
}

void buildDesktopSkies(Assets& assets) {
    const auto packed = [](Vec3 v) {
        const auto channel = [](float x) { return uint32_t(std::clamp(int(x + .5f), 0, 255)); };
        return channel(v.x) << 16 | channel(v.y) << 8 | channel(v.z);
    };
    constexpr int size = desktopSkyFaceSize;
    for (size_t i = 0; i < std::size(skyAxes); ++i) {
        const auto& [normal, right, vertical] = skyAxes[i];
        auto& dusk = assets.desktopDusk[i];
        auto& basin = assets.desktopTimberlineSky[i];
        dusk.resize(size * size);
        basin.resize(size * size);
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            const float u = 2.f * x / (size - 1) - 1.f, v = 2.f * y / (size - 1) - 1.f;
            const Vec3 dir = normalized(normal + right * u + vertical * v);
            const float lift = std::max(0.f, dir.y);
            const float horizon = std::exp(-6.2f * lift);
            const float west = std::max(0.f, dot(dir, sunsetSunDir));
            const float facing = std::pow(west, 4.f);
            const float low = std::pow(west, 19.f);

            // Project cloud decks in world directions so the texture joins are continuous.
            const float perspective = 1.f / (.37f + lift);
            const Vec3 sheet{(dir.x * .91f + dir.z * .42f) * perspective,
                             (dir.z * .91f - dir.x * .42f) * perspective, .7f};
            const Vec3 p{3.4f * sheet.x + 12.3f, 5.8f * sheet.y + 34.1f, 9.1f};
            const float forms = .59f * cloudNoise(p) + .27f * cloudNoise(p * 2.1f + Vec3{4, 11, 7}) +
                                .14f * cloudNoise(p * 4.53f + Vec3{1, 9, 3});
            const Vec3 cirrus{dir.x * 5.1f + dir.z * 2.7f + 4.f, dir.y * 15.6f - dir.z * 2.65f + 8.f,
                              dir.z * 5.3f + .8f};
            const float fine = .70f * cloudNoise(cirrus) + .30f * cloudNoise(cirrus * 3.4f + Vec3{6, 9, 4});
            const float above = smoothStep((dir.y + .09f) / .23f);
            const float band = smoothStep((forms - .38f) / .29f) * above * (1.f - .53f * lift);
            const float veil = smoothStep((fine - .49f) / .33f) * above * (.71f + .29f * lift);
            const float night = smoothStep(lift * 1.47f);
            const float below = smoothStep(-dir.y * 1.9f);

            Vec3 sunset = mix({128, 115, 139}, {28, 49, 83}, night);
            sunset += Vec3{87, 37, -13} * (facing * (.31f + .69f * horizon)) +
                      Vec3{55, 12, -20} * (horizon * west * west) +
                      Vec3{44, 85, 78} * low +
                      Vec3{27, 43, 63} * std::pow(west, 430.f);
            const Vec3 shadow = mix({46, 61, 99}, {130, 71, 99}, facing * (.55f + .45f * horizon));
            sunset = mix(sunset, shadow, band * .66f);
            const float litRim = smoothStep((forms - .38f) / .14f) * (1.f - smoothStep((forms - .52f) / .17f)) * above;
            sunset += Vec3{61, 41, 21} * (litRim * facing * .92f) +
                      mix({16, 22, 34}, {42, 22, 14}, facing) * (veil * .50f);
            sunset = mix(sunset, {69, 66, 80}, below * .86f);

            Vec3 alpine = mix({157, 173, 181}, {58, 94, 129}, night);
            alpine += Vec3{47, 30, 11} * (horizon * facing) + Vec3{23, 28, 30} * low;
            alpine = mix(alpine, mix({88, 112, 141}, {141, 131, 132}, horizon * facing), band * .63f);
            alpine += Vec3{32, 33, 30} * (veil * .68f + litRim * (.18f + .35f * facing));
            alpine = mix(alpine, {102, 119, 135}, below * .87f);

            const size_t index = size_t(y) * size + x;
            dusk[index] = packed(sunset);
            basin[index] = packed(alpine);
        }
    }
}

void buildDesktopDrones(Assets& assets) {
    const auto paint = [&](uint16_t color, bool lit = false) {
        return campaignMaterial(assets, color, lit);
    };
    const auto body = paint(rgb(79, 113, 123)), chine = paint(rgb(161, 193, 188)), dark = paint(rgb(36, 58, 71));
    const auto copper = paint(rgb(186, 126, 74)), gold = paint(rgb(247, 191, 104), true);
    const auto cyan = paint(rgb(95, 235, 249), true), white = paint(rgb(215, 254, 249), true);
    const auto jamBody = paint(rgb(123, 85, 112)), jamCase = paint(rgb(187, 157, 128));
    const auto fuchsia = paint(rgb(249, 82, 167), true), pink = paint(rgb(255, 184, 203), true);
    const Vec3 up{0, 1, 0}, right{1, 0, 0}, forwardDir{0, 0, 1};

    for (const bool jammer : {false, true}) {
        auto& mesh = jammer ? assets.desktopJammer : assets.desktopAttackDrone;
        MeshWriter w{mesh};
        CreatureWriter shell(mesh.vertices, &mesh.triangles);
        shell.tube({{0, 0, -.59f}, {0, 0, -.28f}, {0, -.011f, .12f}, {0, -.015f, .39f}, {0, -.021f, .59f}},
                   {.135f, .229f, .276f, .207f, .106f}, {.216f, .375f, .476f, .353f, .216f},
                   10, up, jammer ? jamBody : body);
        shell.tube({{0, .096f, -.45f}, {0, .14f, -.03f}, {0, .149f, .30f}, {0, .065f, .47f}},
                   {.077f, .159f, .105f, .027f}, {.185f, .284f, .206f, .117f}, 10, up, jammer ? jamCase : chine);
        shell.tube({{0, -.149f, -.39f}, {0, -.173f, -.025f}, {0, -.173f, .28f}, {0, -.111f, .41f}},
                   {.053f, .134f, .101f, .036f}, {.174f, .275f, .194f, .115f}, 8, up, dark);

        // A focused front lens marks the shot origin. The jammer has a separate relay above it.
        shell.tube({{0, 0, .44f}, {0, 0, .565f}, {0, 0, .600f}},
                   {.107f, .107f, .084f}, {.162f, .204f, .168f}, 10, up, dark);
        w.quad({-.148f, -.044f, .603f}, {.148f, -.044f, .603f}, {.129f, .040f, .603f},
               {-.129f, .040f, .603f}, jammer ? fuchsia : cyan);
        w.quad({-.079f, -.012f, .606f}, {.079f, -.012f, .606f}, {.063f, .017f, .606f},
               {-.063f, .017f, .606f}, jammer ? pink : white);

        for (const float sign : {-1.f, 1.f}) {
            const auto point = [&](float x, float y, float z) { return Vec3{x * sign, y, z}; };
            const Vec3 wing = point(droneWingCenter.x, droneWingCenter.y, droneWingCenter.z);
            const Vec3 nose = point(.49f, -.035f, .176f), shoulder = point(.32f, .061f, .18f);
            const Vec3 aft = point(.39f, .009f, -.38f);
            const Vec3 out = point(jammer ? .778f : .847f, jammer ? .008f : -.096f, -.24f);
            const Vec3 flare = point(.73f, -.09f, -.411f);
            shell.quad(shoulder, nose, out, aft, wing - up * .10f, jammer ? jamCase : chine);
            shell.tri(out, flare, aft, wing - up * .10f, jammer ? copper : body);
            const Vec3 underside{0, -.092f, 0};
            shell.quad(nose, nose + underside, out + underside, out, wing, copper);
            shell.quad(out, out + underside, flare + underside, flare, wing, copper);
            shell.tri(aft + underside, out + underside, nose + underside, wing + up * .09f, dark);
            shell.tube({point(.605f, -.092f, .076f), wing - up * .045f, point(.705f, -.096f, -.413f)},
                       {.082f, .123f, .106f}, {.075f, .126f, .090f}, 8, up, dark);
            shell.tube({point(.69f, -.099f, -.37f), point(.705f, -.098f, -.429f), point(.705f, -.098f, -.45f)},
                       {.088f, .093f, .054f}, {.098f, .099f, .052f}, 8, up, jammer ? gold : cyan);
            const Vec3 light = nose + up * .016f, edge = mix(nose, out, .8f) + up * .013f;
            shell.quad(light, edge, edge + Vec3{0, 0, -.05f}, light + Vec3{0, 0, -.05f},
                       wing - up * .04f, jammer ? gold : cyan);

            if (!jammer) {
                const Vec3 tip = point(.856f, .121f, -.396f);
                shell.tri(out, tip, flare, wing, chine);
                shell.tri(out - right * (.041f * sign), flare, tip - right * (.051f * sign), out, body);
                shell.quad(out, out - right * (.041f * sign), tip - right * (.051f * sign), tip, flare, copper);
                continue;
            }
            // Tall, forward-facing antenna panels are also part of the jammer's hittable silhouette.
            const auto c = jammerPanelCenter;
            const auto r = jammerPanelHalf;
            const Vec3 root = point(c.x - .045f, c.y - r.y - .06f, c.z - .07f);
            const Vec3 middle = point(c.x, c.y, c.z);
            const Vec3 top = point(c.x + .007f, c.y + r.y, c.z - .022f);
            shell.tube({root, middle, top}, {.091f, .104f, .090f}, {.086f, .092f, .069f}, 8, forwardDir, jamCase);
            shell.tube({mix(middle, top, .72f), mix(middle, top, .88f)},
                       {.110f, .106f}, {.105f, .102f}, 8, forwardDir, fuchsia, false);
            const Vec3 rim = point(c.x, c.y, c.z + .120f);
            shell.quad(rim + Vec3{-.062f, -.167f, -.009f}, rim + Vec3{.062f, -.164f, -.009f},
                       rim + Vec3{.065f, .173f, -.020f}, rim + Vec3{-.064f, .174f, -.020f}, middle, fuchsia);
            for (const float h : {-.10f, .009f, .12f}) {
                const float z = h > 0 ? -.008f - h * .062f : -.007f;
                shell.quad(rim + Vec3{-.044f, h - .014f, z}, rim + Vec3{.044f, h - .014f, z},
                           rim + Vec3{.042f, h + .014f, z}, rim + Vec3{-.042f, h + .014f, z}, middle, pink);
            }
        }
        if (jammer) {
            shell.tube({jammerCore + Vec3{0, -.14f, -.08f}, jammerCore, jammerCore + Vec3{0, .10f, -.024f}},
                       {.103f, .151f, .072f}, {.106f, .139f, .087f}, 8, right, dark);
            const Vec3 face = jammerCore + Vec3{0, 0, .134f};
            w.quad(face + Vec3{-.095f, 0, 0}, face + Vec3{0, -.072f, 0},
                   face + Vec3{.095f, 0, 0}, face + Vec3{0, .075f, 0}, pink);
        }
        for (auto& v : mesh.vertices) v.u = v.v = 0;
        finalizeProceduralMesh(mesh);
    }
}
