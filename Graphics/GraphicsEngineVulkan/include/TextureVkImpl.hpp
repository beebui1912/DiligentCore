/*
 *  Copyright 2019-2025 Diligent Graphics LLC
 *  Copyright 2015-2019 Egor Yusov
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#pragma once

/// \file
/// Declaration of Diligent::TextureVkImpl class

#include <atomic>

#include "EngineVkImplTraits.hpp"
#include "TextureBase.hpp"
#include "TextureViewVkImpl.hpp"
#include "VulkanUtilities/MemoryManager.hpp"
#include "RefCntAutoPtr.hpp"

namespace Diligent
{

/// Texture object implementation in Vulkan backend.
class TextureVkImpl final : public TextureBase<EngineVkImplTraits>
{
public:
    using TTextureBase = TextureBase<EngineVkImplTraits>;
    using ViewImplType = TextureViewVkImpl;

    // Creates a new Vk resource
    TextureVkImpl(IReferenceCounters*        pRefCounters,
                  FixedBlockMemoryAllocator& TexViewObjAllocator,
                  RenderDeviceVkImpl*        pDeviceVk,
                  const TextureDesc&         TexDesc,
                  const TextureData*         pInitData = nullptr);

    // Attaches to an existing Vk resource
    TextureVkImpl(IReferenceCounters*        pRefCounters,
                  FixedBlockMemoryAllocator& TexViewObjAllocator,
                  class RenderDeviceVkImpl*  pDeviceVk,
                  const TextureDesc&         TexDesc,
                  RESOURCE_STATE             InitialState,
                  VkImage                    VkImageHandle);

    // Host allocation that a staging texture imports (VK_EXT_external_memory_host)
    struct HostMemoryImportInfo
    {
        void*    pHostMemory = nullptr;
        IObject* pOwner      = nullptr;
    };

    // Creates a staging texture whose data is stored in an imported host allocation
    // (IRenderDeviceVk::CreateTextureFromHostMemory)
    TextureVkImpl(IReferenceCounters*         pRefCounters,
                  FixedBlockMemoryAllocator&  TexViewObjAllocator,
                  class RenderDeviceVkImpl*   pDeviceVk,
                  const TextureDesc&          TexDesc,
                  const HostMemoryImportInfo& HostMemory);

    // Size and address alignment of a host allocation for a staging texture created in host memory.
    // Returns false if the device can not import host memory or the description is not a staging one.
    static bool GetHostMemoryRequirements(const class RenderDeviceVkImpl& DeviceVk,
                                          const TextureDesc&              TexDesc,
                                          VkDeviceSize&                   Size,
                                          VkDeviceSize&                   Alignment);

    // True for a staging texture created in an imported host allocation
    bool IsInHostMemory() const { return m_pHostMemory != nullptr; }

    ~TextureVkImpl();

    IMPLEMENT_QUERY_INTERFACE_IN_PLACE(IID_TextureVk, TTextureBase)

    /// Implementation of ITextureVk::GetVkImage().
    virtual VkImage DILIGENT_CALL_TYPE GetVkImage() const override final { return m_VulkanImage; }

    /// Implementation of ITexture::GetNativeHandle() in Vulkan backend.
    virtual Uint64 DILIGENT_CALL_TYPE GetNativeHandle() override final { return BitCast<Uint64>(GetVkImage()); }

    /// Implementation of ITextureVk::SetLayout().
    virtual void DILIGENT_CALL_TYPE SetLayout(VkImageLayout Layout) override final;

    /// Implementation of ITextureVk::GetLayout().
    virtual VkImageLayout DILIGENT_CALL_TYPE GetLayout() const override final;

    VkBuffer GetVkStagingBuffer() const
    {
        return m_StagingBuffer;
    }

    uint8_t* GetStagingDataCPUAddress() const
    {
        if (m_pHostMemory != nullptr)
            return static_cast<uint8_t*>(m_pHostMemory) + m_StagingDataAlignedOffset;

        uint8_t* StagingDataCPUAddress = reinterpret_cast<uint8_t*>(m_MemoryAllocation.Page->GetCPUMemory());
        VERIFY_EXPR(StagingDataCPUAddress != nullptr);
        StagingDataCPUAddress += m_StagingDataAlignedOffset;
        return StagingDataCPUAddress;
    }

    void InvalidateStagingRange(VkDeviceSize Offset, VkDeviceSize Size);

    // For non-compressed color format buffer, the offset must be a multiple of the format's texel block size.
    // For compressed format buffer, the offset must be a multiple of the compressed texel block size in bytes.
    // For depth-stencil format buffer, the offset must be a multiple of 4.
    // If command buffer does not support graphics or compute commands, then the buffer offset must be a multiple of 4.
    // ("Copying Data Between Buffers and Images")
    static constexpr Uint32 StagingBufferOffsetAlignment = 16; // max texel size - 16 bytes (RGBA32F), max texel block size - 16 bytes.

#ifdef DILIGENT_DEVELOPMENT
    // Linked multi-GPU: with default node masks every node has its own instance of the texture, with its
    // own layout, while the state is tracked once per texture. Warns when two nodes use such a texture.
    void DvpVerifySingleNodeUse(Uint32 NodeIndex);
#endif

protected:
    void CreateViewInternal(const struct TextureViewDesc& ViewDesc, ITextureView** ppView, bool bIsDefaultView) override;

    // InitDeviceMask: linked multi-GPU devices whose instance of the memory receives the data (0 = the queue's node)
    void InitializeContentOnDevice(const TextureData&          InitData,
                                   const TextureFormatAttribs& FmtAttribs,
                                   const VkImageCreateInfo&    ImageCI,
                                   uint32_t                    InitDeviceMask) noexcept(false);
    bool InitializeContentOnHost(const TextureData&          InitData,
                                 const TextureFormatAttribs& FmtAttribs,
                                 const VkImageCreateInfo&    ImageCI) noexcept(false);

    void CreateStagingTexture(const TextureData*          pInitData,
                              const TextureFormatAttribs& FmtAttribs);

    VulkanUtilities::ImageViewWrapper CreateImageView(TextureViewDesc& ViewDesc);

    void InitSparseProperties() noexcept(false);

    VulkanUtilities::ImageWrapper     m_VulkanImage;
    VulkanUtilities::BufferWrapper    m_StagingBuffer;
    VulkanUtilities::MemoryAllocation m_MemoryAllocation;
    VkDeviceSize                      m_StagingDataAlignedOffset = 0;

    // Imported host allocation of a staging texture created in host memory. The members are destroyed
    // in reverse order, so the owner releases the allocation after the memory object has been freed.
    struct ImportedHostMemory
    {
        RefCntAutoPtr<IObject>               pOwner;
        VulkanUtilities::DeviceMemoryWrapper Memory;
    };
    ImportedHostMemory m_HostMemory;
    void*              m_pHostMemory = nullptr;

#ifdef DILIGENT_DEVELOPMENT
    std::atomic<Uint32> m_DvpUsedNodes{0};
#endif
};

} // namespace Diligent
