
#include "vulkandevice.h"
#include "vulkanobjects.h"
#include "vulkancompatibledevice.h"
#include <algorithm>
#include <set>
#include <string>

VulkanDevice::VulkanDevice(std::shared_ptr<VulkanInstance> instance, std::shared_ptr<VulkanSurface> surface, const VulkanCompatibleDevice& selectedDevice) : Instance(instance), Surface(surface)
{
	PhysicalDevice = *selectedDevice.Device;
	EnabledDeviceExtensions = selectedDevice.EnabledDeviceExtensions;
	EnabledFeatures = selectedDevice.EnabledFeatures;

	GraphicsFamily = selectedDevice.GraphicsFamily;
	PresentFamily = selectedDevice.PresentFamily;
	GraphicsTimeQueries = selectedDevice.GraphicsTimeQueries;

	try
	{
		CreateDevice();
		CreateAllocator();
	}
	catch (...)
	{
		ReleaseResources();
		throw;
	}
}

VulkanDevice::~VulkanDevice()
{
	ReleaseResources();
}

bool VulkanDevice::SupportsExtension(const char* ext) const
{
	return
		EnabledDeviceExtensions.find(ext) != EnabledDeviceExtensions.end() ||
		Instance->EnabledExtensions.find(ext) != Instance->EnabledExtensions.end();
}

