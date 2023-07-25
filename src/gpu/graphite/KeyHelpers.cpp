/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/graphite/KeyHelpers.h"

#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/effects/SkRuntimeEffect.h"
#include "src/core/SkBlendModeBlender.h"
#include "src/core/SkBlenderBase.h"
#include "src/core/SkColorSpacePriv.h"
#include "src/core/SkDebugUtils.h"
#include "src/core/SkRuntimeBlender.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/effects/colorfilters/SkBlendModeColorFilter.h"
#include "src/effects/colorfilters/SkColorFilterBase.h"
#include "src/effects/colorfilters/SkColorSpaceXformColorFilter.h"
#include "src/effects/colorfilters/SkComposeColorFilter.h"
#include "src/effects/colorfilters/SkGaussianColorFilter.h"
#include "src/effects/colorfilters/SkMatrixColorFilter.h"
#include "src/effects/colorfilters/SkRuntimeColorFilter.h"
#include "src/effects/colorfilters/SkTableColorFilter.h"
#include "src/effects/colorfilters/SkWorkingFormatColorFilter.h"
#include "src/gpu/Blend.h"
#include "src/gpu/DitherUtils.h"
#include "src/gpu/graphite/KeyContext.h"
#include "src/gpu/graphite/Log.h"
#include "src/gpu/graphite/PaintParamsKey.h"
#include "src/gpu/graphite/PipelineData.h"
#include "src/gpu/graphite/ReadSwizzle.h"
#include "src/gpu/graphite/RecorderPriv.h"
#include "src/gpu/graphite/ResourceProvider.h"
#include "src/gpu/graphite/RuntimeEffectDictionary.h"
#include "src/gpu/graphite/ShaderCodeDictionary.h"
#include "src/gpu/graphite/Texture.h"
#include "src/gpu/graphite/TextureProxy.h"
#include "src/gpu/graphite/TextureProxyView.h"
#include "src/gpu/graphite/Uniform.h"
#include "src/gpu/graphite/UniformManager.h"
#include "src/image/SkImage_Base.h"
#include "src/shaders/SkImageShader.h"

constexpr SkPMColor4f kErrorColor = { 1, 0, 0, 1 };

#define VALIDATE_UNIFORMS(gatherer, dict, codeSnippetID) \
    SkDEBUGCODE(UniformExpectationsValidator uev(gatherer, dict->getUniforms(codeSnippetID));)

namespace skgpu::graphite {

//--------------------------------------------------------------------------------------------------

void PriorOutputBlock::BeginBlock(const KeyContext& keyContext,
                                  PaintParamsKeyBuilder* builder,
                                  PipelineDataGatherer* gatherer) {
    builder->beginBlock(BuiltInCodeSnippetID::kPriorOutput);
}

//--------------------------------------------------------------------------------------------------

namespace {

void add_solid_uniform_data(const ShaderCodeDictionary* dict,
                            const SkPMColor4f& premulColor,
                            PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kSolidColorShader)
    gatherer->write(premulColor);
}

} // anonymous namespace

void SolidColorShaderBlock::BeginBlock(const KeyContext& keyContext,
                                       PaintParamsKeyBuilder* builder,
                                       PipelineDataGatherer* gatherer,
                                       const SkPMColor4f& premulColor) {
    if (gatherer) {
        auto dict = keyContext.dict();

        add_solid_uniform_data(dict, premulColor, gatherer);
    }

    builder->beginBlock(BuiltInCodeSnippetID::kSolidColorShader);
}

//--------------------------------------------------------------------------------------------------

namespace {

void add_dst_read_sample_uniform_data(const ShaderCodeDictionary* dict,
                                      PipelineDataGatherer* gatherer,
                                      sk_sp<TextureProxy> dstTexture,
                                      SkIPoint dstOffset) {
    static const SkTileMode kTileModes[2] = {SkTileMode::kClamp, SkTileMode::kClamp};
    gatherer->add(SkSamplingOptions(), kTileModes, dstTexture);

    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kDstReadSample)

    SkV4 coords{static_cast<float>(dstOffset.x()),
                static_cast<float>(dstOffset.y()),
                1.0f / dstTexture->dimensions().width(),
                1.0f / dstTexture->dimensions().height()};
    gatherer->write(coords);
}

} // anonymous namespace

void DstReadSampleBlock::BeginBlock(const KeyContext& keyContext,
                                    PaintParamsKeyBuilder* builder,
                                    PipelineDataGatherer* gatherer,
                                    sk_sp<TextureProxy> dstTexture,
                                    SkIPoint dstOffset) {
    if (gatherer) {
        add_dst_read_sample_uniform_data(
                keyContext.dict(), gatherer, std::move(dstTexture), dstOffset);
    }
    builder->beginBlock(BuiltInCodeSnippetID::kDstReadSample);
}

void DstReadFetchBlock::BeginBlock(const KeyContext& keyContext,
                                   PaintParamsKeyBuilder* builder,
                                   PipelineDataGatherer* gatherer) {
    if (gatherer) {
        VALIDATE_UNIFORMS(gatherer, keyContext.dict(), BuiltInCodeSnippetID::kDstReadFetch)
    }
    builder->beginBlock(BuiltInCodeSnippetID::kDstReadFetch);
}

//--------------------------------------------------------------------------------------------------

