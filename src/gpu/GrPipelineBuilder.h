/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GrPipelineBuilder_DEFINED
#define GrPipelineBuilder_DEFINED

#include "GrBatch.h"
#include "GrBlend.h"
#include "GrClip.h"
#include "GrDrawTargetCaps.h"
#include "GrGpuResourceRef.h"
#include "GrFragmentStage.h"
#include "GrProcOptInfo.h"
#include "GrRenderTarget.h"
#include "GrStencil.h"
#include "GrXferProcessor.h"
#include "SkMatrix.h"
#include "effects/GrCoverageSetOpXP.h"
#include "effects/GrDisableColorXP.h"
#include "effects/GrPorterDuffXferProcessor.h"
#include "effects/GrSimpleTextureEffect.h"

class GrDrawTargetCaps;
class GrPaint;
class GrTexture;

class GrPipelineBuilder {
public:
    GrPipelineBuilder();

    GrPipelineBuilder(const GrPipelineBuilder& pipelineBuilder) {
        SkDEBUGCODE(fBlockEffectRemovalCnt = 0;)
        *this = pipelineBuilder;
    }

    virtual ~GrPipelineBuilder();

    /**
     * Initializes the GrPipelineBuilder based on a GrPaint, view matrix and render target. Note
     * that GrPipelineBuilder encompasses more than GrPaint. Aspects of GrPipelineBuilder that have
     * no GrPaint equivalents are set to default values with the exception of vertex attribute state
     * which is unmodified by this function and clipping which will be enabled.
     */
    void setFromPaint(const GrPaint&, GrRenderTarget*, const GrClip*);

    /// @}

    /**
     * This function returns true if the render target destination pixel values will be read for
     * blending during draw.
     */
    bool willBlendWithDst(const GrPrimitiveProcessor*) const;

    ///////////////////////////////////////////////////////////////////////////
    /// @name Effect Stages
    /// Each stage hosts a GrProcessor. The effect produces an output color or coverage in the
    /// fragment shader. Its inputs are the output from the previous stage as well as some variables
    /// available to it in the fragment and vertex shader (e.g. the vertex position, the dst color,
    /// the fragment position, local coordinates).
    ///
    /// The stages are divided into two sets, color-computing and coverage-computing. The final
    /// color stage produces the final pixel color. The coverage-computing stages function exactly
    /// as the color-computing but the output of the final coverage stage is treated as a fractional
    /// pixel coverage rather than as input to the src/dst color blend step.
    ///
    /// The input color to the first color-stage is either the constant color or interpolated
    /// per-vertex colors. The input to the first coverage stage is either a constant coverage
    /// (usually full-coverage) or interpolated per-vertex coverage.
    ///
    /// See the documentation of kCoverageDrawing_StateBit for information about disabling the
    /// the color / coverage distinction.
    ////

    int numColorStages() const { return fColorStages.count(); }
    int numCoverageStages() const { return fCoverageStages.count(); }
    int numFragmentStages() const { return this->numColorStages() + this->numCoverageStages(); }

    const GrXPFactory* getXPFactory() const {
        if (!fXPFactory) {
            fXPFactory.reset(GrPorterDuffXPFactory::Create(SkXfermode::kSrc_Mode));
        }
        return fXPFactory.get();
    }

    const GrFragmentStage& getColorStage(int idx) const { return fColorStages[idx]; }
    const GrFragmentStage& getCoverageStage(int idx) const { return fCoverageStages[idx]; }

    /**
     * Checks whether the xp will need a copy of the destination to correctly blend.
     */
    bool willXPNeedDstCopy(const GrDrawTargetCaps& caps, const GrProcOptInfo& colorPOI,
                           const GrProcOptInfo& coveragePOI) const;

    /**
     * The xfer processor factory.
     */
    const GrXPFactory* setXPFactory(const GrXPFactory* xpFactory) {
        fXPFactory.reset(SkRef(xpFactory));
        return xpFactory;
    }

    void setCoverageSetOpXPFactory(SkRegion::Op regionOp, bool invertCoverage = false) {
        fXPFactory.reset(GrCoverageSetOpXPFactory::Create(regionOp, invertCoverage));
    }

    void setDisableColorXPFactory() {
        fXPFactory.reset(GrDisableColorXPFactory::Create());
    }

    const GrFragmentProcessor* addColorProcessor(const GrFragmentProcessor* effect) {
        SkASSERT(effect);
        SkNEW_APPEND_TO_TARRAY(&fColorStages, GrFragmentStage, (effect));
        fColorProcInfoValid = false;
        return effect;
    }

    const GrFragmentProcessor* addCoverageProcessor(const GrFragmentProcessor* effect) {
        SkASSERT(effect);
        SkNEW_APPEND_TO_TARRAY(&fCoverageStages, GrFragmentStage, (effect));
        fCoverageProcInfoValid = false;
        return effect;
    }

    /**
     * Creates a GrSimpleTextureEffect that uses local coords as texture coordinates.
     */
    void addColorTextureProcessor(GrTexture* texture, const SkMatrix& matrix) {
        this->addColorProcessor(GrSimpleTextureEffect::Create(texture, matrix))->unref();
    }

