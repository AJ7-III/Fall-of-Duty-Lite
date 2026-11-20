#include "assets.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#ifdef ESP_PLATFORM
#include "miniz.h"
#else
#include <zlib.h>
#endif

namespace fod {
namespace {

constexpr uint32_t maxPackBytes = 24 * 1024 * 1024;
constexpr uint32_t maxResidentBytes = 12 * 1024 * 1024;
constexpr uint32_t bufferBytes = 8192;
constexpr uint32_t headerBytes = 48;
constexpr uint32_t legacySectionCount = 11;
constexpr uint32_t voiceSectionCount = 13;
constexpr uint32_t sectionCount = 14;
constexpr uint32_t legacySectionEntryBytes = 24;
constexpr uint32_t compressedSectionEntryBytes = 28;

struct BlockInflater {
    std::array<uint8_t, bufferBytes> input;
#ifdef ESP_PLATFORM
    tinfl_decompressor decoder;
    static constexpr size_t libraryScratch = 0;
#else
    // Reserve the desktop zlib state and up to a 32 KiB history window.
    static constexpr size_t libraryScratch = 64 * 1024;
#endif

    bool decode(uint32_t stored, uint8_t* output, uint32_t unpacked) {
#ifdef ESP_PLATFORM
        size_t consumed = stored, written = unpacked;
        tinfl_init(&decoder);
        const auto status = tinfl_decompress(&decoder, input.data(), &consumed, output, output, &written,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | TINFL_FLAG_HAS_MORE_INPUT);
        return status == TINFL_STATUS_DONE && consumed == stored && written == unpacked;
#else
        uLong consumed = stored;
        uLongf written = unpacked;
        return uncompress2(output, &written, input.data(), &consumed) == Z_OK && consumed == stored && written == unpacked;
#endif
    }
};

#ifdef ESP_PLATFORM
constexpr std::array<uint32_t, 256> crcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
        table[i] = crc;
    }
    return table;
}
constexpr auto crcs = crcTable();
#endif

class Reader {
public:
    explicit Reader(std::FILE* file) : file_(file), buffer_(new uint8_t[bufferBytes]) {}

    static constexpr size_t scratchBytes(bool compressed) {
        return bufferBytes + (compressed ? sizeof(BlockInflater) + BlockInflater::libraryScratch : 0);
    }

    void reset(uint32_t size) { reset(size, size); }

    void reset(uint32_t stored, uint32_t unpacked) {
        storedRemaining_ = stored;
        remaining_ = unpacked;
        compressed_ = stored != unpacked;
        index_ = 0;
        available_ = 0;
        crc_ = ~0u;
        okay_ = true;
    }

    bool bytes(void* output, uint32_t count) {
        auto* dest = static_cast<uint8_t*>(output);
        if (!okay_ || count > remaining_) {
            okay_ = false;
            return false;
        }
        while (count) {
            if (index_ == available_) {
                if (!refill()) {
                    okay_ = false;
                    return false;
                }
            }
            const uint32_t copied = std::min(count, available_ - index_);
            std::memcpy(dest, buffer_.get() + index_, copied);
            index_ += copied;
            dest += copied;
            count -= copied;
            remaining_ -= copied;
        }
        return true;
    }

    uint8_t u8() {
        uint8_t result = 0;
        bytes(&result, sizeof(result));
        return result;
    }

    uint16_t u16() {
        uint8_t data[2]{};
        bytes(data, sizeof(data));
        return uint16_t(data[0]) | (uint16_t(data[1]) << 8);
    }

    int16_t i16() { return static_cast<int16_t>(u16()); }

    uint32_t u32() {
        uint8_t data[4]{};
        bytes(data, sizeof(data));
        return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
    }