void VulkanDevice::CreateAllocator()
{
	// VMA is built with VMA_STATIC_VULKAN_FUNCTIONS, which resolves every function from
	// the bare global vkWhatever symbols - the same global, single-current-device volk
	// pointers this whole per-device dispatch table exists to stop relying on. explicitly
	// feeding it this device's own table (below) makes VMA's internal calls immune to a
	// second device's volkLoadDeviceTable() changing what "current" means process-wide.
	// physical-device/instance-level functions are left off: there's only ever one
	// VkInstance for this device's whole lifetime, so those stay safe as globals.
	VmaVulkanFunctions vmaFunctions = {};
	vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
	vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
	vmaFunctions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
	vmaFunctions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
	vmaFunctions.vkAllocateMemory = vk.vkAllocateMemory;
	vmaFunctions.vkFreeMemory = vk.vkFreeMemory;
	vmaFunctions.vkMapMemory = vk.vkMapMemory;
	vmaFunctions.vkUnmapMemory = vk.vkUnmapMemory;
	vmaFunctions.vkFlushMappedMemoryRanges = vk.vkFlushMappedMemoryRanges;
	vmaFunctions.vkInvalidateMappedMemoryRanges = vk.vkInvalidateMappedMemoryRanges;
	vmaFunctions.vkBindBufferMemory = vk.vkBindBufferMemory;
	vmaFunctions.vkBindImageMemory = vk.vkBindImageMemory;
	vmaFunctions.vkGetBufferMemoryRequirements = vk.vkGetBufferMemoryRequirements;
	vmaFunctions.vkGetImageMemoryRequirements = vk.vkGetImageMemoryRequirements;
	vmaFunctions.vkCreateBuffer = vk.vkCreateBuffer;
	vmaFunctions.vkDestroyBuffer = vk.vkDestroyBuffer;
	vmaFunctions.vkCreateImage = vk.vkCreateImage;
	vmaFunctions.vkDestroyImage = vk.vkDestroyImage;
	vmaFunctions.vkCmdCopyBuffer = vk.vkCmdCopyBuffer;
#if VMA_DEDICATED_ALLOCATION || VMA_VULKAN_VERSION >= 1001000
	vmaFunctions.vkGetBufferMemoryRequirements2KHR = vk.vkGetBufferMemoryRequirements2;
	vmaFunctions.vkGetImageMemoryRequirements2KHR = vk.vkGetImageMemoryRequirements2;
#endif
#if VMA_BIND_MEMORY2 || VMA_VULKAN_VERSION >= 1001000
	vmaFunctions.vkBindBufferMemory2KHR = vk.vkBindBufferMemory2;
	vmaFunctions.vkBindImageMemory2KHR = vk.vkBindImageMemory2;
#endif
#if VMA_MEMORY_BUDGET || VMA_VULKAN_VERSION >= 1001000
	vmaFunctions.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;
#endif
#if VMA_VULKAN_VERSION >= 1003000
	vmaFunctions.vkGetDeviceBufferMemoryRequirements = vk.vkGetDeviceBufferMemoryRequirements;
	vmaFunctions.vkGetDeviceImageMemoryRequirements = vk.vkGetDeviceImageMemoryRequirements;
#endif

	VmaAllocatorCreateInfo allocinfo = {};
	allocinfo.vulkanApiVersion = Instance->ApiVersion;
	if (SupportsExtension(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME) && SupportsExtension(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
		allocinfo.flags |= VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
	if (SupportsExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
		allocinfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	allocinfo.physicalDevice = PhysicalDevice.Device;
	allocinfo.device = device;
	allocinfo.instance = Instance->Instance;
	allocinfo.preferredLargeHeapBlockSize = 64 * 1024 * 1024;
	allocinfo.pVulkanFunctions = &vmaFunctions;
	if (vmaCreateAllocator(&allocinfo, &allocator) != VK_SUCCESS)
		VulkanError("Unable to create allocator");
}

void VulkanDevice::CreateDevice()
{
	float queuePriority = 1.0f;
	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

	std::set<int> neededFamilies;
	if (GraphicsFamily != -1)
		neededFamilies.insert(GraphicsFamily);
	if (PresentFamily != -1)
		neededFamilies.insert(PresentFamily);

	for (int index : neededFamilies)
	{
		VkDeviceQueueCreateInfo queueCreateInfo = {};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = index;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(queueCreateInfo);
	}

	std::vector<const char*> extensionNames;
	extensionNames.reserve(EnabledDeviceExtensions.size());
	for (const auto& name : EnabledDeviceExtensions)
		extensionNames.push_back(name.c_str());

	VkDeviceCreateInfo deviceCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	deviceCreateInfo.queueCreateInfoCount = (uint32_t)queueCreateInfos.size();
	deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
	deviceCreateInfo.enabledExtensionCount = (uint32_t)extensionNames.size();
	deviceCreateInfo.ppEnabledExtensionNames = extensionNames.data();
	deviceCreateInfo.enabledLayerCount = 0;

	VkPhysicalDeviceFeatures2 deviceFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	deviceFeatures2.features = EnabledFeatures.Features;

	void** next = const_cast<void**>(&deviceCreateInfo.pNext);
	if (SupportsExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
	{
		*next = &deviceFeatures2;
		next = &deviceFeatures2.pNext;
	}
	else // vulkan 1.0 specified features in a different way
	{
		deviceCreateInfo.pEnabledFeatures = &deviceFeatures2.features;
	}

	if (SupportsExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
	{
		*next = &EnabledFeatures.BufferDeviceAddress;
		next = &EnabledFeatures.BufferDeviceAddress.pNext;
	}
	if (SupportsExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME))
	{
		*next = &EnabledFeatures.AccelerationStructure;
		next = &EnabledFeatures.AccelerationStructure.pNext;
	}
	if (SupportsExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME))
	{
		*next = &EnabledFeatures.RayQuery;
		next = &EnabledFeatures.RayQuery.pNext;
	}
	if (SupportsExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
	{
		*next = &EnabledFeatures.DescriptorIndexing;
		next = &EnabledFeatures.DescriptorIndexing.pNext;
	}

	VkResult result = vkCreateDevice(PhysicalDevice.Device, &deviceCreateInfo, nullptr, &device);
	CheckVulkanError(result, "Could not create vulkan device");

	// per-device table, not volkLoadDevice()'s global one - see the comment on the `vk`
	// member in the header for why.
	volkLoadDeviceTable(&vk, device);

	if (GraphicsFamily != -1)
		vk.vkGetDeviceQueue(device, GraphicsFamily, 0, &GraphicsQueue);
	if (PresentFamily != -1)
		vk.vkGetDeviceQueue(device, PresentFamily, 0, &PresentQueue);
}

void VulkanDevice::ReleaseResources()
{
	if (device)
		vk.vkDeviceWaitIdle(device);

	if (allocator)
		vmaDestroyAllocator(allocator);

	if (device)
		vk.vkDestroyDevice(device, nullptr);
	device = nullptr;
}

void VulkanDevice::SetObjectName(const char* name, uint64_t handle, VkObjectType type)
{
	if (!DebugLayerActive) return;

	VkDebugUtilsObjectNameInfoEXT info = {};
	info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	info.objectHandle = handle;
	info.objectType = type;
	info.pObjectName = name;
	// vkSetDebugUtilsObjectNameEXT is dispatched via the instance despite naming a device
	// object (a known VK_EXT_debug_utils quirk) - it's not in VolkDeviceTable at all, only
	// the instance-loaded globals, so this one stays as the bare global call.
	vkSetDebugUtilsObjectNameEXT(device, &info);
}