namespace {

void add_gradient_preamble(const GradientShaderBlocks::GradientData& gradData,
                           PipelineDataGatherer* gatherer) {
    constexpr int kInternalStopLimit = GradientShaderBlocks::GradientData::kNumInternalStorageStops;

    if (gradData.fNumStops <= kInternalStopLimit) {
        if (gradData.fNumStops <= 4) {
            // Round up to 4 stops.
            gatherer->writeArray({gradData.fColors, 4});
            // The offsets are packed into a single float4 to save space.
            gatherer->write(SkSLType::kFloat4, &gradData.fOffsets);
        } else if (gradData.fNumStops <= 8) {
            // Round up to 8 stops.
            gatherer->writeArray({gradData.fColors, 8});
            // The offsets are packed into a float4 array to save space.
            gatherer->writeArray(SkSLType::kFloat4, &gradData.fOffsets, 2);
        } else {
            // Did kNumInternalStorageStops change?
            SkUNREACHABLE;
        }
    }
}

// All the gradients share a common postamble of:
//   numStops - for texture-based gradients
//   tilemode
//   colorSpace
//   doUnPremul
void add_gradient_postamble(const GradientShaderBlocks::GradientData& gradData,
                            PipelineDataGatherer* gatherer) {
    using ColorSpace = SkGradientShader::Interpolation::ColorSpace;

    constexpr int kInternalStopLimit = GradientShaderBlocks::GradientData::kNumInternalStorageStops;

    static_assert(static_cast<int>(ColorSpace::kLab)   == 2);
    static_assert(static_cast<int>(ColorSpace::kOKLab) == 3);
    static_assert(static_cast<int>(ColorSpace::kLCH)   == 4);
    static_assert(static_cast<int>(ColorSpace::kOKLCH) == 5);
    static_assert(static_cast<int>(ColorSpace::kHSL)   == 7);
    static_assert(static_cast<int>(ColorSpace::kHWB)   == 8);

    bool inputPremul = static_cast<bool>(gradData.fInterpolation.fInPremul);

    if (gradData.fNumStops > kInternalStopLimit) {
        gatherer->write(gradData.fNumStops);
    }

    gatherer->write(static_cast<int>(gradData.fTM));
    gatherer->write(static_cast<int>(gradData.fInterpolation.fColorSpace));
    gatherer->write(static_cast<int>(inputPremul));
}

void add_linear_gradient_uniform_data(const ShaderCodeDictionary* dict,
                                      BuiltInCodeSnippetID codeSnippetID,
                                      const GradientShaderBlocks::GradientData& gradData,
                                      PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, codeSnippetID)

    add_gradient_preamble(gradData, gatherer);
    gatherer->write(gradData.fPoints[0]);
    gatherer->write(gradData.fPoints[1]);
    add_gradient_postamble(gradData, gatherer);
};

void add_radial_gradient_uniform_data(const ShaderCodeDictionary* dict,
                                      BuiltInCodeSnippetID codeSnippetID,
                                      const GradientShaderBlocks::GradientData& gradData,
                                      PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, codeSnippetID)

    add_gradient_preamble(gradData, gatherer);
    gatherer->write(gradData.fPoints[0]);
    gatherer->write(gradData.fRadii[0]);
    add_gradient_postamble(gradData, gatherer);
};

void add_sweep_gradient_uniform_data(const ShaderCodeDictionary* dict,
                                     BuiltInCodeSnippetID codeSnippetID,
                                     const GradientShaderBlocks::GradientData& gradData,
                                     PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, codeSnippetID)

    add_gradient_preamble(gradData, gatherer);
    gatherer->write(gradData.fPoints[0]);
    gatherer->write(gradData.fBias);
    gatherer->write(gradData.fScale);
    add_gradient_postamble(gradData, gatherer);
};

void add_conical_gradient_uniform_data(const ShaderCodeDictionary* dict,
                                       BuiltInCodeSnippetID codeSnippetID,
                                       const GradientShaderBlocks::GradientData& gradData,
                                       PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, codeSnippetID)

    add_gradient_preamble(gradData, gatherer);
    gatherer->write(gradData.fPoints[0]);
    gatherer->write(gradData.fPoints[1]);
    gatherer->write(gradData.fRadii[0]);
    gatherer->write(gradData.fRadii[1]);
    add_gradient_postamble(gradData, gatherer);
};

} // anonymous namespace

GradientShaderBlocks::GradientData::GradientData(SkShaderBase::GradientType type, int numStops)
        : fType(type)
        , fPoints{{0.0f, 0.0f}, {0.0f, 0.0f}}
        , fRadii{0.0f, 0.0f}
        , fBias(0.0f)
        , fScale(0.0f)
        , fTM(SkTileMode::kClamp)
        , fNumStops(numStops) {
    sk_bzero(fColors, sizeof(fColors));
    sk_bzero(fOffsets, sizeof(fOffsets));
}

GradientShaderBlocks::GradientData::GradientData(SkShaderBase::GradientType type,
                                                 SkPoint point0, SkPoint point1,
                                                 float radius0, float radius1,
                                                 float bias, float scale,
                                                 SkTileMode tm,
                                                 int numStops,
                                                 const SkPMColor4f* colors,
                                                 float* offsets,
                                                 sk_sp<TextureProxy> colorsAndOffsetsProxy,
                                                 const SkGradientShader::Interpolation& interp)
        : fType(type)
        , fBias(bias)
        , fScale(scale)
        , fTM(tm)
        , fNumStops(numStops)
        , fInterpolation(interp) {
    SkASSERT(fNumStops >= 1);

    fPoints[0] = point0;
    fPoints[1] = point1;
    fRadii[0] = radius0;
    fRadii[1] = radius1;

    if (fNumStops <= kNumInternalStorageStops) {
        memcpy(fColors, colors, fNumStops * sizeof(SkColor4f));
        if (offsets) {
            memcpy(fOffsets, offsets, fNumStops * sizeof(float));
        } else {
            for (int i = 0; i < fNumStops; ++i) {
                fOffsets[i] = SkIntToFloat(i) / (fNumStops-1);
            }
        }

        // Extend the colors and offset, if necessary, to fill out the arrays
        // TODO: this should be done later when the actual code snippet has been selected!!
        for (int i = fNumStops ; i < kNumInternalStorageStops; ++i) {
            fColors[i] = fColors[fNumStops-1];
            fOffsets[i] = fOffsets[fNumStops-1];
        }
    } else {
        fColorsAndOffsetsProxy = std::move(colorsAndOffsetsProxy);
        SkASSERT(fColorsAndOffsetsProxy);
    }
}