    float f32() {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "Pack uses IEEE single precision");
        const uint32_t word = u32();
        float result = 0;
        std::memcpy(&result, &word, sizeof(result));
        if (!std::isfinite(result)) okay_ = false;
        return result;
    }

    Vec3 vec3() { return {f32(), f32(), f32()}; }

    bool done() const { return okay_ && !remaining_ && !storedRemaining_; }
    bool okay() const { return okay_; }
    uint32_t remaining() const { return remaining_; }
    uint32_t crc() const { return ~crc_; }

private:
    bool readStored(void* output, uint32_t size) {
        if (size > storedRemaining_ || std::fread(output, 1, size, file_) != size) return false;
        storedRemaining_ -= size;
        return true;
    }

    bool refill() {
        available_ = std::min(remaining_, bufferBytes);
        index_ = 0;
        if (compressed_) {
            uint8_t length[2];
            if (!readStored(length, sizeof(length))) return false;
            const uint32_t stored = uint32_t(length[0]) | (uint32_t(length[1]) << 8);
            if (!stored || stored > available_) return false;
            if (stored == available_) {
                if (!readStored(buffer_.get(), stored)) return false;
            } else {
                if (!inflater_) inflater_.reset(new (std::nothrow) BlockInflater);
                if (!inflater_ || !readStored(inflater_->input.data(), stored) ||
                    !inflater_->decode(stored, buffer_.get(), available_)) return false;
            }
        } else if (!readStored(buffer_.get(), available_)) {
            return false;
        }
#ifdef ESP_PLATFORM
        for (uint32_t i = 0; i < available_; ++i) crc_ = (crc_ >> 8) ^ crcs[(crc_ ^ buffer_[i]) & 0xffu];
#else
        // zlib takes and returns the complemented CRC for each block.
        crc_ = ~uint32_t(crc32(~crc_, buffer_.get(), available_));
#endif
        return true;
    }

    std::FILE* file_;
    std::unique_ptr<uint8_t[]> buffer_;
    std::unique_ptr<BlockInflater> inflater_;
    uint32_t storedRemaining_ = 0, remaining_ = 0, index_ = 0, available_ = 0;
    uint32_t crc_ = 0;
    bool okay_ = true, compressed_ = false;
};

struct Section {
    uint32_t encoding, stored, unpacked, crc;
};

class LoadBudget {
public:
    uint64_t bytes = sizeof(Assets);
    uint64_t extra = 0;
    uint64_t peak = 0;

    template<class T>
    bool resize(Reader& in, std::vector<T>& data, uint32_t count, uint32_t minimumStored, uint32_t ceiling) {
        if (!in.okay() || count > ceiling || (minimumStored && count > in.remaining() / minimumStored)
            || bytes + extra + uint64_t(count) * sizeof(T) > maxResidentBytes) return false;
        data.resize(count);
        bytes += uint64_t(data.capacity()) * sizeof(T);
        peak = std::max(peak, bytes + extra);
        return bytes + extra <= maxResidentBytes;
    }
};

bool validBounds(const Bounds& bounds) {
    return bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z;
}

bool readChunks(Reader& in, std::vector<Chunk>& chunks, uint32_t triangles) {
    uint32_t next = 0;
    for (auto& chunk : chunks) {
        chunk = {{in.vec3(), in.vec3()}, in.u32(), in.u32()};
        if (!in.okay() || !validBounds(chunk.bounds) || chunk.first != next || !chunk.count || chunk.count > triangles - next) return false;
        next += chunk.count;
    }
    return next == triangles;
}

bool readMaterials(Reader& in, LoadBudget& budget, Assets& assets) {
    const uint32_t count = in.u32();
    if (!count || !budget.resize(in, assets.materials, count, 6, 4096)) return false;
    for (auto& mat : assets.materials) {
        mat = {in.u16(), in.i16(), in.u8(), false};
        const auto flags = in.u8();
        if (flags > 1 || mat.texture < -1) return false;
        mat.doubleSided = flags != 0;
    }
    return in.okay();
}

