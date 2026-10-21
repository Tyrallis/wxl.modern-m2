// wxl.modern-m2: the model record layout the client's M2 loader reads, and the strides that moved.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

// Taken from wxl-core v1.1 (src/engine/assets/shared/models/m2/M2Format.hpp and Contract.hpp, and
// the stride constants of src/offsets/game/M2.hpp), which the 32-bit extension compiled against.
// The x64 client lays a model out exactly as the file does -- an array is a 32-bit count and a
// 32-bit offset in both -- so the records are the same; only what an offset means changes once the
// client has parsed the file (see Arrays.hpp).

#include <cstddef>
#include <cstdint>

namespace wxl::structure::m2
{
    constexpr uint32_t kMagicMD20 = 0x3032444D; // 'MD20' self-contained model (the only magic the loader accepts)
    constexpr uint32_t kMagicMD21 = 0x3132444D; // 'MD21' chunked container
    constexpr uint32_t kMagicAFM2 = 0x324D4641; // 'AFM2' chunked external-animation wrapper

    constexpr uint32_t kClientVersion = 264;    // the loader's native inner version

    constexpr uint32_t kFlagUseTextureCombinerCombos = 0x8;

#pragma pack(push, 1)

    struct M2Array { uint32_t count; uint32_t offset; };

    struct M2Header
    {
        uint32_t magic;                  // 0x00
        uint32_t version;                // 0x04
        M2Array  name;                   // 0x08
        uint32_t globalFlags;            // 0x10
        M2Array  globalLoops;            // 0x14
        M2Array  sequences;              // 0x1C
        M2Array  sequenceLookup;         // 0x24
        M2Array  bones;                  // 0x2C
        M2Array  boneLookup;             // 0x34
        M2Array  vertices;               // 0x3C
        uint32_t numSkinProfiles;        // 0x44
        M2Array  colors;                 // 0x48
        M2Array  textures;               // 0x50
        M2Array  textureWeights;         // 0x58
        M2Array  textureTransforms;      // 0x60
        M2Array  textureReplacements;    // 0x68
        M2Array  materials;              // 0x70
        M2Array  boneCombos;             // 0x78
        M2Array  textureCombos;          // 0x80
        M2Array  textureUnitLookup;      // 0x88
        M2Array  textureWeightCombos;    // 0x90
        M2Array  textureTransformCombos; // 0x98
        float    boundingBox[6];         // 0xA0
        float    boundingSphereRadius;   // 0xB8
        float    collisionBox[6];        // 0xBC
        float    collisionSphereRadius;  // 0xD4
        M2Array  collisionIndices;       // 0xD8
        M2Array  collisionPositions;     // 0xE0
        M2Array  collisionNormals;       // 0xE8
        M2Array  attachments;            // 0xF0
        M2Array  attachmentLookup;       // 0xF8
        M2Array  events;                 // 0x100
        M2Array  lights;                 // 0x108
        M2Array  cameras;                // 0x110
        M2Array  cameraLookup;           // 0x118
        M2Array  ribbonEmitters;         // 0x120
        M2Array  particleEmitters;       // 0x128
        M2Array  textureCombinerCombos;  // 0x130  (only if globalFlags & kFlagUseTextureCombinerCombos)

        uint8_t*       base()       { return reinterpret_cast<uint8_t*>(this); }
        const uint8_t* base() const { return reinterpret_cast<const uint8_t*>(this); }
    };

    struct M2Sequence
    {
        uint16_t id;             // 0x00
        uint16_t variationIndex; // 0x02
        uint32_t duration;       // 0x04
        float    movespeed;      // 0x08
        uint32_t flags;          // 0x0C  bit 0x20 = embedded
        int16_t  frequency;      // 0x10
        uint16_t _pad12;         // 0x12
        uint32_t replayMin;      // 0x14
        uint32_t replayMax;      // 0x18
        uint32_t blendTime;      // 0x1C
        uint8_t  _bounds[0x1C];  // 0x20
        int16_t  variationNext;  // 0x3C
        uint16_t aliasNext;      // 0x3E
    };

    struct M2Texture
    {
        uint32_t type;     // 0x00
        uint32_t flags;    // 0x04
        M2Array  filename; // 0x08
    };

    constexpr uint32_t kTexTypeHardcoded = 0;

    struct M2Batch
    {
        uint8_t  flags;                      // 0x00
        uint8_t  priorityPlane;              // 0x01
        uint16_t shaderId;                   // 0x02
        uint16_t skinSectionIndex;           // 0x04
        uint16_t geosetIndex;                // 0x06
        uint16_t colorIndex;                 // 0x08
        uint16_t materialIndex;              // 0x0A
        uint16_t materialLayer;              // 0x0C
        uint16_t textureCount;               // 0x0E
        uint16_t textureComboIndex;          // 0x10
        uint16_t textureCoordComboIndex;     // 0x12
        uint16_t textureWeightComboIndex;    // 0x14
        uint16_t textureTransformComboIndex; // 0x16
    };