void GradientShaderBlocks::BeginBlock(const KeyContext& keyContext,
                                      PaintParamsKeyBuilder *builder,
                                      PipelineDataGatherer* gatherer,
                                      const GradientData& gradData) {
    auto dict = keyContext.dict();

    if (gradData.fNumStops > GradientData::kNumInternalStorageStops && gatherer) {
        SkASSERT(gradData.fColorsAndOffsetsProxy);

        static constexpr SkSamplingOptions kNearest(SkFilterMode::kNearest, SkMipmapMode::kNone);
        static constexpr SkTileMode kClampTiling[2] = {SkTileMode::kClamp, SkTileMode::kClamp};
        gatherer->add(kNearest, kClampTiling, gradData.fColorsAndOffsetsProxy);
    }

    BuiltInCodeSnippetID codeSnippetID = BuiltInCodeSnippetID::kSolidColorShader;
    switch (gradData.fType) {
        case SkShaderBase::GradientType::kLinear:
            codeSnippetID =
                    gradData.fNumStops <= 4 ? BuiltInCodeSnippetID::kLinearGradientShader4
                    : gradData.fNumStops <= 8 ? BuiltInCodeSnippetID::kLinearGradientShader8
                                              : BuiltInCodeSnippetID::kLinearGradientShaderTexture;
            if (gatherer) {
                add_linear_gradient_uniform_data(dict, codeSnippetID, gradData, gatherer);
            }
            break;
        case SkShaderBase::GradientType::kRadial:
            codeSnippetID =
                    gradData.fNumStops <= 4 ? BuiltInCodeSnippetID::kRadialGradientShader4
                    : gradData.fNumStops <= 8 ? BuiltInCodeSnippetID::kRadialGradientShader8
                                              : BuiltInCodeSnippetID::kRadialGradientShaderTexture;
            if (gatherer) {
                add_radial_gradient_uniform_data(dict, codeSnippetID, gradData, gatherer);
            }
            break;
        case SkShaderBase::GradientType::kSweep:
            codeSnippetID =
                    gradData.fNumStops <= 4 ? BuiltInCodeSnippetID::kSweepGradientShader4
                    : gradData.fNumStops <= 8 ? BuiltInCodeSnippetID::kSweepGradientShader8
                                              : BuiltInCodeSnippetID::kSweepGradientShaderTexture;
            if (gatherer) {
                add_sweep_gradient_uniform_data(dict, codeSnippetID, gradData, gatherer);
            }
            break;
        case SkShaderBase::GradientType::kConical:
            codeSnippetID =
                    gradData.fNumStops <= 4 ? BuiltInCodeSnippetID::kConicalGradientShader4
                    : gradData.fNumStops <= 8 ? BuiltInCodeSnippetID::kConicalGradientShader8
                                              : BuiltInCodeSnippetID::kConicalGradientShaderTexture;
            if (gatherer) {
                add_conical_gradient_uniform_data(dict, codeSnippetID, gradData, gatherer);
            }
            break;
        case SkShaderBase::GradientType::kNone:
        default:
            SkDEBUGFAIL("Expected a gradient shader, but it wasn't one.");
            break;
    }

    builder->beginBlock(codeSnippetID);
}

//--------------------------------------------------------------------------------------------------

namespace {

void add_localmatrixshader_uniform_data(const ShaderCodeDictionary* dict,
                                        const SkM44& localMatrix,
                                        PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kLocalMatrixShader)

    SkM44 lmInverse;
    bool wasInverted = localMatrix.invert(&lmInverse);  // TODO: handle failure up stack
    if (!wasInverted) {
        lmInverse.setIdentity();
    }

    gatherer->write(lmInverse);
}

} // anonymous namespace

void LocalMatrixShaderBlock::BeginBlock(const KeyContext& keyContext,
                                        PaintParamsKeyBuilder* builder,
                                        PipelineDataGatherer* gatherer,
                                        const LMShaderData* lmShaderData) {
    SkASSERT(!gatherer == !lmShaderData);

    auto dict = keyContext.dict();
    // When extracted into ShaderInfo::SnippetEntries the children will appear after their
    // parent. Thus, the parent's uniform data must appear in the uniform block before the
    // uniform data of the children.
    if (gatherer) {
        add_localmatrixshader_uniform_data(dict, lmShaderData->fLocalMatrix, gatherer);
    }

    builder->beginBlock(BuiltInCodeSnippetID::kLocalMatrixShader);
}

//--------------------------------------------------------------------------------------------------

namespace {

void add_color_space_uniforms(const SkColorSpaceXformSteps& steps, PipelineDataGatherer* gatherer) {
    static constexpr int kNumXferFnCoeffs = 7;
    static constexpr float kEmptyXferFn[kNumXferFnCoeffs] = {};

    gatherer->write(SkTo<int>(steps.flags.mask()));

    if (steps.flags.linearize) {
        gatherer->write(SkTo<int>(skcms_TransferFunction_getType(&steps.srcTF)));
        gatherer->writeHalfArray({&steps.srcTF.g, kNumXferFnCoeffs});
    } else {
        gatherer->write(SkTo<int>(skcms_TFType::skcms_TFType_Invalid));
        gatherer->writeHalfArray({kEmptyXferFn, kNumXferFnCoeffs});
    }

    SkMatrix gamutTransform;
    if (steps.flags.gamut_transform) {
        // TODO: it seems odd to copy this into an SkMatrix just to write it to the gatherer
        gamutTransform.set9(steps.src_to_dst_matrix);
    }
    gatherer->writeHalf(gamutTransform);

    if (steps.flags.encode) {
        gatherer->write(SkTo<int>(skcms_TransferFunction_getType(&steps.dstTFInv)));
        gatherer->writeHalfArray({&steps.dstTFInv.g, kNumXferFnCoeffs});
    } else {
        gatherer->write(SkTo<int>(skcms_TFType::skcms_TFType_Invalid));
        gatherer->writeHalfArray({kEmptyXferFn, kNumXferFnCoeffs});
    }
}

void add_image_uniform_data(const ShaderCodeDictionary* dict,
                            const ImageShaderBlock::ImageData& imgData,
                            PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kImageShader)

