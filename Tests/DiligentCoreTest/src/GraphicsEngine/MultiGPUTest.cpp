/*
 *  Copyright 2019-2026 Diligent Graphics LLC
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

#include "GraphicsTypes.h"
#include "Buffer.h"
#include "Texture.h"
#include "DeviceContext.h"
#include "GraphicsAccessories.hpp"
#include "CrossDeviceTransferManager.hpp"

#include "gtest/gtest.h"

using namespace Diligent;

namespace
{

TEST(MultiGPU, GpuModeEnum)
{
    static_assert(GPU_MODE_COUNT == 3, "Unexpected GPU_MODE count");

    EXPECT_EQ(GPU_MODE_SINGLE, 0);
    EXPECT_EQ(GPU_MODE_LINKED, 1);
    EXPECT_EQ(GPU_MODE_UNLINKED, 2);

    EXPECT_STREQ(GetGpuModeString(GPU_MODE_SINGLE), "Single");
    EXPECT_STREQ(GetGpuModeString(GPU_MODE_LINKED), "Linked");
    EXPECT_STREQ(GetGpuModeString(GPU_MODE_UNLINKED), "Unlinked");

    EXPECT_STREQ(GetGpuModeString(GPU_MODE_SINGLE, true), "GPU_MODE_SINGLE");
    EXPECT_STREQ(GetGpuModeString(GPU_MODE_LINKED, true), "GPU_MODE_LINKED");
    EXPECT_STREQ(GetGpuModeString(GPU_MODE_UNLINKED, true), "GPU_MODE_UNLINKED");
}

TEST(MultiGPU, GraphicsAdapterInfo)
{
    GraphicsAdapterInfo Info1{};
    EXPECT_EQ(Info1.NodeCount, 1u);
    EXPECT_EQ(Info1.NodeMask, 1u);

    GraphicsAdapterInfo Info2{};
    EXPECT_TRUE(Info1 == Info2);

    Info2.NodeCount = 2;
    Info2.NodeMask  = 0x3;
    EXPECT_FALSE(Info1 == Info2);

    Info1.NodeCount = 2;
    Info1.NodeMask  = 0x3;
    EXPECT_TRUE(Info1 == Info2);
}

TEST(MultiGPU, ImmediateContextCreateInfo)
{
    ImmediateContextCreateInfo DefaultCI{};
    EXPECT_EQ(DefaultCI.NodeIndex, 0u);
    EXPECT_EQ(DefaultCI.QueueId, DEFAULT_QUEUE_ID);

    ImmediateContextCreateInfo Node1CI{"Worker Node 1", 0, QUEUE_PRIORITY_HIGH, 1};
    EXPECT_STREQ(Node1CI.Name, "Worker Node 1");
    EXPECT_EQ(Node1CI.QueueId, 0u);
    EXPECT_EQ(Node1CI.Priority, QUEUE_PRIORITY_HIGH);
    EXPECT_EQ(Node1CI.NodeIndex, 1u);
}

TEST(MultiGPU, EngineCreateInfo)
{
    EngineCreateInfo EngineCI{};
    EXPECT_EQ(EngineCI.GpuMode, GPU_MODE_SINGLE);
    EXPECT_EQ(EngineCI.NodeCount, 0u);

    EngineCI.GpuMode   = GPU_MODE_LINKED;
    EngineCI.NodeCount = 2;
    EXPECT_EQ(EngineCI.GpuMode, GPU_MODE_LINKED);
    EXPECT_EQ(EngineCI.NodeCount, 2u);
}

TEST(MultiGPU, BufferDesc)
{
    BufferDesc DefaultBuffDesc{};
    EXPECT_EQ(DefaultBuffDesc.CreationNodeMask, 0u);
    EXPECT_EQ(DefaultBuffDesc.VisibleNodeMask, 0u);

    BufferDesc Buff1{"Test Buffer 1", 1024, BIND_UNIFORM_BUFFER, USAGE_DEFAULT, CPU_ACCESS_NONE, BUFFER_MODE_UNDEFINED, 0, 1, 0x1, 0x3};
    EXPECT_EQ(Buff1.CreationNodeMask, 0x1u);
    EXPECT_EQ(Buff1.VisibleNodeMask, 0x3u);

    BufferDesc Buff2 = Buff1;
    EXPECT_TRUE(Buff1 == Buff2);

    Buff2.CreationNodeMask = 0x2;
    EXPECT_FALSE(Buff1 == Buff2);

    Buff2.CreationNodeMask = 0x1;
    Buff2.VisibleNodeMask  = 0x1;
    EXPECT_FALSE(Buff1 == Buff2);
}

TEST(MultiGPU, TextureDesc)
{
    TextureDesc DefaultTexDesc{};
    EXPECT_EQ(DefaultTexDesc.CreationNodeMask, 0u);
    EXPECT_EQ(DefaultTexDesc.VisibleNodeMask, 0u);

    TextureDesc Tex1{"Peer Texture", RESOURCE_DIM_TEX_2D, 1920, 1080, 1, TEX_FORMAT_RGBA8_UNORM,
                     1, 1, USAGE_DEFAULT, BIND_SHADER_RESOURCE | BIND_RENDER_TARGET,
                     CPU_ACCESS_NONE, MISC_TEXTURE_FLAG_NONE, OptimizedClearValue{}, 1, 0x2, 0x3};

    EXPECT_EQ(Tex1.CreationNodeMask, 0x2u);
    EXPECT_EQ(Tex1.VisibleNodeMask, 0x3u);

    TextureDesc Tex2 = Tex1;
    EXPECT_TRUE(Tex1 == Tex2);

    Tex2.VisibleNodeMask = 0x2;
    EXPECT_FALSE(Tex1 == Tex2);
}

TEST(MultiGPU, DeviceContextDesc)
{
    DeviceContextDesc DefaultCtxDesc{};
    EXPECT_EQ(DefaultCtxDesc.NodeIndex, 0u);

    DeviceContextDesc Node1Ctx{"Node 1 Graphics Context", COMMAND_QUEUE_TYPE_GRAPHICS, false, 1, 0, 1};
    EXPECT_STREQ(Node1Ctx.Name, "Node 1 Graphics Context");
    EXPECT_EQ(Node1Ctx.QueueType, COMMAND_QUEUE_TYPE_GRAPHICS);
    EXPECT_EQ(Node1Ctx.ContextId, 1u);
    EXPECT_EQ(Node1Ctx.QueueId, 0u);
    EXPECT_EQ(Node1Ctx.NodeIndex, 1u);
}

TEST(MultiGPU, CrossDeviceTransferManager)
{
    CrossDeviceTransferManager TransferMgr;
    EXPECT_FALSE(TransferMgr.IsInitialized());
    EXPECT_EQ(TransferMgr.GetLatency(), 2u);

    TextureDesc TexDesc;
    TexDesc.Type   = RESOURCE_DIM_TEX_2D;
    TexDesc.Width  = 512;
    TexDesc.Height = 512;
    TexDesc.Format = TEX_FORMAT_RGBA8_UNORM;

    // Initialize with nullptr devices (valid for testing state transitions and latency tracking)
    TransferMgr.Init(nullptr, nullptr, TexDesc, 3);
    EXPECT_TRUE(TransferMgr.IsInitialized());
    EXPECT_EQ(TransferMgr.GetLatency(), 3u);

    // Initial frames should fill latency window and return false for readiness
    EXPECT_FALSE(TransferMgr.TransferReadyFrame(nullptr, nullptr, nullptr, 0));
    EXPECT_FALSE(TransferMgr.TransferReadyFrame(nullptr, nullptr, nullptr, 1));

    TransferMgr.Reset();
    EXPECT_FALSE(TransferMgr.IsInitialized());
}

} // namespace