    /// One skin submesh. level is the high half of indexStart in a skin past 65535 indices; the
    /// client reads both as its 32-bit m_skinSectionId and folds the high half in only when the
    /// window it names lies in the skin's index array (Engine/Source/Model2/M2Data.h).
    struct M2SkinSection
    {
        uint16_t skinSectionId;     // 0x00
        uint16_t level;             // 0x02
        uint16_t vertexStart;       // 0x04
        uint16_t vertexCount;       // 0x06
        uint16_t indexStart;        // 0x08
        uint16_t indexCount;        // 0x0A
        uint16_t boneCount;         // 0x0C
        uint16_t boneComboIndex;    // 0x0E
        uint16_t boneInfluences;    // 0x10
        uint16_t centerBoneIndex;   // 0x12
        float    centerPosition[3]; // 0x14
        float    sortCenterPos[3];  // 0x20
        float    sortRadius;        // 0x2C
    };

    /// The skin profile header: the client's M2SkinProfile.
    struct M2SkinHeader
    {
        uint32_t magic;        // 0x00 'SKIN'
        M2Array  vertices;     // 0x04 u16: the model vertex each skin vertex is
        M2Array  indices;      // 0x0C u16: triangles, into vertices
        M2Array  bones;        // 0x14 ubyte4 per skin vertex: into the submesh's boneCombos slice
        M2Array  submeshes;    // 0x1C M2SkinSection
        M2Array  batches;      // 0x24 M2Batch
        uint32_t boneCountMax; // 0x2C
    };

    struct M2Ribbon
    {
        uint32_t ribbonId;        // 0x00
        uint32_t boneIndex;       // 0x04
        float    position[3];     // 0x08
        M2Array  textureIndices;  // 0x14
        M2Array  materialIndices; // 0x1C
        uint8_t  _tracks[0x8C];   // 0x24
    };

    struct M2TrackHeader
    {
        uint16_t interpolationType; // 0x00
        int16_t  globalSequence;    // 0x02
        M2Array  timestamps;        // 0x04
        M2Array  values;            // 0x0C
    };

    struct M2CompBone
    {
        int32_t       boneId;     // 0x00
        uint32_t      flags;      // 0x04
        int16_t       parentBone; // 0x08
        uint16_t      submeshId;  // 0x0A
        uint16_t      unk1;       // 0x0C
        uint16_t      unk2;       // 0x0E
        M2TrackHeader translation;
        M2TrackHeader rotation;
        M2TrackHeader scale;
        float         pivot[3];   // 0x4C
    };

    struct M2CameraBody { uint8_t trackData[0x54]; };

    struct M2Camera
    {
        uint32_t     type;     // 0x00
        float        fov;      // 0x04
        float        farClip;  // 0x08
        float        nearClip; // 0x0C
        M2CameraBody body;     // 0x10
    };

#pragma pack(pop)

    static_assert(sizeof(M2Array) == 8, "M2Array");
    static_assert(offsetof(M2Header, cameras) == 0x110, "cameras");
    static_assert(offsetof(M2Header, particleEmitters) == 0x128, "particleEmitters");
    static_assert(offsetof(M2Header, textureCombinerCombos) == 0x130, "textureCombinerCombos");
    static_assert(sizeof(M2Sequence) == 0x40, "M2Sequence");
    static_assert(sizeof(M2Texture) == 0x10, "M2Texture");
    static_assert(sizeof(M2Batch) == 0x18, "M2Batch");
    static_assert(sizeof(M2SkinSection) == 0x30, "M2SkinSection");
    static_assert(sizeof(M2SkinHeader) == 0x30, "M2SkinHeader");
    static_assert(sizeof(M2CompBone) == 0x58, "M2CompBone");
    static_assert(sizeof(M2Ribbon) == 0xB0, "M2Ribbon");
    static_assert(sizeof(M2Camera) == 0x64, "M2Camera");
}

/// Strides that differ between the client's records and the modern ones (wxl-core v1.1
/// offsets/game/M2.hpp; the same numbers hold for the x64 client, whose records are the file's).
namespace wxl::offsets::game::m2
{
    constexpr uint32_t kParticleStrideClient = 0x1DC; // v264
    constexpr uint32_t kParticleStrideModern = 0x1EC; // v272-274
    constexpr uint32_t kParticleModernMinVer = 272;
    constexpr uint32_t kParticleFlagMultiTex = 0x10000000;

    constexpr uint32_t kCameraStrideClient = 0x64; // v264
    constexpr uint32_t kCameraStrideModern = 0x74; // v272-274
    constexpr uint32_t kCameraModernMinVer = 272;
}

/// The modern inner-version window the reader accepts (wxl-core v1.1 Contract.hpp).
namespace wxl::modern::assets::m2
{
    constexpr uint32_t kSourceVersionMin = 272;
    constexpr uint32_t kSourceVersionMax = 274;
}
