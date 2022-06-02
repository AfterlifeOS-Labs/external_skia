/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkCombinationBuilder_DEFINED
#define SkCombinationBuilder_DEFINED

#include <functional>
#include <memory>
#include <vector>
#include "include/core/SkBlendMode.h"
#include "include/core/SkSpan.h"
#include "include/core/SkTileMode.h"
#include "include/private/SkTArray.h"
#include "include/private/SkTHash.h"

class SkArenaAllocWithReset;
class SkCombinationBuilder;
class SkOption;
class SkPaintParamsKeyBuilder;
class SkShaderCodeDictionary;
class SkUniquePaintParamsID;

namespace skgpu::graphite {
class Context;
}

enum class SkShaderType : uint32_t {
    kSolidColor,

    kLinearGradient,
    kRadialGradient,
    kSweepGradient,
    kConicalGradient,

    kLocalMatrix,
    kImage,
    kBlendShader,

    kLast          = kBlendShader
};

static constexpr int kSkShaderTypeCount = static_cast<int>(SkShaderType::kLast) + 1;

struct SkTileModePair {
    SkTileMode fX;
    SkTileMode fY;

    bool operator==(const SkTileModePair& other) const { return fX == other.fX && fY == other.fY; }
    bool operator!=(const SkTileModePair& other) const { return !(*this == other); }
};

// TODO: add SkShaderID and SkColorFilterID too
class SkBlenderID {
public:
    SkBlenderID() : fID(0) {}  // 0 is an invalid blender ID
    SkBlenderID(const SkBlenderID& src) : fID(src.fID) {}

    bool isValid() const { return fID > 0; }

    bool operator==(const SkBlenderID& other) const { return fID == other.fID; }

    SkBlenderID& operator=(const SkBlenderID& src) {
        fID = src.fID;
        return *this;
    }

private:
    friend class SkShaderCodeDictionary;     // for ctor and asUInt access
    friend class SkCombinationBuilder;       // for asUInt access

    SkBlenderID(uint32_t id) : fID(id) {}

    uint32_t asUInt() const { return fID; }

    uint32_t fID;
};

// When combination options are added to the combination builder an SkCombinationOption
// object is frequently returned. This allows options to be added, recursively, to the
// previously added options.
// Note: SkCombinationOptions are stable memory-wise so, once returned, they are valid
// until SkCombinationBuilder::reset is called.
class SkCombinationOption {
public:
    SkCombinationOption addChildOption(int childIndex, SkShaderType);

    SkCombinationOption addChildOption(int childIndex, SkShaderType,
                                       int minNumStops, int maxNumStops);

    SkCombinationOption addChildOption(int childIndex, SkShaderType,
                                       SkSpan<SkTileModePair> tileModes);

    bool isValid() const { return fDataInArena; }
    SkShaderType type() const;
    int numChildSlots() const;

private:
    friend class SkCombinationBuilder; // for ctor

    SkCombinationOption(SkCombinationBuilder* builder, SkOption* dataInArena)
            : fBuilder(builder)
            , fDataInArena(dataInArena) {}

    SkCombinationBuilder* fBuilder;
    SkOption* fDataInArena;
};

class SkCombinationBuilder {
public:
    enum class BlendModeGroup {
        kPorterDuff,         // [ kClear .. kScreen ]
        kAdvanced,           // [ kOverlay .. kMultiply ]
        kColorAware,         // [ kHue .. kLuminosity ]
        kAll
    };

#ifdef SK_GRAPHITE_ENABLED
    SkCombinationBuilder(skgpu::graphite::Context*);
#else
    SkCombinationBuilder(SkShaderCodeDictionary*);
#endif

    // Blend Modes
    void addOption(SkBlendMode);
    void addOption(SkBlendMode rangeStart, SkBlendMode rangeEnd); // inclusive
    void addOption(BlendModeGroup);

    // TODO: have this variant return an SkCombinationOption object
    void addOption(SkBlenderID);

    // Shaders
    SkCombinationOption addOption(SkShaderType);
    SkCombinationOption addOption(SkShaderType, int minNumStops, int maxNumStops);  // inclusive
    SkCombinationOption addOption(SkShaderType, SkSpan<SkTileModePair> tileModes);

    void reset();

    int numCombinations();

#ifdef SK_DEBUG
    void dump() const;
#endif

private:
    friend class skgpu::graphite::Context; // for access to buildCombinations
    friend class SkCombinationOption;      // for 'addOptionInternal' and 'arena'

    SkArenaAllocWithReset* arena() { return fArena.get(); }

    template<typename T, typename... Args>
    SkOption* allocInArena(Args&&... args);

    SkOption* addOptionInternal(SkShaderType);
    SkOption* addOptionInternal(SkShaderType, int minNumStops, int maxNumStops);
    SkOption* addOptionInternal(SkShaderType, SkSpan<SkTileModePair> tileModes);

    void buildCombinations(SkShaderCodeDictionary*,
                           const std::function<void(SkUniquePaintParamsID)>&) const;

    SkShaderCodeDictionary* fDictionary;
    std::unique_ptr<SkArenaAllocWithReset> fArena;
    SkTArray<SkOption*> fShaderOptions;

    // TODO: store the SkBlender-based blenders in the arena
    SkTHashSet<uint32_t> fBlendModes;
};

#endif // SkCombinationBuilder_DEFINED
