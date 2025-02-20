/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VulkanMemory.h"
#include "VulkanTexture.h"
#include "VulkanUtility.h"

#include <private/backend/BackendUtils.h>

#include "DataReshaper.h"

#include <utils/Panic.h>

using namespace bluevk;

namespace filament {
namespace backend {

VulkanTexture::VulkanTexture(VulkanContext& context, SamplerType target, uint8_t levels,
        TextureFormat tformat, uint8_t samples, uint32_t w, uint32_t h, uint32_t depth,
        TextureUsage tusage, VulkanStagePool& stagePool, VkComponentMapping swizzle) :
        HwTexture(target, levels, samples, w, h, depth, tformat, tusage),

        // Vulkan does not support 24-bit depth, use the official fallback format.
        mVkFormat(tformat == TextureFormat::DEPTH24 ? context.finalDepthFormat :
                backend::getVkFormat(tformat)),

        mSwizzle(swizzle), mContext(context), mStagePool(stagePool) {

    // Create an appropriately-sized device-only VkImage, but do not fill it yet.
    VkImageCreateInfo imageInfo {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = target == SamplerType::SAMPLER_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = mVkFormat,
        .extent = { w, h, depth },
        .mipLevels = levels,
        .arrayLayers = 1,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = 0
    };
    if (target == SamplerType::SAMPLER_CUBEMAP) {
        imageInfo.arrayLayers = 6;
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    if (target == SamplerType::SAMPLER_2D_ARRAY) {
        imageInfo.arrayLayers = depth;
        imageInfo.extent.depth = 1;
        // NOTE: We do not use VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT here because:
        //
        //  (a) MoltenVK does not support it, and
        //  (b) it is necessary only when 3D textures need to support array-style access
        //
        // In other words, the "arrayness" of the texture is an aspect of the VkImageView,
        // not the VkImage.
    }

    // Filament expects blit() to work with any texture, so we almost always set these usage flags.
    // TODO: investigate performance implications of setting these flags.
    const VkImageUsageFlags blittable = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (any(usage & TextureUsage::SAMPLEABLE)) {

#if VK_ENABLE_VALIDATION
        // Validate that the format is actually sampleable.
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice, mVkFormat, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            utils::slog.w << "Texture usage is SAMPLEABLE but format " << mVkFormat << " is not "
                    "sampleable with optimal tiling." << utils::io::endl;
        }
#endif

        imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (any(usage & TextureUsage::COLOR_ATTACHMENT)) {
        imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | blittable;
        if (any(usage & TextureUsage::SUBPASS_INPUT)) {
            imageInfo.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        }
    }
    if (any(usage & TextureUsage::STENCIL_ATTACHMENT)) {
        imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (any(usage & TextureUsage::UPLOADABLE)) {
        imageInfo.usage |= blittable;
    }
    if (any(usage & TextureUsage::DEPTH_ATTACHMENT)) {
        imageInfo.usage |= blittable;
        imageInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        // Depth resolves uses a custom shader and therefore needs to be sampleable.
        if (samples > 1) {
            imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        }
    }

    // Constrain the sample count according to the sample count masks in VkPhysicalDeviceProperties.
    // Note that VulkanRenderTarget holds a single MSAA count, so we play it safe if this is used as
    // any kind of attachment (color or depth).
    const auto& limits = context.physicalDeviceProperties.limits;
    if (imageInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        samples = reduceSampleCount(samples, isDepthFormat(mVkFormat) ?
                limits.sampledImageDepthSampleCounts : limits.sampledImageColorSampleCounts);
    }
    if (imageInfo.usage & (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        samples = reduceSampleCount(samples, limits.framebufferDepthSampleCounts &
            limits.framebufferColorSampleCounts);
    }
    this->samples = samples;
    imageInfo.samples = (VkSampleCountFlagBits) samples;

    VkResult error = vkCreateImage(context.device, &imageInfo, VKALLOC, &mTextureImage);
    if (error || FILAMENT_VULKAN_VERBOSE) {
        utils::slog.d << "vkCreateImage: "
            << "result = " << error << ", "
            << "handle = " << utils::io::hex << mTextureImage << utils::io::dec << ", "
            << "extent = " << w << "x" << h << "x"<< depth << ", "
            << "mipLevels = " << int(levels) << ", "
            << "usage = " << imageInfo.usage << ", "
            << "samples = " << imageInfo.samples << ", "
            << "format = " << mVkFormat << utils::io::endl;
    }
    ASSERT_POSTCONDITION(!error, "Unable to create image.");

    // Allocate memory for the VkImage and bind it.
    VkMemoryRequirements memReqs = {};
    vkGetImageMemoryRequirements(context.device, mTextureImage, &memReqs);
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = context.selectMemoryType(memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    error = vkAllocateMemory(context.device, &allocInfo, nullptr, &mTextureImageMemory);
    ASSERT_POSTCONDITION(!error, "Unable to allocate image memory.");
    error = vkBindImageMemory(context.device, mTextureImage, mTextureImageMemory, 0);
    ASSERT_POSTCONDITION(!error, "Unable to bind image.");

    mAspect = any(usage & TextureUsage::DEPTH_ATTACHMENT) ? VK_IMAGE_ASPECT_DEPTH_BIT :
            VK_IMAGE_ASPECT_COLOR_BIT;

    // Spec out the "primary" VkImageView that shaders use to sample from the image.
    mPrimaryViewRange.aspectMask = mAspect;
    mPrimaryViewRange.baseMipLevel = 0;
    mPrimaryViewRange.levelCount = levels;
    mPrimaryViewRange.baseArrayLayer = 0;
    if (target == SamplerType::SAMPLER_CUBEMAP) {
        mViewType = VK_IMAGE_VIEW_TYPE_CUBE;
        mPrimaryViewRange.layerCount = 6;
    } else if (target == SamplerType::SAMPLER_2D_ARRAY) {
        mViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        mPrimaryViewRange.layerCount = depth;
    } else if (target == SamplerType::SAMPLER_3D) {
        mViewType = VK_IMAGE_VIEW_TYPE_3D;
        mPrimaryViewRange.layerCount = 1;
    } else {
        mViewType = VK_IMAGE_VIEW_TYPE_2D;
        mPrimaryViewRange.layerCount = 1;
    }

    // Go ahead and create the primary image view, no need to do it lazily.
    getImageView(mPrimaryViewRange);

    // Transition the layout of each image slice.
    // TODO: The potentially redundant transition for SAMPLEABLE images.
    if (any(usage & (TextureUsage::COLOR_ATTACHMENT | TextureUsage::DEPTH_ATTACHMENT | TextureUsage::SAMPLEABLE))) {
        const uint32_t layers = mPrimaryViewRange.layerCount;
        transitionImageLayout(mContext.commands->get().cmdbuffer, textureTransitionHelper({
                .image = mTextureImage,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = mContext.getTextureLayout(usage),
                .subresources = {
                        mAspect,
                        0,
                        levels,
                        0,
                        layers
                }
        }));
    }
}

VulkanTexture::~VulkanTexture() {
    delete mSidecarMSAA;
    vkDestroyImage(mContext.device, mTextureImage, VKALLOC);
    vkFreeMemory(mContext.device, mTextureImageMemory, VKALLOC);
    for (auto entry : mCachedImageViews) {
        vkDestroyImageView(mContext.device, entry.second, VKALLOC);
    }
}

void VulkanTexture::update2DImage(const PixelBufferDescriptor& data, uint32_t width,
        uint32_t height, int miplevel) {
    update3DImage(std::move(data), width, height, 1, miplevel);
}

void VulkanTexture::update3DImage(const PixelBufferDescriptor& data, uint32_t width, uint32_t height,
        uint32_t depth, int miplevel) {
    assert_invariant(width <= this->width && height <= this->height && depth <= this->depth);
    const PixelBufferDescriptor* hostData = &data;
    PixelBufferDescriptor reshapedData;

    // First, reshape 3-component data into 4-component data. The fourth component is usually
    // set to 1 (one exception is when type = HALF). In practice, alpha is just a dummy channel.
    // Note that the reshaped data is freed at the end of this method due to the callback.
    if (reshape(data, reshapedData)) {
        hostData = &reshapedData;
    }

    // If format conversion is both required and supported, use vkCmdBlitImage. Otherwise, use
    // vkCmdCopyBufferToImage.

    const VkFormat hostFormat = backend::getVkFormat(hostData->format, hostData->type);
    const VkFormat deviceFormat = getVkFormatLinear(mVkFormat);

    if (hostFormat != deviceFormat && hostFormat != VK_FORMAT_UNDEFINED) {
        updateWithBlitImage(*hostData, width, height, depth, miplevel);
    } else {
        updateWithCopyBuffer(*hostData, width, height, depth, miplevel);
    }
}

void VulkanTexture::updateWithCopyBuffer(const PixelBufferDescriptor& hostData, uint32_t width,
        uint32_t height, uint32_t depth, uint32_t miplevel) {
    void* mapped = nullptr;
    VulkanStage const* stage = mStagePool.acquireStage(hostData.size);
    vmaMapMemory(mContext.allocator, stage->memory, &mapped);
    memcpy(mapped, hostData.buffer, hostData.size);
    vmaUnmapMemory(mContext.allocator, stage->memory);
    vmaFlushAllocation(mContext.allocator, stage->memory, 0, hostData.size);

    const VkCommandBuffer cmdbuffer = mContext.commands->get().cmdbuffer;

    // We can't blindly use LAYOUT_UNDEFINED because it may destroy the data, and because
    // we're potentially updating only a sub-region it would be a problem.
    VkImageLayout textureLayout = mContext.getTextureLayout(usage);
    transitionImageLayout(cmdbuffer, textureTransitionHelper({
            .image = mTextureImage,
            .oldLayout = textureLayout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .subresources = {
                    mAspect,
                    miplevel, 1,
                    0,1
            }
    }));

    copyBufferToImage(cmdbuffer, stage->buffer, mTextureImage, width, height, depth,
            nullptr, miplevel);

    transitionImageLayout(cmdbuffer, textureTransitionHelper({
            .image = mTextureImage,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = textureLayout,
            .subresources = {
                    mAspect,
                    miplevel, 1,
                    0,1
            }
    }));
}

void VulkanTexture::updateWithBlitImage(const PixelBufferDescriptor& hostData, uint32_t width,
        uint32_t height, uint32_t depth, uint32_t miplevel) {
    void* mapped = nullptr;
    VulkanStageImage const* stage = mStagePool.acquireImage(
            hostData.format, hostData.type, width, height);
    vmaMapMemory(mContext.allocator, stage->memory, &mapped);
    memcpy(mapped, hostData.buffer, hostData.size);
    vmaUnmapMemory(mContext.allocator, stage->memory);
    vmaFlushAllocation(mContext.allocator, stage->memory, 0, hostData.size);

    const VkCommandBuffer cmdbuffer = mContext.commands->get().cmdbuffer;

    // TODO: support blit-based format conversion for 3D images and cubemaps.
    const int layer = 0;

    const VkOffset3D rect[2] { {0, 0, 0}, {int32_t(width), int32_t(height), 1} };

    const VkImageBlit blitRegions[1] = {{
        .srcSubresource = { mAspect, 0, 0, 1 },
        .srcOffsets = { rect[0], rect[1] },
        .dstSubresource = { mAspect, uint32_t(miplevel), layer, 1 },
        .dstOffsets = { rect[0], rect[1] }
    }};

    // We can't blindly use LAYOUT_UNDEFINED because it may destroy the data, and because
    // we're potentially updating only a sub-region it would be a problem.
    VkImageLayout textureLayout = mContext.getTextureLayout(usage);
    transitionImageLayout(cmdbuffer, textureTransitionHelper({
            .image = mTextureImage,
            .oldLayout = textureLayout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .subresources = { mAspect, miplevel, 1, 0, 1 }
    }));

    vkCmdBlitImage(cmdbuffer, stage->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mTextureImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, blitRegions, VK_FILTER_NEAREST);

    transitionImageLayout(cmdbuffer, textureTransitionHelper({
            .image = mTextureImage,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = textureLayout,
            .subresources = { mAspect, miplevel, 1, 0, 1 }
    }));
}

void VulkanTexture::updateCubeImage(const PixelBufferDescriptor& data,
        const FaceOffsets& faceOffsets, uint32_t miplevel) {
    assert_invariant(this->target == SamplerType::SAMPLER_CUBEMAP);
    const bool reshape = getBytesPerPixel(format) == 3;
    const void* cpuData = data.buffer;
    const uint32_t numSrcBytes = data.size;
    const uint32_t numDstBytes = reshape ? (4 * numSrcBytes / 3) : numSrcBytes;

    // Create and populate the staging buffer.
    VulkanStage const* stage = mStagePool.acquireStage(numDstBytes);
    void* mapped;
    vmaMapMemory(mContext.allocator, stage->memory, &mapped);
    if (reshape) {
        DataReshaper::reshape<uint8_t, 3, 4>(mapped, cpuData, numSrcBytes);
    } else {
        memcpy(mapped, cpuData, numSrcBytes);
    }
    vmaUnmapMemory(mContext.allocator, stage->memory);
    vmaFlushAllocation(mContext.allocator, stage->memory, 0, numDstBytes);


    const VkCommandBuffer cmdbuffer = mContext.commands->get().cmdbuffer;
    const uint32_t width = std::max(1u, this->width >> miplevel);
    const uint32_t height = std::max(1u, this->height >> miplevel);

    // We can use LAYOUT_UNDEFINED here because we're always replacing the whole data, so it
    // doesn't matter if the previous data is lost.
    transitionImageLayout(cmdbuffer, textureTransitionHelper({
            .image = mTextureImage,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .subresources = { mAspect, miplevel, 1, 0, 6 }
    }));

    copyBufferToImage(cmdbuffer, stage->buffer, mTextureImage, width, height, 1,
            &faceOffsets, miplevel);

    transitionImageLayout(cmdbuffer, textureTransitionHelper({
            .image = mTextureImage,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = mContext.getTextureLayout(usage),
            .subresources = { mAspect, miplevel, 1, 0, 6 }
    }));
}

void VulkanTexture::setPrimaryRange(uint32_t minMiplevel, uint32_t maxMiplevel) {
    maxMiplevel = filament::math::min(int(maxMiplevel), int(this->levels - 1));
    mPrimaryViewRange.baseMipLevel = minMiplevel;
    mPrimaryViewRange.levelCount = maxMiplevel - minMiplevel + 1;
    getImageView(mPrimaryViewRange);
}

VkImageView VulkanTexture::getAttachmentView(int singleLevel, int singleLayer,
        VkImageAspectFlags aspect) {
    return getImageView({
        .aspectMask = aspect,
        .baseMipLevel = uint32_t(singleLevel),
        .levelCount = uint32_t(1),
        .baseArrayLayer = uint32_t(singleLayer),
        .layerCount = uint32_t(1),
    }, true);
}

VkImageView VulkanTexture::getImageView(VkImageSubresourceRange range, bool isAttachment) {
    auto iter = mCachedImageViews.find(range);
    if (iter != mCachedImageViews.end()) {
        return iter->second;
    }
    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = mTextureImage,
        .viewType = isAttachment ? VK_IMAGE_VIEW_TYPE_2D : mViewType,
        .format = mVkFormat,
        .components = isAttachment ? (VkComponentMapping{}) : mSwizzle,
        .subresourceRange = range
    };
    VkImageView imageView;
    vkCreateImageView(mContext.device, &viewInfo, VKALLOC, &imageView);
    mCachedImageViews.emplace(range, imageView);
    return imageView;
}

void VulkanTexture::copyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image,
        uint32_t width, uint32_t height, uint32_t depth, FaceOffsets const* faceOffsets, uint32_t miplevel) {
    VkExtent3D extent { width, height, depth };
    if (target == SamplerType::SAMPLER_CUBEMAP) {
        assert_invariant(faceOffsets);
        VkBufferImageCopy regions[6] = {{}};
        for (size_t face = 0; face < 6; face++) {
            auto& region = regions[face];
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.baseArrayLayer = face;
            region.imageSubresource.layerCount = 1;
            region.imageSubresource.mipLevel = miplevel;
            region.imageExtent = extent;
            region.bufferOffset = faceOffsets->offsets[face];
        }
        vkCmdCopyBufferToImage(cmd, buffer, image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions);
        return;
    }
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = miplevel;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = extent;
    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

} // namespace filament
} // namespace backend
