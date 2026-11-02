# Direct GPU-to-GPU (Peer-to-Peer) Communication in Vulkan

In Vulkan, direct GPU-to-GPU communication (Peer-to-Peer / P2P) without staging data through CPU system RAM is achieved primarily through **Device Groups** (core since Vulkan 1.1 via `VK_KHR_device_group` and `VK_KHR_device_group_creation`) or **External Memory** extensions.

Which approach you use depends on whether the GPUs are linked in a single logical group or operate as independent devices.

---

## Method 1: Vulkan Device Groups (Direct P2P inside a Single `VkDevice`)

A **Device Group** combines multiple physical GPUs (`VkPhysicalDevice`) into a single logical device (`VkDevice`). Once grouped, one GPU can directly read from, write to, or copy memory allocated on another GPU over PCIe or high-speed interconnects (such as NVLink or Infinity Fabric) without touching CPU RAM.

### 1. Enumerate and Create the Grouped Logical Device
Instead of calling `vkEnumeratePhysicalDevices`, query the available groups using `vkEnumeratePhysicalDeviceGroups` and pass the group into `VkDeviceCreateInfo`:

```cpp
// 1. Enumerate physical device groups
uint32_t groupCount = 0;
vkEnumeratePhysicalDeviceGroups(instance, &groupCount, nullptr);
std::vector<VkPhysicalDeviceGroupProperties> groups(
    groupCount, {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES});
vkEnumeratePhysicalDeviceGroups(instance, &groupCount, groups.data());

// Select a group that has more than 1 physical device
VkPhysicalDeviceGroupProperties selectedGroup = groups[0]; 

// 2. Chain VkDeviceGroupDeviceCreateInfo into VkDeviceCreateInfo
VkDeviceGroupDeviceCreateInfo deviceGroupCreateInfo = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,
    .physicalDeviceCount = selectedGroup.physicalDeviceCount,
    .pPhysicalDevices = selectedGroup.physicalDevices
};

VkDeviceCreateInfo deviceCreateInfo = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .pNext = &deviceGroupCreateInfo,
    // ... queueCreateInfos, enabledFeatures, etc.
};

VkDevice device;
vkCreateDevice(selectedGroup.physicalDevices[0], &deviceCreateInfo, nullptr, &device);
```

### 2. Verify Direct P2P Memory Support
Before attempting a direct transfer between GPU `0` and GPU `1`, check if the hardware and driver support peer memory access for a given memory heap:

```cpp
VkPeerMemoryFeatureFlags peerFeatures;
vkGetDeviceGroupPeerMemoryFeatures(
    device,
    heapIndex,
    0, // localDeviceIndex (e.g., GPU 0)
    1, // remoteDeviceIndex (e.g., GPU 1)
    &peerFeatures
);

// Check capabilities:
// VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT / COPY_DST_BIT -> Direct DMA vkCmdCopyBuffer / vkCmdCopyImage
// VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT / GENERIC_DST_BIT -> Direct shader reads/writes across GPUs
```

### 3. Allocate and Bind Resources Across Peer GPUs
To allocate memory on a specific physical GPU inside the device group, chain `VkMemoryAllocateFlagsInfo` with `VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT`:

```cpp
// Allocate memory specifically on GPU 0 (deviceMask = 0b01 = 1)
VkMemoryAllocateFlagsInfo allocFlagsInfo = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
    .flags = VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT,
    .deviceMask = (1 << 0) // Allocate on physical device 0
};

VkMemoryAllocateInfo allocInfo = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .pNext = &allocFlagsInfo,
    .allocationSize = bufferSize,
    .memoryTypeIndex = deviceLocalMemoryTypeIndex
};

VkDeviceMemory gpu0Memory;
vkAllocateMemory(device, &allocInfo, nullptr, &gpu0Memory);
```

When binding memory (`vkBindBufferMemory2` or `vkBindImageMemory2`), pass `VkBindBufferMemoryDeviceGroupInfo` in the `pNext` chain so that GPU 1 knows how to map GPU 0's physical allocation:

```cpp
// Both GPU 0 and GPU 1 point to the physical instance allocated on GPU 0 (index 0)
uint32_t deviceIndices[] = { 0, 0 }; 

VkBindBufferMemoryDeviceGroupInfo bindGroupInfo = {
    .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO,
    .deviceIndexCount = 2,
    .pDeviceIndices = deviceIndices
};

VkBindBufferMemoryInfo bindInfo = {
    .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
    .pNext = &bindGroupInfo,
    .buffer = peerBuffer,
    .memory = gpu0Memory,
    .memoryOffset = 0
};

vkBindBufferMemory2(device, 1, &bindInfo);
```

You can then record a standard `vkCmdCopyBuffer` or `vkCmdCopyImage` using a `deviceMask` (`VkDeviceGroupCommandBufferBeginInfo`) so the DMA engine transfers the data directly over the PCIe bus or interconnect bridge.

---

## Method 2: External Memory Sharing (Separate `VkDevice` Instances or Heterogeneous GPUs)

Device Groups only work if the driver exposes the GPUs in the same group (typically requiring identical GPU architectures from the same vendor, and on Windows, sometimes requiring Linked Display Adapter modes enabled).

If you have **two separate `VkDevice` handles** or different GPUs (e.g., an integrated GPU and a discrete GPU, or two independent workstation cards), you can avoid CPU memory copies by exporting and importing DMA handles using `VK_KHR_external_memory`:

1. **Enable Extensions:**
   * **Linux:** `VK_KHR_external_memory_fd` and `VK_EXT_external_memory_dma_buf`
   * **Windows:** `VK_KHR_external_memory_win32`
2. **Export from GPU A:** Allocate `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` memory on GPU A with `VkExportMemoryAllocateInfo` in the `pNext` chain, then extract the OS handle (`vkGetMemoryFdKHR` on Linux or `vkGetMemoryWin32HandleKHR` on Windows).
3. **Import to GPU B:** Allocate memory on GPU B using `VkImportMemoryFdInfoKHR` (or `VkImportMemoryWin32HandleInfoKHR`) pointing to the exported handle.
4. **Synchronize:** Use `VK_KHR_external_semaphore` (`vkGetSemaphoreFdKHR` / `vkImportSemaphoreFdKHR`) so GPU B waits on hardware synchronization before reading the buffer written by GPU A.

> **Note on PCIe BAR / Resizable BAR:** For External Memory (`dma_buf` / Win32 handle) to transfer directly from GPU A to GPU B over PCIe without bouncing through host RAM, both GPUs must sit on a compatible PCIe root complex, **Resizable BAR (Above 4G Decoding)** should be enabled in BIOS, and the OS kernel/driver must support P2P DMA-BUF routing.

---

## Comparison Summary

| Feature | `VK_KHR_device_group` (Core 1.1) | `VK_KHR_external_memory` (DMA-BUF / Win32) |
| :--- | :--- | :--- |
| **Logical Setup** | Single `VkDevice` managing multiple GPUs | Separate `VkDevice` per GPU |
| **Hardware Requirement** | Identical / vendor-linked GPUs | Any GPUs supporting external handles (even cross-vendor) |
| **Shader Direct Access** | Supported (if `GENERIC_SRC/DST` bit is set) | Limited (usually used with DMA copy queues) |
| **Synchronization** | Standard `VkSemaphore` / `VkEvent` with device masks | Requires `VK_KHR_external_semaphore` |