bool readTexture(Reader& in, LoadBudget& budget, Texture& texture) {
    texture.width = in.u16();
    texture.height = in.u16();
    if (!texture.width || !texture.height || texture.width > 2048 || texture.height > 2048
        || !budget.resize(in, texture.pixels, texture.width * texture.height, 2, 2048 * 2048)) return false;
    return in.bytes(texture.pixels.data(), uint32_t(texture.pixels.size()) * 2);
}

bool readTextures(Reader& in, LoadBudget& budget, Assets& assets) {
    const uint32_t count = in.u32();
    if (!budget.resize(in, assets.textures, count, 4, 512)) return false;
    for (auto& texture : assets.textures) if (!readTexture(in, budget, texture)) return false;
    return true;
}

bool readMesh(Reader& in, LoadBudget& budget, Mesh& mesh, uint32_t encoding, uint32_t materials, const Mesh* reference) {
    const uint32_t vertices = in.u32();
    const uint32_t triangles = in.u32();
    const uint32_t chunks = in.u32();
    const uint32_t flags = in.u32();
    if (!vertices || flags > 1 || (flags && (!reference || vertices != reference->vertices.size() || triangles || chunks))
        || (!flags && (!triangles || !chunks))) return false;
    Bounds bounds{};
    Vec3 scale{};
    if (encoding == 1) {
        bounds = {in.vec3(), in.vec3()};
        if (!validBounds(bounds)) return false;
        scale = (bounds.max - bounds.min) / 65535.0f;
    }
    const uint32_t positionBytes = encoding == 1 ? 6 : 12;
    if (flags) {
        if (!budget.resize(in, mesh.posePositions, vertices, positionBytes, 200000)) return false;
    } else if (!budget.resize(in, mesh.vertices, vertices, positionBytes + 8, 200000)) {
        return false;
    }
    for (uint32_t i = 0; i < vertices; ++i) {
        auto& position = flags ? mesh.posePositions[i] : mesh.vertices[i].position;
        if (encoding == 1) {
            position = {
                bounds.min.x + scale.x * in.u16(),
                bounds.min.y + scale.y * in.u16(),
                bounds.min.z + scale.z * in.u16(),
            };
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return false;
        } else {
            position = in.vec3();
        }
        if (!flags) {
            mesh.vertices[i].u = in.f32();
            mesh.vertices[i].v = in.f32();
        }
    }
    if (!budget.resize(in, mesh.triangles, triangles, 16, 200000)) return false;
    for (auto& tri : mesh.triangles) {
        tri = {in.u32(), in.u32(), in.u32(), in.u16(), in.i16()};
        if (tri.a >= vertices || tri.b >= vertices || tri.c >= vertices || tri.material >= materials || tri.glass < -1) return false;
    }
    if (!budget.resize(in, mesh.chunks, chunks, 32, 4096) || !readChunks(in, mesh.chunks, triangles)) return false;
    // Expand baked bounds by a quantization step so the visual cull cannot
    // drop an edge shifted over the source bounds during quantization.
    for (auto& chunk : mesh.chunks) {
        chunk.bounds.min -= scale;
        chunk.bounds.max += scale;
    }
    return in.okay();
}

bool readMeshes(Reader& in, LoadBudget& budget, Assets& assets, uint32_t encoding) {
    const uint32_t count = in.u32();
    const uint32_t frames = 1 + assets.walkFrameCount + assets.runFrameCount;
    if (count != 8 + frames || !budget.resize(in, assets.rivalFrames, frames, 16, 41)) return false;
    const uint32_t materials = static_cast<uint32_t>(assets.materials.size());
    for (uint32_t i = 0; i < count; ++i) {
        Mesh* mesh = &assets.world;
        const Mesh* reference = nullptr;
        if (i >= 1 && i <= 3) mesh = &assets.weapon[i - 1];
        if (i == 4) mesh = &assets.apache;
        if (i >= 5 && i <= 7) {
            mesh = &assets.weaponAds[i - 5];
            reference = &assets.weapon[i - 5];
        }
        if (i >= 8) {
            mesh = &assets.rivalFrames[i - 8];
            if (i > 8) reference = &assets.rivalFrames[0];
        }
        if (!readMesh(in, budget, *mesh, encoding, materials, reference)) return false;
    }
    return true;
}