    gatherer->write(SkPoint::Make(imgData.fTextureProxy->dimensions().fWidth,
                                  imgData.fTextureProxy->dimensions().fHeight));
    gatherer->write(imgData.fSubset);
    gatherer->write(SkTo<int>(imgData.fTileModes[0]));
    gatherer->write(SkTo<int>(imgData.fTileModes[1]));
    gatherer->write(SkTo<int>(imgData.fSampling.filter));
    gatherer->write(imgData.fSampling.useCubic);
    if (imgData.fSampling.useCubic) {
        const SkCubicResampler& cubic = imgData.fSampling.cubic;
        gatherer->writeHalf(SkImageShader::CubicResamplerMatrix(cubic.B, cubic.C));
    } else {
        gatherer->writeHalf(SkM44());
    }
    gatherer->write(SkTo<int>(imgData.fReadSwizzle));

    add_color_space_uniforms(imgData.fSteps, gatherer);
}

} // anonymous namespace

ImageShaderBlock::ImageData::ImageData(const SkSamplingOptions& sampling,
                                       SkTileMode tileModeX,
                                       SkTileMode tileModeY,
                                       SkRect subset,
                                       ReadSwizzle readSwizzle)
        : fSampling(sampling)
        , fTileModes{tileModeX, tileModeY}
        , fSubset(subset)
        , fReadSwizzle(readSwizzle) {
    SkASSERT(fSteps.flags.mask() == 0);   // By default, the colorspace should have no effect
}

void ImageShaderBlock::BeginBlock(const KeyContext& keyContext,
                                  PaintParamsKeyBuilder* builder,
                                  PipelineDataGatherer* gatherer,
                                  const ImageData* imgData) {
    SkASSERT(!gatherer == !imgData);

    // TODO: allow through lazy proxies
    if (gatherer && !imgData->fTextureProxy) {
        // TODO: At some point the pre-compile path should also be creating a texture
        // proxy (i.e., we can remove the 'pipelineData' in the above test).
        SolidColorShaderBlock::BeginBlock(keyContext, builder, gatherer, kErrorColor);
        return;
    }

    auto dict = keyContext.dict();
    if (gatherer) {
        gatherer->add(imgData->fSampling,
                      imgData->fTileModes,
                      imgData->fTextureProxy);

        add_image_uniform_data(dict, *imgData, gatherer);
    }

    builder->beginBlock(BuiltInCodeSnippetID::kImageShader);
}

//--------------------------------------------------------------------------------------------------

// makes use of ImageShader functions, above
namespace {

void add_yuv_image_uniform_data(const ShaderCodeDictionary* dict,
                                const YUVImageShaderBlock::ImageData& imgData,
                                PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kYUVImageShader)

    gatherer->write(imgData.fImgSize);
    gatherer->write(imgData.fSubset);
    gatherer->write(SkTo<int>(imgData.fTileModes[0]));
    gatherer->write(SkTo<int>(imgData.fTileModes[1]));
    gatherer->write(SkTo<int>(imgData.fSampling.filter));
    gatherer->write(imgData.fSampling.useCubic);
    if (imgData.fSampling.useCubic) {
        const SkCubicResampler& cubic = imgData.fSampling.cubic;
        gatherer->writeHalf(SkImageShader::CubicResamplerMatrix(cubic.B, cubic.C));
    } else {
        gatherer->writeHalf(SkM44());
    }

    for (int i = 0; i < 4; ++i) {
        gatherer->writeHalf(imgData.fChannelSelect[i]);
    }
    gatherer->writeHalf(imgData.fYUVtoRGBMatrix);
    gatherer->write(imgData.fYUVtoRGBTranslate);

    add_color_space_uniforms(imgData.fSteps, gatherer);
}

} // anonymous namespace

YUVImageShaderBlock::ImageData::ImageData(const SkSamplingOptions& sampling,
                                          SkTileMode tileModeX,
                                          SkTileMode tileModeY,
                                          SkRect subset)
        : fSampling(sampling)
        , fTileModes{tileModeX, tileModeY}
        , fSubset(subset) {
    SkASSERT(fSteps.flags.mask() == 0);   // By default, the colorspace should have no effect
}

void YUVImageShaderBlock::BeginBlock(const KeyContext& keyContext,
                                     PaintParamsKeyBuilder* builder,
                                     PipelineDataGatherer* gatherer,
                                     const ImageData* imgData) {
    SkASSERT(!gatherer == !imgData);

    // TODO: allow through lazy proxies
    if (gatherer &&
        (!imgData->fTextureProxies[0] || !imgData->fTextureProxies[1] ||
         !imgData->fTextureProxies[2] || !imgData->fTextureProxies[3])) {
        // TODO: At some point the pre-compile path should also be creating a texture
        // proxy (i.e., we can remove the 'pipelineData' in the above test).
        SolidColorShaderBlock::BeginBlock(keyContext, builder, gatherer, kErrorColor);
        return;
    }

    auto dict = keyContext.dict();
    if (gatherer) {
        for (int i = 0; i < 4; ++i) {
            gatherer->add(imgData->fSampling,
                          imgData->fTileModes,
                          imgData->fTextureProxies[i]);
        }

        add_yuv_image_uniform_data(dict, *imgData, gatherer);
    }

    builder->beginBlock(BuiltInCodeSnippetID::kYUVImageShader);
}

//--------------------------------------------------------------------------------------------------

namespace {

void add_coordclamp_uniform_data(const ShaderCodeDictionary* dict,
                                 const CoordClampShaderBlock::CoordClampData& clampData,
                                 PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kCoordClampShader)

    gatherer->write(clampData.fSubset);
}

} // anonymous namespace

void CoordClampShaderBlock::BeginBlock(const KeyContext& keyContext,
                                       PaintParamsKeyBuilder* builder,
                                       PipelineDataGatherer* gatherer,
                                       const CoordClampData* clampData) {
    SkASSERT(!gatherer == !clampData);

    auto dict = keyContext.dict();
    if (gatherer) {
        add_coordclamp_uniform_data(dict, *clampData, gatherer);
    }

    builder->beginBlock(BuiltInCodeSnippetID::kCoordClampShader);
}