    void addCoverageTextureProcessor(GrTexture* texture, const SkMatrix& matrix) {
        this->addCoverageProcessor(GrSimpleTextureEffect::Create(texture, matrix))->unref();
    }

    void addColorTextureProcessor(GrTexture* texture,
                                  const SkMatrix& matrix,
                                  const GrTextureParams& params) {
        this->addColorProcessor(GrSimpleTextureEffect::Create(texture, matrix, params))->unref();
    }

    void addCoverageTextureProcessor(GrTexture* texture,
                                     const SkMatrix& matrix,
                                     const GrTextureParams& params) {
        this->addCoverageProcessor(GrSimpleTextureEffect::Create(texture, matrix, params))->unref();
    }

    /**
     * When this object is destroyed it will remove any color/coverage effects from the pipeline
     * builder that were added after its constructor.
     *
     * This class has strange behavior around geometry processor. If there is a GP on the
     * GrPipelineBuilder it will assert that the GP is not modified until after the destructor of
     * the ARE. If the GrPipelineBuilder has a NULL GP when the ARE is constructed then it will reset
     * it to null in the destructor.
     */
    class AutoRestoreEffects : public ::SkNoncopyable {
    public:
        AutoRestoreEffects() 
            : fPipelineBuilder(NULL)
            , fColorEffectCnt(0)
            , fCoverageEffectCnt(0) {}

        AutoRestoreEffects(GrPipelineBuilder* ds)
            : fPipelineBuilder(NULL)
            , fColorEffectCnt(0)
            , fCoverageEffectCnt(0) {
            this->set(ds);
        }

        ~AutoRestoreEffects() { this->set(NULL); }

        void set(GrPipelineBuilder* ds);

        bool isSet() const { return SkToBool(fPipelineBuilder); }

    private:
        GrPipelineBuilder*    fPipelineBuilder;
        int             fColorEffectCnt;
        int             fCoverageEffectCnt;
    };

    /**
     * AutoRestoreStencil
     *
     * This simple struct saves and restores the stencil settings
     */
    class AutoRestoreStencil : public ::SkNoncopyable {
    public:
        AutoRestoreStencil() : fPipelineBuilder(NULL) {}

        AutoRestoreStencil(GrPipelineBuilder* ds) : fPipelineBuilder(NULL) { this->set(ds); }

        ~AutoRestoreStencil() { this->set(NULL); }

        void set(GrPipelineBuilder* ds) {
            if (fPipelineBuilder) {
                fPipelineBuilder->setStencil(fStencilSettings);
            }
            fPipelineBuilder = ds;
            if (ds) {
                fStencilSettings = ds->getStencil();
            }
        }

        bool isSet() const { return SkToBool(fPipelineBuilder); }

    private:
        GrPipelineBuilder*  fPipelineBuilder;
        GrStencilSettings   fStencilSettings;
    };

    /// @}

    ///////////////////////////////////////////////////////////////////////////
    /// @name Blending
    ////

    /**
     * Determines whether multiplying the computed per-pixel color by the pixel's fractional
     * coverage before the blend will give the correct final destination color. In general it
     * will not as coverage is applied after blending.
     */
    bool canTweakAlphaForCoverage() const;

    /// @}


    /// @}

    ///////////////////////////////////////////////////////////////////////////
    /// @name Render Target
    ////

    /**
     * Retrieves the currently set render-target.
     *
     * @return    The currently set render target.
     */
    GrRenderTarget* getRenderTarget() const { return fRenderTarget.get(); }

    /**
     * Sets the render-target used at the next drawing call
     *
     * @param target  The render target to set.
     */
    void setRenderTarget(GrRenderTarget* target) { fRenderTarget.reset(SkSafeRef(target)); }

    /// @}

    ///////////////////////////////////////////////////////////////////////////
    /// @name Stencil
    ////

    const GrStencilSettings& getStencil() const { return fStencilSettings; }

    /**
     * Sets the stencil settings to use for the next draw.
     * Changing the clip has the side-effect of possibly zeroing
     * out the client settable stencil bits. So multipass algorithms
     * using stencil should not change the clip between passes.
     * @param settings  the stencil settings to use.
     */
    void setStencil(const GrStencilSettings& settings) { fStencilSettings = settings; }

    /**
     * Shortcut to disable stencil testing and ops.
     */
    void disableStencil() { fStencilSettings.setDisabled(); }

    GrStencilSettings* stencil() { return &fStencilSettings; }

    /// @}

    ///////////////////////////////////////////////////////////////////////////
    /// @name State Flags
    ////

    /**
     *  Flags that affect rendering. Controlled using enable/disableState(). All
     *  default to disabled.
     */
    enum StateBits {
        /**
         * Perform dithering. TODO: Re-evaluate whether we need this bit
         */
        kDither_StateBit        = 0x01,
        /**
         * Perform HW anti-aliasing. This means either HW FSAA, if supported by the render target,
         * or smooth-line rendering if a line primitive is drawn and line smoothing is supported by
         * the 3D API.
         */
        kHWAntialias_StateBit   = 0x02,