bool readShots(Reader& in, LoadBudget& budget, Assets& assets, uint32_t encoding) {
    const auto triangles = in.u32();
    const auto chunks = in.u32();
    const auto surfaces = in.u32();
    if (!surfaces || surfaces > triangles) return false;
    auto& positions = assets.shotVertices;
    if (encoding == 1) {
        const auto count = in.u32();
        if (!count || !budget.resize(in, positions, count, 12, 200000)) return false;
        for (auto& position : positions) position = in.vec3();
    }
    const uint32_t indexBytes = positions.size() <= 65536 ? 2 : 4;
    const uint32_t triangleBytes = encoding == 1 ? 3 * indexBytes + 6 : 42;
    if (!budget.resize(in, assets.shotSurfaces, triangles, triangleBytes, 200000)) return false;
    if (encoding == 1) {
        if (!budget.resize(in, assets.shotIndices, triangles, 0, 200000)) return false;
    } else if (!budget.resize(in, positions, triangles * 3, 0, 600000)) {
        return false;
    }
    for (uint32_t i = 0; i < triangles; ++i) {
        if (encoding == 1) {
            for (auto& corner : assets.shotIndices[i]) {
                corner = indexBytes == 2 ? in.u16() : in.u32();
                if (corner >= positions.size()) return false;
            }
        } else {
            for (uint32_t vertex = 3 * i; vertex < 3 * i + 3; ++vertex) positions[vertex] = in.vec3();
        }
        auto& surface = assets.shotSurfaces[i];
        surface = {in.u32(), in.i16()};
        if (surface.surface >= surfaces || surface.glass < -1) return false;
    }
    return budget.resize(in, assets.shotChunks, chunks, 32, 4096) && readChunks(in, assets.shotChunks, triangles);
}

bool readObstacles(Reader& in, LoadBudget& budget, Assets& assets) {
    const auto count = in.u32();
    if (!count || !budget.resize(in, assets.obstacles, count, 36, 4096)) return false;
    for (auto& box : assets.obstacles) {
        box = {in.f32(), in.f32(), in.f32(), in.f32(), in.f32(), in.f32(), in.f32(), in.f32(), in.f32()};
        if (box.hw <= 0 || box.hd <= 0 || box.minY >= box.maxY) return false;
        const float rotation = box.cos * box.cos + box.sin * box.sin;
        if (std::abs(rotation - 1) > 0.001f) return false;
    }
    return in.okay();
}

bool readNav(Reader& in, LoadBudget& budget, Assets& assets) {
    const auto nodes = in.u32();
    const auto edges = in.u32();
    if (!nodes || !budget.resize(in, assets.navNodes, nodes, 22, 4096)) return false;
    uint32_t consumed = 0;
    for (auto& node : assets.navNodes) {
        node = {in.vec3(), in.u32(), in.u32(), false, false};
        const auto cover = in.u8(), walkable = in.u8();
        if (node.first != consumed || node.count > edges - consumed || node.count > 8 || cover > 1 || walkable > 1
            || (!walkable && node.count)) return false;
        consumed += node.count;
        node.cover = cover;
        node.walkable = walkable;
    }
    if (consumed != edges || !budget.resize(in, assets.navEdges, edges, 8, 4096 * 8)) return false;
    for (auto& edge : assets.navEdges) {
        edge = {in.u32(), in.f32()};
        if (edge.to >= nodes || !assets.navNodes[edge.to].walkable || edge.cost <= 0) return false;
    }
    return in.okay();
}

bool readGlass(Reader& in, LoadBudget& budget, Assets& assets) {
    const auto count = in.u32();
    if (!budget.resize(in, assets.glassHits, count, 1, 256)) return false;
    for (auto& hits : assets.glassHits) {
        hits = in.u8();
        if (hits != 1 && hits != 3) return false;
    }
    return in.okay();
}