//--------------------------------------------------------------------------------------------------

namespace {

void add_dither_uniform_data(const ShaderCodeDictionary* dict,
                             const DitherShaderBlock::DitherData& ditherData,
                             PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kDitherShader)

    gatherer->writeHalf(ditherData.fRange);
}

} // anonymous namespace

void DitherShaderBlock::BeginBlock(const KeyContext& keyContext,
                                   PaintParamsKeyBuilder* builder,
                                   PipelineDataGatherer* gatherer,
                                   const DitherData* ditherData) {
    SkASSERT(!gatherer == !ditherData);

    auto dict = keyContext.dict();
    if (gatherer) {
        static const SkBitmap gLUT = skgpu::MakeDitherLUT();

        sk_sp<TextureProxy> proxy = RecorderPriv::CreateCachedProxy(keyContext.recorder(), gLUT);
        if (!proxy) {
            SKGPU_LOG_W("Couldn't create dither shader's LUT");

            PriorOutputBlock::BeginBlock(keyContext, builder, gatherer);
            return;
        }

        add_dither_uniform_data(dict, *ditherData, gatherer);

        static constexpr SkSamplingOptions kNearest(SkFilterMode::kNearest, SkMipmapMode::kNone);
        static constexpr SkTileMode kRepeatTiling[2] = { SkTileMode::kRepeat, SkTileMode::kRepeat };

        gatherer->add(kNearest, kRepeatTiling, std::move(proxy));
    }

    builder->beginBlock(BuiltInCodeSnippetID::kDitherShader);
}

//--------------------------------------------------------------------------------------------------

namespace {

void add_perlin_noise_uniform_data(const ShaderCodeDictionary* dict,
                                   const PerlinNoiseShaderBlock::PerlinNoiseData& noiseData,
                                   PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kPerlinNoiseShader)

    gatherer->write(noiseData.fBaseFrequency);
    gatherer->write(noiseData.fStitchData);
    gatherer->write(static_cast<int>(noiseData.fType));
    gatherer->write(noiseData.fNumOctaves);
    gatherer->write(static_cast<int>(noiseData.stitching()));

    static const SkTileMode kRepeatXTileModes[2] = { SkTileMode::kRepeat, SkTileMode::kClamp };
    static const SkSamplingOptions kNearestSampling { SkFilterMode::kNearest };

    gatherer->add(kNearestSampling, kRepeatXTileModes, noiseData.fPermutationsProxy);
    gatherer->add(kNearestSampling, kRepeatXTileModes, noiseData.fNoiseProxy);
}

} // anonymous namespace

void PerlinNoiseShaderBlock::BeginBlock(const KeyContext& keyContext,
                                        PaintParamsKeyBuilder* builder,
                                        PipelineDataGatherer* gatherer,
                                        const PerlinNoiseData* noiseData) {
    SkASSERT(!gatherer == !noiseData);

    auto dict = keyContext.dict();
    if (gatherer) {
        add_perlin_noise_uniform_data(dict, *noiseData, gatherer);
    }

    builder->beginBlock(BuiltInCodeSnippetID::kPerlinNoiseShader);
}

//--------------------------------------------------------------------------------------------------

void BlendShaderBlock::BeginBlock(const KeyContext& keyContext,
                                  PaintParamsKeyBuilder* builder,
                                  PipelineDataGatherer* gatherer) {
    if (gatherer) {
        VALIDATE_UNIFORMS(gatherer, keyContext.dict(), BuiltInCodeSnippetID::kBlendShader)
    }

    builder->beginBlock(BuiltInCodeSnippetID::kBlendShader);
}

//--------------------------------------------------------------------------------------------------

void BlendModeBlenderBlock::BeginBlock(const KeyContext& keyContext,
                                       PaintParamsKeyBuilder* builder,
                                       PipelineDataGatherer* gatherer,
                                       SkBlendMode blendMode) {
    if (gatherer) {
        VALIDATE_UNIFORMS(gatherer, keyContext.dict(), BuiltInCodeSnippetID::kBlendModeBlender)
        gatherer->write(SkTo<int>(blendMode));
    }

    builder->beginBlock(BuiltInCodeSnippetID::kBlendModeBlender);
}

//--------------------------------------------------------------------------------------------------

void CoeffBlenderBlock::BeginBlock(const KeyContext& keyContext,
                                   PaintParamsKeyBuilder* builder,
                                   PipelineDataGatherer* gatherer,
                                   SkSpan<const float> coeffs) {
    if (gatherer) {
        VALIDATE_UNIFORMS(gatherer, keyContext.dict(), BuiltInCodeSnippetID::kCoeffBlender)
        SkASSERT(coeffs.size() == 4);
        gatherer->write(SkSLType::kHalf4, coeffs.data());
    }

    builder->beginBlock(BuiltInCodeSnippetID::kCoeffBlender);
}

//--------------------------------------------------------------------------------------------------

void DstColorBlock::BeginBlock(const KeyContext& keyContext,
                               PaintParamsKeyBuilder* builder,
                               PipelineDataGatherer* gatherer) {
    if (gatherer) {
        VALIDATE_UNIFORMS(gatherer, keyContext.dict(), BuiltInCodeSnippetID::kDstColor)
    }

    builder->beginBlock(BuiltInCodeSnippetID::kDstColor);
}

void PrimitiveColorBlock::BeginBlock(const KeyContext& keyContext,
                                     PaintParamsKeyBuilder* builder,
                                     PipelineDataGatherer* gatherer) {
    if (gatherer) {
        VALIDATE_UNIFORMS(gatherer, keyContext.dict(), BuiltInCodeSnippetID::kPrimitiveColor)
    }

    builder->beginBlock(BuiltInCodeSnippetID::kPrimitiveColor);
}

//--------------------------------------------------------------------------------------------------

void ColorFilterShaderBlock::BeginBlock(const KeyContext& keyContext,
                                        PaintParamsKeyBuilder* builder,
                                        PipelineDataGatherer* gatherer) {
    builder->beginBlock(BuiltInCodeSnippetID::kColorFilterShader);
}

