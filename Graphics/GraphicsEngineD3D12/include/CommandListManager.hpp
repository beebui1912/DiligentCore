/*
 *  Copyright 2019-2022 Diligent Graphics LLC
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

#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <stdint.h>
#include "IndexWrapper.hpp"

namespace Diligent
{

class RenderDeviceD3D12Impl;

class CommandListManager
{
public:
    CommandListManager(RenderDeviceD3D12Impl& DeviceD3D12Impl, D3D12_COMMAND_LIST_TYPE ListType);
    ~CommandListManager();

    // clang-format off
    CommandListManager             (const CommandListManager&)  = delete;
    CommandListManager             (      CommandListManager&&) = delete;
    CommandListManager& operator = (const CommandListManager&)  = delete;
    CommandListManager& operator = (      CommandListManager&&) = delete;
    // clang-format on

    // Returns the maximum supported interface version.
    // NodeMask selects the GPU node the command list and its allocator target in Linked Multi-GPU mode.
    // The default (1) is node 0, which matches the legacy single-GPU behavior.
    void CreateNewCommandList(ID3D12GraphicsCommandList** ppList, ID3D12CommandAllocator** ppAllocator, Uint32& IfaceVersion, UINT NodeMask = 1);

    // NodeMask selects the node pool the allocator comes from. A D3D12 command allocator becomes
    // associated with the node of the first command list it records, so allocators must not be shared
    // across nodes; they are pooled per node mask. Default (1) is node 0 (legacy single-GPU behavior).
    void RequestAllocator(ID3D12CommandAllocator** ppAllocator, UINT NodeMask = 1);
    void ReleaseAllocator(CComPtr<ID3D12CommandAllocator>&& Allocator, SoftwareQueueIndex CmdQueue, Uint64 FenceValue);

    // Returns allocator to the list of available allocators. The GPU must have finished using the
    // allocator
    void FreeAllocator(CComPtr<ID3D12CommandAllocator>&& Allocator);

#ifdef DILIGENT_DEVELOPMENT
    Int32 GetAllocatorCounter() const
    {
        return m_AllocatorCounter.load();
    }
#endif

    D3D12_COMMAND_LIST_TYPE GetCommandListType() const
    {
        return m_CmdListType;
    }

private:
    std::mutex m_AllocatorMutex;
    // Free command allocators pooled per GPU node mask. For single-GPU there is a single entry
    // (node mask 1), so behavior matches the legacy single-pool implementation.
    std::unordered_map<UINT, std::vector<CComPtr<ID3D12CommandAllocator>>> m_FreeAllocators;
    // Tracks the node mask each allocator was created for, so FreeAllocator() returns it to the
    // correct per-node pool. Entries persist for the lifetime of the allocator.
    std::unordered_map<ID3D12CommandAllocator*, UINT> m_AllocatorNodeMask;

    RenderDeviceD3D12Impl& m_DeviceD3D12Impl;

    const D3D12_COMMAND_LIST_TYPE m_CmdListType;

    std::atomic<Int32> m_NumAllocators{0}; // For logging only

#ifdef DILIGENT_DEVELOPMENT
    std::atomic<Int32> m_AllocatorCounter{0};
#endif
};

} // namespace Diligent