bool readAudio(Reader& in, LoadBudget& budget, Assets& assets, uint32_t schema) {
    constexpr auto legacyEffects = uint32_t(Sound::Rain) + 1;
    constexpr auto effects = uint32_t(Sound::VoiceStart);
    constexpr auto allSounds = uint32_t(Sound::Count);
    const auto storedEffects = schema < 4 ? legacyEffects : effects;
    const auto storedVoices = schema == 2 ? allSounds - effects : 0;
    const auto count = in.u32();
    if (count != storedEffects + storedVoices ||
        !budget.resize(in, assets.audio, effects + storedVoices, 0, allSounds)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto index = i < storedEffects ? i : effects + i - storedEffects;
        auto& clip = assets.audio[index];
        const auto samples = in.u32();
        if (!samples || !budget.resize(in, clip.samples, samples, 2, audioHz * 30)
            || !in.bytes(clip.samples.data(), samples * 2)) return false;
    }
    return true;
}

bool readVoices(Reader& in, LoadBudget& budget, Assets& assets) {
    if (in.u32() != assets.voices.size()) return false;
    for (auto& bank : assets.voices) {
        const auto clips = in.u32();
        if (!clips || !budget.resize(in, bank, clips, 8, maxVoiceVariants)) return false;
        for (auto& clip : bank) {
            clip.frames = in.u32();
            clip.predictor = in.i16();
            clip.index = in.u8();
            if (in.u8() || !clip.frames || clip.frames > audioHz * 30 || clip.index > 88) return false;
            const auto bytes = clip.frames / 2;
            if (!budget.resize(in, clip.codes, bytes, 1, audioHz * 15) || !in.bytes(clip.codes.data(), bytes))
                return false;
        }
    }
    return in.okay();
}

bool readSky(Reader& in, LoadBudget& budget, Assets& assets) {
    constexpr uint32_t samples = skyFaceSize * skyFaceSize;
    for (auto& face : assets.sky) {
        if (!budget.resize(in, face, samples, 1, samples) || !in.bytes(face.data(), samples)) return false;
    }
    return true;
}

bool readHitboxes(Reader& in, LoadBudget& budget, Assets& assets) {
    const auto frames = in.u32();
    if (frames != assets.rivalFrames.size() || !budget.resize(in, assets.rivalHitboxes, frames, 4, 41)) return false;
    for (auto& frame : assets.rivalHitboxes) {
        const auto count = in.u32();
        if (!count || !budget.resize(in, frame, count, 64, 32)) return false;
        for (auto& hitbox : frame) {
            hitbox = {in.vec3(), in.vec3(), in.vec3(), in.vec3(), in.vec3(), in.f32()};
            if (hitbox.half.x <= 0 || hitbox.half.y <= 0 || hitbox.half.z <= 0 || hitbox.multiplier <= 0) return false;
        }
    }
    return in.okay();
}

bool readMechanics(Reader& in, LoadBudget& budget, Assets& assets, uint32_t encoding) {
    if (in.u32() != 4) return false;
    for (uint32_t i = 0; i < 4; ++i) {
        const auto& reference = assets.weapon[i < 3 ? i : 1];
        auto& pose = i < 3 ? assets.weaponReload[i] : assets.sniperBolt;
        if (!readMesh(in, budget, pose, encoding, uint32_t(assets.materials.size()), &reference)
            || !pose.triangles.empty()) return false;
    }
    return true;
}