//--------------------------------------------------------------------------------------------------

namespace {

void add_matrix_colorfilter_uniform_data(const ShaderCodeDictionary* dict,
                                         const MatrixColorFilterBlock::MatrixColorFilterData& data,
                                         PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kMatrixColorFilter)
    gatherer->write(data.fMatrix);
    gatherer->write(data.fTranslate);
    gatherer->write(static_cast<int>(data.fInHSLA));
}

} // anonymous namespace

void MatrixColorFilterBlock::BeginBlock(const KeyContext& keyContext,
                                        PaintParamsKeyBuilder* builder,
                                        PipelineDataGatherer* gatherer,
                                        const MatrixColorFilterData* matrixCFData) {
    SkASSERT(!gatherer == !matrixCFData);

    auto dict = keyContext.dict();

    if (gatherer) {
        add_matrix_colorfilter_uniform_data(dict, *matrixCFData, gatherer);
    }

    builder->beginBlock(BuiltInCodeSnippetID::kMatrixColorFilter);
}

//--------------------------------------------------------------------------------------------------
void ComposeColorFilterBlock::BeginBlock(const KeyContext& keyContext,
                                         PaintParamsKeyBuilder* builder,
                                         PipelineDataGatherer* gatherer) {
    builder->beginBlock(BuiltInCodeSnippetID::kComposeColorFilter);
}

//--------------------------------------------------------------------------------------------------
void GaussianColorFilterBlock::BeginBlock(const KeyContext& keyContext,
                                          PaintParamsKeyBuilder* builder,
                                          PipelineDataGatherer* gatherer) {
    builder->beginBlock(BuiltInCodeSnippetID::kGaussianColorFilter);
}

//--------------------------------------------------------------------------------------------------

namespace {

void add_table_colorfilter_uniform_data(const ShaderCodeDictionary* dict,
                                        const TableColorFilterBlock::TableColorFilterData& data,
                                        PipelineDataGatherer* gatherer) {
    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kTableColorFilter)

    static const SkTileMode kTileModes[2] = { SkTileMode::kClamp, SkTileMode::kClamp };
    gatherer->add(SkSamplingOptions(), kTileModes, data.fTextureProxy);
}

} // anonymous namespace

void TableColorFilterBlock::BeginBlock(const KeyContext& keyContext,
                                       PaintParamsKeyBuilder* builder,
                                       PipelineDataGatherer* gatherer,
                                       const TableColorFilterData& data) {
    auto dict = keyContext.dict();

    if (gatherer) {
        if (!data.fTextureProxy) {
            // We're dropping the color filter here!
            PriorOutputBlock::BeginBlock(keyContext, builder, gatherer);
            return;
        }

        add_table_colorfilter_uniform_data(dict, data, gatherer);
    }

    builder->beginBlock(BuiltInCodeSnippetID::kTableColorFilter);
}

//--------------------------------------------------------------------------------------------------
namespace {

void add_color_space_xform_uniform_data(
        const ShaderCodeDictionary* dict,
        const ColorSpaceTransformBlock::ColorSpaceTransformData* data,
        PipelineDataGatherer* gatherer) {

    VALIDATE_UNIFORMS(gatherer, dict, BuiltInCodeSnippetID::kColorSpaceXformColorFilter)
    add_color_space_uniforms(data->fSteps, gatherer);
}

}  // anonymous namespace

ColorSpaceTransformBlock::ColorSpaceTransformData::ColorSpaceTransformData(const SkColorSpace* src,
                                                                           SkAlphaType srcAT,
                                                                           const SkColorSpace* dst,
                                                                           SkAlphaType dstAT)
        : fSteps(src, srcAT, dst, dstAT) {}

void ColorSpaceTransformBlock::BeginBlock(const KeyContext& keyContext,
                                          PaintParamsKeyBuilder* builder,
                                          PipelineDataGatherer* gatherer,
                                          const ColorSpaceTransformData* data) {
    if (gatherer) {
        add_color_space_xform_uniform_data(keyContext.dict(), data, gatherer);
    }
    builder->beginBlock(BuiltInCodeSnippetID::kColorSpaceXformColorFilter);
}

//--------------------------------------------------------------------------------------------------

void AddDstBlendBlock(const KeyContext& keyContext,
                      PaintParamsKeyBuilder* builder,
                      PipelineDataGatherer* gatherer,
                      const SkBlender* blender) {
    BlendShaderBlock::BeginBlock(keyContext, builder, gatherer);

    // src -- prior output
    PriorOutputBlock::BeginBlock(keyContext, builder, gatherer);
    builder->endBlock();
    // dst -- surface color
    DstColorBlock::BeginBlock(keyContext, builder, gatherer);
    builder->endBlock();
    // blender -- shader based blending
    AddToKey(keyContext, builder, gatherer, blender);

    builder->endBlock();  // BlendShaderBlock
}

void AddPrimitiveBlendBlock(const KeyContext& keyContext,
                            PaintParamsKeyBuilder* builder,
                            PipelineDataGatherer* gatherer,
                            const SkBlender* blender) {
    BlendShaderBlock::BeginBlock(keyContext, builder, gatherer);

    // src -- prior output
    PriorOutputBlock::BeginBlock(keyContext, builder, gatherer);
    builder->endBlock();
    // dst -- primitive color
    PrimitiveColorBlock::BeginBlock(keyContext, builder, gatherer);
    builder->endBlock();
    // blender -- shader based blending
    AddToKey(keyContext, builder, gatherer, blender);

    builder->endBlock();  // BlendShaderBlock
}

void AddColorBlendBlock(const KeyContext& keyContext,
                        PaintParamsKeyBuilder* builder,
                        PipelineDataGatherer* gatherer,
                        SkBlendMode bm,
                        const SkPMColor4f& srcColor) {
    BlendShaderBlock::BeginBlock(keyContext, builder, gatherer);

    // src -- solid color
    SolidColorShaderBlock::BeginBlock(keyContext, builder, gatherer, srcColor);
    builder->endBlock();
    // dst -- prior output
    PriorOutputBlock::BeginBlock(keyContext, builder, gatherer);
    builder->endBlock();
    // blender -- shader based blending
    BlendModeBlenderBlock::BeginBlock(keyContext, builder, gatherer, bm);
    builder->endBlock();

    builder->endBlock();  // BlendShaderBlock
}

