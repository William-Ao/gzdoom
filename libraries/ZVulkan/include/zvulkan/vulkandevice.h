#pragma once

#include "vulkaninstance.h"

#include <functional>
#include <mutex>
#include <vector>
#include <algorithm>
#include <memory>

class VulkanSwapChain;
class VulkanSemaphore;
class VulkanFence;
class VulkanPhysicalDevice;
class VulkanSurface;
class VulkanCompatibleDevice;

class VulkanDevice
{
public:
	VulkanDevice(std::shared_ptr<VulkanInstance> instance, std::shared_ptr<VulkanSurface> surface, const VulkanCompatibleDevice& selectedDevice);
	~VulkanDevice();

	std::set<std::string> EnabledDeviceExtensions;
	VulkanDeviceFeatures EnabledFeatures;

	VulkanPhysicalDevice PhysicalDevice;

	std::shared_ptr<VulkanInstance> Instance;
	std::shared_ptr<VulkanSurface> Surface;

	VkDevice device = VK_NULL_HANDLE;
	VmaAllocator allocator = VK_NULL_HANDLE;

	// per-device dispatch table. volk's plain vkWhatever(...) globals only ever point at
	// the most recently created device - fine for one device, silently wrong the moment a
	// second VkDevice exists in the same process. every device-level call in this library
	// (and gzdoom's own vulkan backend) goes through device->vk.vkWhatever(...) instead so
	// two live devices can coexist without corrupting each other's calls.
	VolkDeviceTable vk = {};

	VkQueue GraphicsQueue = VK_NULL_HANDLE;
	VkQueue PresentQueue = VK_NULL_HANDLE;

	int GraphicsFamily = -1;
	int PresentFamily = -1;
	bool GraphicsTimeQueries = false;

	bool SupportsExtension(const char* ext) const;

	void SetObjectName(const char* name, uint64_t handle, VkObjectType type);

private:
	bool DebugLayerActive = false;

	void CreateDevice();
	void CreateAllocator();
	void ReleaseResources();
};