bool readWeaponTracks(Reader& in, LoadBudget& budget, Assets& assets, uint32_t encoding) {
    if (in.u32() != assets.weaponTracks.size()) return false;
    constexpr int references[]{0, 1, 2, 1, 0, 2};
    for (size_t i = 0; i < assets.weaponTracks.size(); ++i) {
        auto& track = assets.weaponTracks[i];
        const auto count = in.u32();
        const auto vertices = uint32_t(assets.weapon[references[i]].vertices.size());
        if (count < 2 || !budget.resize(in, track, count, 4, 32)) return false;
        float previous = -1;
        for (auto& key : track) {
            key.seconds = in.f32();
            if (key.seconds <= previous || key.seconds < 0 || key.seconds > 5 ||
                (previous < 0 && key.seconds != 0)) return false;
            previous = key.seconds;
            if (encoding) {
                if (!budget.resize(in, key.packed, vertices, 6, 200000)) return false;
                for (auto& p : key.packed) p = {in.i16(), in.i16(), in.i16()};
            } else {
                if (!budget.resize(in, key.positions, vertices, 12, 200000)) return false;
                for (auto& p : key.positions) p = in.vec3();
            }
        }
    }
    return in.okay();
}

bool readActorVariants(Reader& in, LoadBudget& budget, Assets& assets, uint32_t encoding) {
    const auto frames = in.u32();
    const auto materials = uint32_t(assets.materials.size());
    if (frames != assets.rivalFrames.size() || !budget.resize(in, assets.hanzFrames, frames, 16, 41)) return false;
    for (size_t i = 0; i < frames; ++i) {
        const auto* base = i ? &assets.hanzFrames.front() : nullptr;
        if (!readMesh(in, budget, assets.hanzFrames[i], encoding, materials, base) ||
            (i && !assets.hanzFrames[i].triangles.empty())) return false;
    }
    return readMesh(in, budget, assets.playerBody, encoding, materials, nullptr);
}

bool validReferences(const Assets& assets) {
    for (const auto& mat : assets.materials)
        if (mat.texture >= static_cast<int>(assets.textures.size())) return false;
    const auto count = static_cast<int>(assets.glassHits.size());
    for (const auto& tri : assets.world.triangles)
        if (tri.glass >= count) return false;
    for (const auto& tri : assets.shotSurfaces)
        if (tri.glass >= count) return false;
    return true;
}

bool readPlayerUniform(Reader& in, LoadBudget& budget, Assets& assets) {
    const auto count = in.u32();
    const auto materials = uint32_t(assets.materials.size());
    if (!count || count > materials || count > in.remaining() / 4 ||
        !budget.resize(in, assets.playerUniform, materials, 0, materials)) return false;
    for (uint32_t i = 0; i < materials; ++i) assets.playerUniform[i] = uint16_t(i);
    for (uint32_t i = 0; i < count; ++i) {
        const auto source = in.u16(), replacement = in.u16();
        if (source >= materials || replacement >= materials || source == replacement ||
            assets.playerUniform[source] != source) return false;
        assets.playerUniform[source] = replacement;
    }
    return in.okay();
}

} // namespace