RuntimeEffectBlock::ShaderData::ShaderData(sk_sp<const SkRuntimeEffect> effect)
        : fEffect(std::move(effect)) {}

RuntimeEffectBlock::ShaderData::ShaderData(sk_sp<const SkRuntimeEffect> effect,
                                           sk_sp<const SkData> uniforms)
        : fEffect(std::move(effect))
        , fUniforms(std::move(uniforms)) {}

static bool skdata_matches(const SkData* a, const SkData* b) {
    // Returns true if both SkData objects hold the same contents, or if they are both null.
    // (SkData::equals supports passing null, and returns false.)
    return a ? a->equals(b) : (a == b);
}

bool RuntimeEffectBlock::ShaderData::operator==(const ShaderData& rhs) const {
    return fEffect == rhs.fEffect && skdata_matches(fUniforms.get(), rhs.fUniforms.get());
}

static void gather_runtime_effect_uniforms(SkSpan<const SkRuntimeEffect::Uniform> rtsUniforms,
                                           SkSpan<const Uniform> graphiteUniforms,
                                           const SkData* uniformData,
                                           PipelineDataGatherer* gatherer) {
    // Collect all the other uniforms from the provided SkData.
    const uint8_t* uniformBase = uniformData->bytes();
    for (size_t index = 0; index < rtsUniforms.size(); ++index) {
        const Uniform& uniform = graphiteUniforms[index];
        // Get a pointer to the offset in our data for this uniform.
        const uint8_t* uniformPtr = uniformBase + rtsUniforms[index].offset;
        // Pass the uniform data to the gatherer.
        gatherer->write(uniform, uniformPtr);
    }
}

void RuntimeEffectBlock::BeginBlock(const KeyContext& keyContext,
                                    PaintParamsKeyBuilder* builder,
                                    PipelineDataGatherer* gatherer,
                                    const ShaderData& shaderData) {
    ShaderCodeDictionary* dict = keyContext.dict();
    int codeSnippetID = dict->findOrCreateRuntimeEffectSnippet(shaderData.fEffect.get());

    keyContext.rtEffectDict()->set(codeSnippetID, shaderData.fEffect);

    if (gatherer) {
        const ShaderSnippet* entry = dict->getEntry(codeSnippetID);
        SkASSERT(entry);

        SkDEBUGCODE(UniformExpectationsValidator uev(gatherer, entry->fUniforms);)

        gather_runtime_effect_uniforms(shaderData.fEffect->uniforms(),
                                       entry->fUniforms,
                                       shaderData.fUniforms.get(),
                                       gatherer);
    }

    builder->beginBlock(codeSnippetID);
}

// ==================================================================

static void add_to_key(const KeyContext& keyContext,
                       PaintParamsKeyBuilder* builder,
                       PipelineDataGatherer* gatherer,
                       const SkBlendModeBlender* blender) {
    SkASSERT(blender);
    SkSpan<const float> coeffs = skgpu::GetPorterDuffBlendConstants(blender->mode());
    if (!coeffs.empty()) {
        CoeffBlenderBlock::BeginBlock(keyContext, builder, gatherer, coeffs);
        builder->endBlock();
    } else {
        BlendModeBlenderBlock::BeginBlock(keyContext, builder, gatherer, blender->mode());
        builder->endBlock();
    }
}

static void add_to_key(const KeyContext& keyContext,
                       PaintParamsKeyBuilder* builder,
                       PipelineDataGatherer* gatherer,
                       const SkRuntimeBlender* blender) {
    SkASSERT(blender);
    sk_sp<SkRuntimeEffect> effect = blender->effect();
    SkASSERT(effect);
    sk_sp<const SkData> uniforms = SkRuntimeEffectPriv::TransformUniforms(
            effect->uniforms(),
            blender->uniforms(),
            keyContext.dstColorInfo().colorSpace());
    SkASSERT(uniforms);

    RuntimeEffectBlock::BeginBlock(keyContext, builder, gatherer,
                                   { effect, std::move(uniforms) });

    SkRuntimeEffectPriv::AddChildrenToKey(blender->children(), effect->children(), keyContext,
                                          builder, gatherer);

    builder->endBlock();
}

