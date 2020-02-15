/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/GrImageTextureMaker.h"

#include "src/gpu/GrColorSpaceXform.h"
#include "src/gpu/GrContextPriv.h"
#include "src/gpu/GrRecordingContextPriv.h"
#include "src/gpu/SkGr.h"
#include "src/gpu/effects/GrYUVtoRGBEffect.h"
#include "src/image/SkImage_GpuYUVA.h"
#include "src/image/SkImage_Lazy.h"

static GrImageInfo get_image_info(GrRecordingContext* context, const SkImage* client) {
    SkASSERT(client->isLazyGenerated());
    const SkImage_Lazy* lazyImage = static_cast<const SkImage_Lazy*>(client);

    GrColorType ct = lazyImage->colorTypeOfLockTextureProxy(context->priv().caps());

    return {ct, client->alphaType(), client->refColorSpace(), client->dimensions()};
}

GrImageTextureMaker::GrImageTextureMaker(GrRecordingContext* context, const SkImage* client,
                                         SkImage::CachingHint chint, bool useDecal)
        : INHERITED(context, get_image_info(context, client), useDecal)
        , fImage(static_cast<const SkImage_Lazy*>(client))
        , fCachingHint(chint) {
    SkASSERT(client->isLazyGenerated());
    GrMakeKeyFromImageID(&fOriginalKey, client->uniqueID(), SkIRect::MakeSize(this->dimensions()));
}

GrSurfaceProxyView GrImageTextureMaker::refOriginalTextureProxyView(bool willBeMipped,
                                                                    AllowedTexGenType onlyIfFast) {
    return fImage->lockTextureProxyView(this->context(), fOriginalKey, fCachingHint, willBeMipped,
                                        onlyIfFast);
}

void GrImageTextureMaker::makeCopyKey(const CopyParams& stretch, GrUniqueKey* paramsCopyKey) {
    if (fOriginalKey.isValid() && SkImage::kAllow_CachingHint == fCachingHint) {
        GrUniqueKey cacheKey;
        fImage->makeCacheKeyFromOrigKey(fOriginalKey, &cacheKey);
        MakeCopyKeyFromOrigKey(cacheKey, stretch, paramsCopyKey);
    }
}


/////////////////////////////////////////////////////////////////////////////////////////////////

GrYUVAImageTextureMaker::GrYUVAImageTextureMaker(GrContext* context, const SkImage* client,
                                                 bool useDecal)
        : INHERITED(context, client->imageInfo(), useDecal)
        , fImage(static_cast<const SkImage_GpuYUVA*>(client)) {
    SkASSERT(as_IB(client)->isYUVA());
    GrMakeKeyFromImageID(&fOriginalKey, client->uniqueID(), SkIRect::MakeSize(this->dimensions()));
}

GrSurfaceProxyView GrYUVAImageTextureMaker::refOriginalTextureProxyView(
        bool willBeMipped, AllowedTexGenType onlyIfFast) {
    if (AllowedTexGenType::kCheap == onlyIfFast) {
        return {};
    }

    if (willBeMipped) {
        return fImage->asMippedTextureProxyViewRef(this->context());
    } else {
        return fImage->asSurfaceProxyViewRef(this->context());
    }
}

void GrYUVAImageTextureMaker::makeCopyKey(const CopyParams& stretch, GrUniqueKey* paramsCopyKey) {
    // TODO: Do we ever want to disable caching?
    if (fOriginalKey.isValid()) {
        GrUniqueKey cacheKey;
        static const GrUniqueKey::Domain kDomain = GrUniqueKey::GenerateDomain();
        GrUniqueKey::Builder builder(&cacheKey, fOriginalKey, kDomain, 0, "Image");
        MakeCopyKeyFromOrigKey(cacheKey, stretch, paramsCopyKey);
    }
}

std::unique_ptr<GrFragmentProcessor> GrYUVAImageTextureMaker::createFragmentProcessor(
    const SkMatrix& textureMatrix,
    const SkRect& constraintRect,
    FilterConstraint filterConstraint,
    bool coordsLimitedToConstraintRect,
    const GrSamplerState::Filter* filterOrNullForBicubic) {

    // Check simple cases to see if we need to fall back to flattening the image (or whether it's
    // already been flattened.)
    if (!filterOrNullForBicubic || this->domainNeedsDecal() || fImage->fRGBView.proxy()) {
        return this->INHERITED::createFragmentProcessor(textureMatrix, constraintRect,
                                                        filterConstraint,
                                                        coordsLimitedToConstraintRect,
                                                        filterOrNullForBicubic);
    }

    // Check to see if the client has given us pre-mipped textures or we can generate them
    // If not, fall back to bilerp. Also fall back to bilerp when a domain is requested
    GrSamplerState::Filter filter = *filterOrNullForBicubic;
    if (GrSamplerState::Filter::kMipMap == filter &&
        (filterConstraint == GrTextureProducer::kYes_FilterConstraint ||
         !fImage->setupMipmapsForPlanes(this->context()))) {
        filter = GrSamplerState::Filter::kBilerp;
    }

    // Cannot rely on GrTextureProducer's domain infrastructure since we need to calculate domain's
    // per plane, which may be different, so respect the filterConstraint without any additional
    // analysis.
    const SkRect* domain = nullptr;
    if (filterConstraint == GrTextureProducer::kYes_FilterConstraint) {
        domain = &constraintRect;
    }

    const auto& caps = *fImage->context()->priv().caps();
    auto fp = GrYUVtoRGBEffect::Make(fImage->fProxies, fImage->fYUVAIndices, fImage->fYUVColorSpace,
                                     filter, caps, textureMatrix, domain);
    if (fImage->fFromColorSpace) {
        fp = GrColorSpaceXformEffect::Make(std::move(fp), fImage->fFromColorSpace.get(),
                                           fImage->alphaType(), fImage->colorSpace());
    }
    return fp;
}