bool Assets::load(const char* path, std::string& error) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Fall of Duty texture and audio loading require a little-endian target"
#endif
    error.clear();
    const auto failed = [&](const char* reason) {
        error = reason;
        return false;
    };
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::fopen(path, "rb"), &std::fclose);
    if (!file) return failed("Game pack missing. Copy fall-of-duty.fodp to the SD card.");
    if (std::fseek(file.get(), 0, SEEK_END)) return failed("Cannot determine the game pack size.");
    const long fileBytes = std::ftell(file.get());
    if (fileBytes < long(headerBytes + legacySectionCount * legacySectionEntryBytes) || fileBytes > maxPackBytes ||
        std::fseek(file.get(), 0, SEEK_SET))
        return failed("Game pack size is invalid.");

    Reader in(file.get());
    in.reset(headerBytes);
    std::array<uint8_t, 8> magic{};
    in.bytes(magic.data(), magic.size());
    const auto schema = in.u32();
    if (std::memcmp(magic.data(), "FODPACK\0", magic.size()) || schema < 2 || schema > 7)
        return failed("Game pack schema is incompatible.");
    const uint32_t sectionsInFile = schema == 2 ? legacySectionCount : schema < 5 ? voiceSectionCount : sectionCount;
    if (in.u32() != sectionsInFile || in.u32() != uint32_t(fileBytes)) return failed("Game pack header is invalid.");

    Assets next;
    next.artSeed = in.u32();
    in.bytes(next.sourceRevision.data(), next.sourceRevision.size());
    next.walkFrameCount = in.u16();
    next.runFrameCount = in.u16();
    if (!in.done() || !next.walkFrameCount || next.walkFrameCount > 20 || next.runFrameCount > 20)
        return failed("Game pack animation counts are invalid.");
    const auto tableBytes = sectionsInFile * (schema >= 7 ? compressedSectionEntryBytes : legacySectionEntryBytes);
    in.reset(tableBytes);
    std::array<Section, sectionCount> sections{};
    uint64_t expectedOffset = headerBytes + tableBytes;
    uint64_t unpackedBytes = expectedOffset, declaredBytes = 0;
    bool compressed = false;
    for (uint32_t i = 0; i < sectionsInFile; ++i) {
        const auto id = in.u32();
        const auto encoding = in.u32();
        const auto offset = in.u32();
        const auto stored = in.u32();
        const auto resident = in.u32();
        const auto crc = in.u32();
        const auto unpacked = schema >= 7 ? in.u32() : stored;
        const bool encoded = id == 3 || id == 4 || id == 10 || id == 11 || (id == 14 && schema >= 6);
        if (id != i + 1 || encoding > unsigned(encoded) || offset != expectedOffset || !stored || stored > unpacked ||
            uint64_t(offset) + stored > uint32_t(fileBytes)) return failed("Game pack section table is invalid.");
        sections[i] = {encoding, stored, unpacked, crc};
        expectedOffset += stored;
        unpackedBytes += unpacked;
        declaredBytes += resident;
        compressed |= stored != unpacked;
    }
    if (!in.done() || expectedOffset != uint32_t(fileBytes) || unpackedBytes > maxPackBytes || declaredBytes > maxResidentBytes)
        return failed("Game pack sections exceed the storage or memory limit.");

    using Parser = bool (*)(Reader&, LoadBudget&, Assets&);
    constexpr std::array<Parser, sectionCount> parsers = {
        readMaterials, readTextures, nullptr, nullptr, readObstacles, readNav, readGlass, nullptr, readHitboxes,
        nullptr, nullptr, readVoices, readSky, readPlayerUniform,
    };
    constexpr std::array<const char*, sectionCount> names = {
        "materials", "textures", "meshes", "shot surfaces", "obstacles", "navigation", "glass", "audio", "rival hitboxes", "weapon poses",
        "strike jet", "rival voices", "sky", "player uniform",
    };
    LoadBudget budget;
    budget.extra = residentBytes + Reader::scratchBytes(compressed);
    for (uint32_t i = 0; i < sectionsInFile; ++i) {
        const auto section = sections[i];
        in.reset(section.stored, section.unpacked);
        bool decoded;
        if (i == 2) decoded = readMeshes(in, budget, next, section.encoding);
        else if (i == 3) decoded = readShots(in, budget, next, section.encoding);
        else if (i == 7) decoded = readAudio(in, budget, next, schema);
        else if (i == 9) decoded = schema >= 6 ? readWeaponTracks(in, budget, next, section.encoding)
                                             : readMechanics(in, budget, next, section.encoding);
        else if (i == 10) decoded = readMesh(in, budget, next.jet, section.encoding, uint32_t(next.materials.size()), nullptr);
        else if (i == 13 && schema >= 6) decoded = readActorVariants(in, budget, next, section.encoding);
        else decoded = parsers[i](in, budget, next);
        if (!decoded || !in.done() || in.crc() != section.crc) {
            error = std::string("Invalid game pack section: ") + names[i];
            return false;
        }
    }
    if (!validReferences(next)) return failed("Game pack asset indices are invalid.");
    next.storedBytes = fileBytes;
    next.residentBytes = budget.bytes;
    next.peakBytes = budget.peak;
    *this = std::move(next);
    return true;
}

} // namespace fod