void AddToKey(const KeyContext& keyContext,
              PaintParamsKeyBuilder* builder,
              PipelineDataGatherer* gatherer,
              const SkBlender* blender) {
    if (!blender) {
        return;
    }
    switch (as_BB(blender)->type()) {
#define M(type)                                                    \
    case SkBlenderBase::BlenderType::k##type:                      \
        add_to_key(keyContext,                                     \
                   builder,                                        \
                   gatherer,                                       \
                   static_cast<const Sk##type##Blender*>(blender)); \
        return;
        SK_ALL_BLENDERS(M)
#undef M
    }
    SkUNREACHABLE;
}

static SkPMColor4f map_color(const SkColor4f& c, SkColorSpace* src, SkColorSpace* dst) {
    SkPMColor4f color = {c.fR, c.fG, c.fB, c.fA};
    SkColorSpaceXformSteps(src, kUnpremul_SkAlphaType, dst, kPremul_SkAlphaType).apply(color.vec());
    return color;
}
static void add_to_key(const KeyContext& keyContext,
                       PaintParamsKeyBuilder* builder,
                       PipelineDataGatherer* gatherer,
                       const SkBlendModeColorFilter* filter) {
    SkASSERT(filter);

    SkPMColor4f color =
            map_color(filter->color(), sk_srgb_singleton(), keyContext.dstColorInfo().colorSpace());
    AddColorBlendBlock(keyContext, builder, gatherer, filter->mode(), color);
}

static void add_to_key(const KeyContext& keyContext,
                       PaintParamsKeyBuilder* builder,
                       PipelineDataGatherer* gatherer,
                       const SkColorSpaceXformColorFilter* filter) {
    SkASSERT(filter);

    constexpr SkAlphaType alphaType = kPremul_SkAlphaType;
    ColorSpaceTransformBlock::ColorSpaceTransformData data(
            filter->src().get(), alphaType, filter->src().get(), alphaType);
    ColorSpaceTransformBlock::BeginBlock(keyContext, builder, gatherer, &data);
    builder->endBlock();
}

static void add_to_key(const KeyContext& keyContext,
                       PaintParamsKeyBuilder* builder,
                       PipelineDataGatherer* gatherer,
                       const SkComposeColorFilter* filter) {
    SkASSERT(filter);

    ComposeColorFilterBlock::BeginBlock(keyContext, builder, gatherer);

    AddToKey(keyContext, builder, gatherer, filter->inner().get());
    AddToKey(keyContext, builder, gatherer, filter->outer().get());

    builder->endBlock();
}

static void add_to_key(const KeyContext& keyContext,
                       PaintParamsKeyBuilder* builder,
                       PipelineDataGatherer* gatherer,
                       const SkGaussianColorFilter*) {
    GaussianColorFilterBlock::BeginBlock(keyContext, builder, gatherer);
    builder->endBlock();
}

static void add_to_key(const KeyContext& keyContext,
                       PaintParamsKeyBuilder* builder,
                       PipelineDataGatherer* gatherer,
                       const SkMatrixColorFilter* filter) {
    SkASSERT(filter);

    bool inHSLA = filter->domain() == SkMatrixColorFilter::Domain::kHSLA;
    MatrixColorFilterBlock::MatrixColorFilterData matrixCFData(filter->matrix(), inHSLA);

    MatrixColorFilterBlock::BeginBlock(keyContext, builder, gatherer, &matrixCFData);
    builder->endBlock();
}

static void add_to_key(const KeyContext& keyContext,
                       PaintParamsKeyBuilder* builder,
                       PipelineDataGatherer* gatherer,
                       const SkRuntimeColorFilter* filter) {
    SkASSERT(filter);

    sk_sp<SkRuntimeEffect> effect = filter->effect();
    sk_sp<const SkData> uniforms = SkRuntimeEffectPriv::TransformUniforms(
            effect->uniforms(), filter->uniforms(), keyContext.dstColorInfo().colorSpace());
    SkASSERT(uniforms);

    RuntimeEffectBlock::BeginBlock(keyContext, builder, gatherer, {effect, std::move(uniforms)});

    SkRuntimeEffectPriv::AddChildrenToKey(
            filter->children(), effect->children(), keyContext, builder, gatherer);

    builder->endBlock();
}

static void add_to_key(const KeyContext& keyContext,
                       PaintParamsKeyBuilder* builder,
                       PipelineDataGatherer* gatherer,
                       const SkTableColorFilter* filter) {
    SkASSERT(filter);

    sk_sp<TextureProxy> proxy = RecorderPriv::CreateCachedProxy(keyContext.recorder(),
                                                                filter->bitmap());
    if (!proxy) {
        SKGPU_LOG_W("Couldn't create TableColorFilter's table");

        // Return the input color as-is.
        PriorOutputBlock::BeginBlock(keyContext, builder, gatherer);
        builder->endBlock();
        return;
    }

    TableColorFilterBlock::TableColorFilterData data(std::move(proxy));

    TableColorFilterBlock::BeginBlock(keyContext, builder, gatherer, data);
    builder->endBlock();
}

static void add_to_key(const KeyContext& keyContext,
                       PaintParamsKeyBuilder* builder,
                       PipelineDataGatherer* gatherer,
                       const SkWorkingFormatColorFilter* filter) {
    SkASSERT(filter);

    const SkAlphaType dstAT = keyContext.dstColorInfo().alphaType();
    sk_sp<SkColorSpace> dstCS = keyContext.dstColorInfo().refColorSpace();
    if (!dstCS) {
        dstCS = SkColorSpace::MakeSRGB();
    }

    SkAlphaType workingAT;
    sk_sp<SkColorSpace> workingCS = filter->workingFormat(dstCS, &workingAT);

    // Use two nested compose blocks to chain (dst->working), child, and (working->dst) together
    // while appearing as one block to the parent node.
    ComposeColorFilterBlock::BeginBlock(keyContext, builder, gatherer);
        // Inner compose
        ComposeColorFilterBlock::BeginBlock(keyContext, builder, gatherer);
            // Innermost (inner of inner compose)
            ColorSpaceTransformBlock::ColorSpaceTransformData data1(
                    dstCS.get(), dstAT, workingCS.get(), workingAT);
            ColorSpaceTransformBlock::BeginBlock(keyContext, builder, gatherer, &data1);
            builder->endBlock();

            // Middle (outer of inner compose)
            AddToKey(keyContext, builder, gatherer, filter->child().get());
        builder->endBlock();

        // Outermost (outer of outer compose)
        ColorSpaceTransformBlock::ColorSpaceTransformData data2(
                workingCS.get(), workingAT, dstCS.get(), dstAT);
        ColorSpaceTransformBlock::BeginBlock(keyContext, builder, gatherer, &data2);
        builder->endBlock();
    builder->endBlock();
}

void AddToKey(const KeyContext& keyContext,
              PaintParamsKeyBuilder* builder,
              PipelineDataGatherer* gatherer,
              const SkColorFilter* filter) {
    if (!filter) {
        return;
    }
    switch (as_CFB(filter)->type()) {
    case SkColorFilterBase::Type::kNoop:
        // Return the input color as-is.
        PriorOutputBlock::BeginBlock(keyContext, builder, gatherer);
        builder->endBlock();
        return;
#define M(type)                                                        \
    case SkColorFilterBase::Type::k##type:                             \
        add_to_key(keyContext,                                         \
                   builder,                                            \
                   gatherer,                                           \
                   static_cast<const Sk##type##ColorFilter*>(filter)); \
        return;
        SK_ALL_COLOR_FILTERS(M)
#undef M
    }
    SkUNREACHABLE;
}

} // namespace skgpu::graphite