        kLast_StateBit = kHWAntialias_StateBit,
    };

    bool isDither() const { return 0 != (fFlagBits & kDither_StateBit); }
    bool isHWAntialias() const { return 0 != (fFlagBits & kHWAntialias_StateBit); }

    /**
     * Enable render state settings.
     *
     * @param stateBits bitfield of StateBits specifying the states to enable
     */
    void enableState(uint32_t stateBits) { fFlagBits |= stateBits; }

    /**
     * Disable render state settings.
     *
     * @param stateBits bitfield of StateBits specifying the states to disable
     */
    void disableState(uint32_t stateBits) { fFlagBits &= ~(stateBits); }

    /**
     * Enable or disable stateBits based on a boolean.
     *
     * @param stateBits bitfield of StateBits to enable or disable
     * @param enable    if true enable stateBits, otherwise disable
     */
    void setState(uint32_t stateBits, bool enable) {
        if (enable) {
            this->enableState(stateBits);
        } else {
            this->disableState(stateBits);
        }
    }

    /// @}

    ///////////////////////////////////////////////////////////////////////////
    /// @name Face Culling
    ////

    enum DrawFace {
        kInvalid_DrawFace = -1,

        kBoth_DrawFace,
        kCCW_DrawFace,
        kCW_DrawFace,
    };

    /**
     * Gets whether the target is drawing clockwise, counterclockwise,
     * or both faces.
     * @return the current draw face(s).
     */
    DrawFace getDrawFace() const { return fDrawFace; }

    /**
     * Controls whether clockwise, counterclockwise, or both faces are drawn.
     * @param face  the face(s) to draw.
     */
    void setDrawFace(DrawFace face) {
        SkASSERT(kInvalid_DrawFace != face);
        fDrawFace = face;
    }

    /// @}

    ///////////////////////////////////////////////////////////////////////////

    GrPipelineBuilder& operator= (const GrPipelineBuilder& that);

    // TODO delete when we have Batch
    const GrProcOptInfo& colorProcInfo(const GrPrimitiveProcessor* pp) const {
        this->calcColorInvariantOutput(pp);
        return fColorProcInfo;
    }

    const GrProcOptInfo& coverageProcInfo(const GrPrimitiveProcessor* pp) const {
        this->calcCoverageInvariantOutput(pp);
        return fCoverageProcInfo;
    }

    const GrProcOptInfo& colorProcInfo(const GrBatch* batch) const {
        this->calcColorInvariantOutput(batch);
        return fColorProcInfo;
    }

    const GrProcOptInfo& coverageProcInfo(const GrBatch* batch) const {
        this->calcCoverageInvariantOutput(batch);
        return fCoverageProcInfo;
    }

    void setClip(const GrClip& clip) { fClip = clip; }
    const GrClip& clip() const { return fClip; }

private:
    // Calculating invariant color / coverage information is expensive, so we partially cache the
    // results.
    //
    // canUseFracCoveragePrimProc() - Called in regular skia draw, caches results but only for a
    //                                specific color and coverage.  May be called multiple times
    // willBlendWithDst() - only called by Nvpr, does not cache results
    // GrOptDrawState constructor - never caches results

    /**
     * Primproc variants of the calc functions
     * TODO remove these when batch is everywhere
     */
    void calcColorInvariantOutput(const GrPrimitiveProcessor*) const;
    void calcCoverageInvariantOutput(const GrPrimitiveProcessor*) const;

    /**
     * GrBatch provides the initial seed for these loops based off of its initial geometry data
     */
    void calcColorInvariantOutput(const GrBatch*) const;
    void calcCoverageInvariantOutput(const GrBatch*) const;

    /**
     * If fColorProcInfoValid is false, function calculates the invariant output for the color
     * stages and results are stored in fColorProcInfo.
     */
    void calcColorInvariantOutput(GrColor) const;

    /**
     * If fCoverageProcInfoValid is false, function calculates the invariant output for the coverage
     * stages and results are stored in fCoverageProcInfo.
     */
    void calcCoverageInvariantOutput(GrColor) const;

    // Some of the auto restore objects assume that no effects are removed during their lifetime.
    // This is used to assert that this condition holds.
    SkDEBUGCODE(int fBlockEffectRemovalCnt;)

    typedef SkSTArray<4, GrFragmentStage> FragmentStageArray;

    SkAutoTUnref<GrRenderTarget>            fRenderTarget;
    uint32_t                                fFlagBits;
    GrStencilSettings                       fStencilSettings;
    DrawFace                                fDrawFace;
    mutable SkAutoTUnref<const GrXPFactory> fXPFactory;
    FragmentStageArray                      fColorStages;
    FragmentStageArray                      fCoverageStages;
    GrClip                                  fClip;

    mutable GrProcOptInfo fColorProcInfo;
    mutable GrProcOptInfo fCoverageProcInfo;
    mutable bool fColorProcInfoValid;
    mutable bool fCoverageProcInfoValid;
    mutable GrColor fColorCache;
    mutable GrColor fCoverageCache;

    friend class GrPipeline;
};

#endif
