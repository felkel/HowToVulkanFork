/* Copyright (c) 2025-2026, Sascha Willems
 * SPDX-License-Identifier: MIT
 *
 * Comments and slight modifications: Petr Felkel, 2026
 */

#define VOLK_IMPLEMENTATION
#include <vulkan/vulkan.h>
#include <volk/volk.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vector>
#include <array>
#include <string>
#include <iostream>
#include <filesystem>
#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include "slang/slang.h"
#include "slang/slang-com-ptr.h"
#include <ktx.h>
#include <ktxvulkan.h>
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

// Constants
constexpr uint32_t maxFramesInFlight{ 2 }; // two buffers - front and back

// global variables
uint32_t imageIndex{ 0 }; // got from the swapchain on line 207
uint32_t frameIndex{ 0 }; // 0 or 1, used to select the current frame's resources
                          // (shaderDataBuffers[frameIndex], fences[frameIndex], imageAcquiredSemaphores[frameIndex])

// Vulkan objects
VkInstance instance{ VK_NULL_HANDLE };
VkDevice device{ VK_NULL_HANDLE };
VkQueue queue{ VK_NULL_HANDLE }; // got on line 207 (queue family 161-207)
VkSurfaceKHR surface{ VK_NULL_HANDLE };  // parameter for swapchain
bool updateSwapchain{ false };
VkSwapchainKHR swapchain{ VK_NULL_HANDLE };
VkCommandPool commandPool{ VK_NULL_HANDLE };
VkPipeline pipeline{ VK_NULL_HANDLE };
VkPipelineLayout pipelineLayout{ VK_NULL_HANDLE };

// VMA allocator for images (depthImage, texture) and buffers (vBuffer and shader buffers)
VmaAllocator allocator{ VK_NULL_HANDLE };

// single depth buffer
VkImage depthImage;
VmaAllocation depthImageAllocation;
VkImageView depthImageView;

// swapchain - uses [imageIndex] to select the current image for rendering
std::vector<VkImage> swapchainImages; // 2, got from swapchain, passed to views
std::vector<VkImageView> swapchainImageViews;

// structures for shader data for each frame in flight [frameIndex]
//   command buffers[frameIndex]
std::array<VkCommandBuffer, maxFramesInFlight> commandBuffers;

//   fences[frameIndex] - tell CPU when GPU has finished the commands in the commandBuffer[] and can change uniforms, create new queue
std::array<VkFence, maxFramesInFlight> fences; 

std::array<VkSemaphore, maxFramesInFlight> imageAcquiredSemaphores;  // imageAcquiredSemaphores
std::vector<VkSemaphore> renderCompleteSemaphores;  // renderCompleteSemaphores[imageIndex] GPU finished rendering the frame
VmaAllocation vBufferAllocation{ VK_NULL_HANDLE };

// buffer for Suzanne geometry and indices
VkBuffer vBuffer{ VK_NULL_HANDLE };

//
struct ShaderData {
  glm::mat4 projection;
  glm::mat4 view;
  glm::mat4 model[3]; //for 3 instances
  glm::vec4 lightPos{ 0.0f, -10.0f, 10.0f, 0.0f };
  uint32_t selected{ 1 }; // selected model {0,1,2}
} shaderData{};
struct ShaderDataBuffer {
	VmaAllocation allocation{ VK_NULL_HANDLE };
	VmaAllocationInfo allocationInfo{};
	VkBuffer buffer{ VK_NULL_HANDLE };
	VkDeviceAddress deviceAddress{};
};
std::array<ShaderDataBuffer, maxFramesInFlight> shaderDataBuffers;

// textures
struct Texture {
	VmaAllocation allocation{ VK_NULL_HANDLE };
	VkImage image{ VK_NULL_HANDLE };
	VkImageView view{ VK_NULL_HANDLE };
	VkSampler sampler{ VK_NULL_HANDLE };
};
std::array<Texture, 3> textures{};

// Descriptor set layout and pool for texture sampler
VkDescriptorPool descriptorPool{ VK_NULL_HANDLE };
VkDescriptorSetLayout descriptorSetLayoutTex{ VK_NULL_HANDLE };
VkDescriptorSet descriptorSetTex{ VK_NULL_HANDLE };

// Slang session for compiling shaders
Slang::ComPtr<slang::IGlobalSession> slangGlobalSession;

// camera and object rotations
glm::vec3 camPos{ 0.0f, 0.0f, -6.0f };
glm::vec3 objectRotations[3]{};
glm::ivec2 windowSize{};

// vertex attributes in vBuffer / shader input
struct Vertex {
	glm::vec3 pos;
	glm::vec3 normal;
	glm::vec2 uv;
};


static inline void chkFence(VkResult result, int n=-1) {
  std::cerr << "The fence";
  if (n != -1)
    std::cerr << "[" << n << "]";
  switch (result) {
  case VK_SUCCESS:
    std::cerr << " is signaled" << "\n";
    break;
  case VK_NOT_READY:
    std::cerr << " is unsignaled" << "\n";
    break;
  }
}


// methods for checking results (of type VkResult and bool)
static inline void chk(VkResult result) {
	if (result != VK_SUCCESS) {
		std::cerr << "Vulkan call returned an error (" << result << ")\n";
		exit(result);
	}
}
static inline void chkSwapchain(VkResult result) {
	if (result < VK_SUCCESS) {
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			updateSwapchain = true;
			return;
		}
		std::cerr << "Vulkan call returned an error (" << result << ")\n";
		exit(result);
	}
}
static inline void chk(bool result) {
	if (!result) {
		std::cerr << "Call returned an error\n";
		exit(result);
	}
}

// huge main
int main(int argc, char* argv[])
{
	// Make sure asset folder is present from the current working directory
	if (!std::filesystem::is_directory("assets")) {
		std::cerr << "Could not locate assets folder from current working directory\n";
		exit(-1);
	}
	chk(SDL_Init(SDL_INIT_VIDEO));
	chk(SDL_Vulkan_LoadLibrary(NULL));
	volkInitialize();
	// 1. Instance
	VkApplicationInfo appInfo{ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "How to Vulkan", .apiVersion = VK_API_VERSION_1_3 };
	uint32_t instanceExtensionsCount{ 0 };
	char const* const* instanceExtensions{ SDL_Vulkan_GetInstanceExtensions(&instanceExtensionsCount) };
	VkInstanceCreateInfo instanceCI{
	  .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
		.enabledExtensionCount = instanceExtensionsCount,
		.ppEnabledExtensionNames = instanceExtensions,
	};
	chk(vkCreateInstance(&instanceCI, nullptr, &instance));
	volkLoadInstance(instance);

  //VkInstance instance2{ VK_NULL_HANDLE };
  //chk(vkCreateInstance(&instanceCI, nullptr, &instance2));
  //volkLoadInstance(instance2);


  // 2. Device
	uint32_t deviceCount{ 0 };
	chk(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr));
	std::vector<VkPhysicalDevice> devices(deviceCount);
	chk(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()));
	uint32_t deviceIndex{ 0 };
	if (argc > 1) {
		deviceIndex = std::stoi(argv[1]);
		assert(deviceIndex < deviceCount);
	}
	VkPhysicalDeviceProperties2 deviceProperties{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	vkGetPhysicalDeviceProperties2(devices[deviceIndex], &deviceProperties); // p139
	std::cout << "Selected device: " << deviceProperties.properties.deviceName << "\n";

	// 3. Find a queue family for graphics -----------------------------------------> queueFamily
	uint32_t queueFamilyCount{ 0 };
	vkGetPhysicalDeviceQueueFamilyProperties(devices[deviceIndex], &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(devices[deviceIndex], &queueFamilyCount, queueFamilies.data());
	uint32_t queueFamily{ 0 };
	for (size_t i = 0; i < queueFamilies.size(); i++) {
		if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			queueFamily = i;
			break;
		}
	}
  // on Windows: bool xx = vkGetPhysicalDeviceWin32PresentationSupportKHR(devices[deviceIndex], queueFamily);
  // multiplatform:
	chk(SDL_Vulkan_GetPresentationSupport(instance, devices[deviceIndex], queueFamily));

	// 4. Logical device
	const float qfpriorities{ 1.0f };
  VkDeviceQueueCreateInfo queueCI{ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = queueFamily, .queueCount = 1, .pQueuePriorities = &qfpriorities };

	VkPhysicalDeviceVulkan12Features enabledVk12Features{
	  .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
	  .descriptorIndexing = true,
	  .shaderSampledImageArrayNonUniformIndexing = true,
	  .descriptorBindingVariableDescriptorCount = true,
    .runtimeDescriptorArray = true, // enable variable descriptor count, texture[] in shader, and
	                                  //        descriptorCount > 1 in VkDescriptorSetLayoutBinding
	  .bufferDeviceAddress = true };  // enable BDA on logical device
	VkPhysicalDeviceVulkan13Features enabledVk13Features{
	  .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
	  .pNext = &enabledVk12Features,
	  .synchronization2 = true,
	  .dynamicRendering = true };
	VkPhysicalDeviceFeatures enabledVk10Features{
	  .samplerAnisotropy = VK_TRUE };
	const std::vector<const char*> deviceExtensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };

	VkDeviceCreateInfo deviceCI{
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &enabledVk13Features,  // deviceCI -> 13 -> 12
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueCI,  //
		.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
		.ppEnabledExtensionNames = deviceExtensions.data(), // VK_KHR_swapchain
		.pEnabledFeatures = &enabledVk10Features  // 10
	};
	chk(vkCreateDevice(devices[deviceIndex], &deviceCI, nullptr, &device)); // p17
	vkGetDeviceQueue(device, queueFamily, 0, &queue);  // really queue 0?

  //--------------------------------------------------------------------------------------
	// 5. VMA --> 8,9,12
	VmaVulkanFunctions vkFunctions{
	  .vkGetInstanceProcAddr = vkGetInstanceProcAddr,  // already set in init?
	  .vkGetDeviceProcAddr = vkGetDeviceProcAddr,      // already set in init?
	  .vkCreateImage = vkCreateImage };

	VmaAllocatorCreateInfo allocatorCI{
	  .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,  // VK_KHR_buffer_device_address, GPU pointers
	  .physicalDevice = devices[deviceIndex],
	  .device = device,
	  .pVulkanFunctions = &vkFunctions,
	  .instance = instance };
	chk(vmaCreateAllocator(&allocatorCI, &allocator));

  //--------------------------------------------------------------------------------------
  // 6. Window and surface
SDL_Window* window = SDL_CreateWindow("How to Vulkan (SDL)", 1280u, 720u, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
	assert(window);
	chk(SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface));
	chk(SDL_GetWindowSize(window, &windowSize.x, &windowSize.y));

	VkSurfaceCapabilitiesKHR surfaceCaps{};
	chk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(devices[deviceIndex], surface, &surfaceCaps));

	VkExtent2D swapchainExtent{ surfaceCaps.currentExtent };
	if (surfaceCaps.currentExtent.width == 0xFFFFFFFF) {  // why?
		swapchainExtent = { .width = static_cast<uint32_t>(windowSize.x), .height = static_cast<uint32_t>(windowSize.y) };
	}

	// 7. Swap chain

  // .present mode tests:
  // https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentModeKHR.html#
  // more swapchain images - 2,3,4,5,6,7,8 - increase latency for FIFO_KHR
   uint32_t desiredImageCount{ 3 }; // was 2, but this will be different than maxFramesInFlight=2
   desiredImageCount = std::max(desiredImageCount, surfaceCaps.minImageCount);
   if (surfaceCaps.maxImageCount > 0) {  // 0 means no limits, only total amount of memory
     desiredImageCount = std::min(desiredImageCount, surfaceCaps.maxImageCount);
   }
  // on Intel UHD - all modes made a huge latency for .minImageCount = 64, none of them dropped any frame.
  // all cached all movements
  // on NVIDIA, 8 images max, all were fast, latency not visible
  //
  // .minImageCount = desiredImageCount,
  // .presentMode = VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR,
  // .presentMode = VK_PRESENT_MODE_MAILBOX_KHR, // This should drop frames, but huge latency, 64 frames in flight, no tearing, vsync, but can drop frames but doesn't do it
  //.presentMode = VK_PRESENT_MODE_FIFO_LATEST_READY_KHR, // latest ready image, no tearing, vsync, 1 frame latency


	const VkFormat imageFormat{ VK_FORMAT_B8G8R8A8_SRGB };
  VkSwapchainCreateInfoKHR swapchainCI{
    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
    .surface = surface,
    //.minImageCount = surfaceCaps.minImageCount, // from range 2..8 on NVIDIA, Intel has 2..64 
    .minImageCount = desiredImageCount,
    .imageFormat = imageFormat,
		.imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR,
		.imageExtent{.width = swapchainExtent.width, .height = swapchainExtent.height },
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    .presentMode = VK_PRESENT_MODE_FIFO_KHR // required, no tearing, vsync, 1 frame latency, 2 frames in flight
	};
	chk(vkCreateSwapchainKHR(device, &swapchainCI, nullptr, &swapchain));

	uint32_t imageCount{ 0 };
	chk(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr));
	swapchainImages.resize(imageCount);
	chk(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data()));

	swapchainImageViews.resize(imageCount);
	for (auto i = 0; i < imageCount; i++) {
		VkImageViewCreateInfo viewCI{
		  .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		  .image = swapchainImages[i],
		  .viewType = VK_IMAGE_VIEW_TYPE_2D,
		  .format = imageFormat,
		  .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 }
		};
		chk(vkCreateImageView(device, &viewCI, nullptr, &swapchainImageViews[i]));
	}

	// 8. Depth attachment
	std::vector<VkFormat> depthFormatList{
	  VK_FORMAT_D32_SFLOAT_S8_UINT,
	  VK_FORMAT_D24_UNORM_S8_UINT
	}; // one must be supported - select format in loop

	VkFormat depthFormat{ VK_FORMAT_UNDEFINED };
	for (VkFormat& format : depthFormatList) {
		VkFormatProperties2 formatProperties{ .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
		vkGetPhysicalDeviceFormatProperties2(devices[deviceIndex], format, &formatProperties);
		if (formatProperties.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
			depthFormat = format;
			break;
		}
	}
	assert(depthFormat != VK_FORMAT_UNDEFINED);

	VkImageCreateInfo depthImageCI{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = depthFormat,
		.extent{.width = static_cast<uint32_t>(windowSize.x), .height = static_cast<uint32_t>(windowSize.y), .depth = 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VmaAllocationCreateInfo allocCI{ .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT, .usage = VMA_MEMORY_USAGE_AUTO };
	chk(vmaCreateImage(allocator, &depthImageCI, &allocCI, &depthImage, &depthImageAllocation, nullptr));
	VkImageViewCreateInfo depthViewCI{
	  .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	  .image = depthImage,
	  .viewType = VK_IMAGE_VIEW_TYPE_2D,
	  .format = depthFormat,
	  .subresourceRange{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1 } };
	chk(vkCreateImageView(device, &depthViewCI, nullptr, &depthImageView));

	// 8. Mesh data:  .obj file ---> vBuffer
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	chk(tinyobj::LoadObj(&attrib, &shapes, &materials, nullptr, nullptr, "assets/suzanne.obj"));
	const VkDeviceSize indexCount{ shapes[0].mesh.indices.size() };
	std::vector<Vertex> vertices{};
	std::vector<uint16_t> indices{};
	// Load vertex and index data into vBuffer
	for (auto& index : shapes[0].mesh.indices) {
		Vertex v{
			.pos = { attrib.vertices[index.vertex_index * 3], -attrib.vertices[index.vertex_index * 3 + 1], attrib.vertices[index.vertex_index * 3 + 2] },
			.normal = { attrib.normals[index.normal_index * 3], -attrib.normals[index.normal_index * 3 + 1], attrib.normals[index.normal_index * 3 + 2] },
			.uv = { attrib.texcoords[index.texcoord_index * 2], 1.0 - attrib.texcoords[index.texcoord_index * 2 + 1] }
		};
		vertices.push_back(v);
		indices.push_back(indices.size()); // starts indexing from 0, and ignores indices in .obj that are the same
	}
	VkDeviceSize vBufSize{ sizeof(Vertex) * vertices.size() };
	VkDeviceSize iBufSize{ sizeof(uint16_t) * indices.size() };
	VkBufferCreateInfo bufferCI{
	  .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	  .size = vBufSize + iBufSize,
	  .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT };
	VmaAllocationCreateInfo vBufferAllocCI{
	  .flags =   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
	           | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT
	           | VMA_ALLOCATION_CREATE_MAPPED_BIT,
	  .usage = VMA_MEMORY_USAGE_AUTO
	};
	VmaAllocationInfo vBufferAllocInfo{};
	chk(vmaCreateBuffer(allocator, &bufferCI, &vBufferAllocCI, &vBuffer, &vBufferAllocation, &vBufferAllocInfo));

	memcpy(vBufferAllocInfo.pMappedData, vertices.data(), vBufSize);
	memcpy(((char*)vBufferAllocInfo.pMappedData) + vBufSize, indices.data(), iBufSize);


	// 9. Shader data buffers uBuffer
  //    per frame
  //    accessed through deviceAddress pointer
  //    BDA - buffer device address / shader device address

  // both infos are the same for all frames - I moved both out of the loop
  VkBufferCreateInfo uBufferCI{
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = sizeof(ShaderData),
    .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT  // access this buffer via its device address - BDA
  };
  VmaAllocationCreateInfo uBufferAllocCI{
    .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
           | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT // uncached and write-combined memory type
           | VMA_ALLOCATION_CREATE_MAPPED_BIT, // mapped persistently to host address space
    .usage = VMA_MEMORY_USAGE_AUTO
  };
  for (auto i = 0; i < maxFramesInFlight; i++) {
 
    chk(vmaCreateBuffer(allocator, &uBufferCI, &uBufferAllocCI, 
      &shaderDataBuffers[i].buffer, // VkBuffer
      &shaderDataBuffers[i].allocation, // handle
      &shaderDataBuffers[i].allocationInfo // type, size, offset, VkMemory, pMappedData 
    ));

    VkBufferDeviceAddressInfo uBufferBdaInfo{
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = shaderDataBuffers[i].buffer
    };
    shaderDataBuffers[i].deviceAddress = vkGetBufferDeviceAddress(device, &uBufferBdaInfo);  // uint64_t
  }

	// 10. Sync objects
	VkSemaphoreCreateInfo semaphoreCI{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo fenceCI{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
	for (auto i = 0; i < maxFramesInFlight; i++) {
		chk(vkCreateFence(device, &fenceCI, nullptr, &fences[i]));
		chk(vkCreateSemaphore(device, &semaphoreCI, nullptr, &imageAcquiredSemaphores[i]));
	}
	renderCompleteSemaphores.resize(swapchainImages.size());
	for (auto& semaphore : renderCompleteSemaphores) {
		chk(vkCreateSemaphore(device, &semaphoreCI, nullptr, &semaphore));
	}

	// 11. Command pool
	VkCommandPoolCreateInfo commandPoolCI{
	  .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, // implicit reset on vkBeginCommandBuffer()
	  .queueFamilyIndex = queueFamily
	};
	chk(vkCreateCommandPool(device, &commandPoolCI, nullptr, &commandPool));

  // Command buffers for each frame
	VkCommandBufferAllocateInfo cbAllocCI{
	  .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	  .commandPool = commandPool,
	  .commandBufferCount = maxFramesInFlight
	};
	chk(vkAllocateCommandBuffers(device, &cbAllocCI, commandBuffers.data()));
    

  //----------------------------- TEXTURES ------------------------------------

  // 12. Texture images(suzanne0, 1, and 2)
  //    a) Load Texture from file
	std::vector<VkDescriptorImageInfo> textureDescriptors{};
	for (auto i = 0; i < textures.size(); i++) {
		ktxTexture* ktxTexture{ nullptr };
		std::string filename = "assets/suzanne" + std::to_string(i) + ".ktx";
		ktxTexture_CreateFromNamedFile(filename.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktxTexture);

    //     b) create image + image view
    VkImageCreateInfo texImgCI{
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = ktxTexture_GetVkFormat(ktxTexture),
			.extent = {.width = ktxTexture->baseWidth, .height = ktxTexture->baseHeight, .depth = 1 },
			.mipLevels = ktxTexture->numLevels,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,    // DST
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
		};
    
		VmaAllocationCreateInfo texImageAllocCI{
		  .usage = VMA_MEMORY_USAGE_AUTO
		};
		chk(vmaCreateImage(allocator, &texImgCI, &texImageAllocCI, &textures[i].image, &textures[i].allocation, nullptr));

		VkImageViewCreateInfo texViewCI{
		  .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		  .image = textures[i].image,  // just created
		  .viewType = VK_IMAGE_VIEW_TYPE_2D,  // repeat
		  .format = texImgCI.format, // repeat
		  .subresourceRange = {   // repeat - all levels
		    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		    .levelCount = ktxTexture->numLevels,
		    .layerCount = 1
		  }
		};
		chk(vkCreateImageView(device, &texViewCI, nullptr, &textures[i].view));

	  // Upload
    //     c) copy texture to temporary buffer   
		VkBuffer imgSrcBuffer{};
		VmaAllocation imgSrcAllocation{};
		VkBufferCreateInfo imgSrcBufferCI{
		  .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (uint32_t)ktxTexture->dataSize,
		  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT }; //p43

		VmaAllocationCreateInfo imgSrcAllocCI{
		  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |  // uncached and write-combined memory type
               VMA_ALLOCATION_CREATE_MAPPED_BIT,  // mapped to host address space  - can use memcpy()
		  .usage = VMA_MEMORY_USAGE_AUTO };           // must have .flags access_sew_wright or random

	  VmaAllocationInfo imgSrcAllocInfo{};
    chk(vmaCreateBuffer(allocator, &imgSrcBufferCI, &imgSrcAllocCI, 
                              &imgSrcBuffer, &imgSrcAllocation, &imgSrcAllocInfo));

		memcpy(imgSrcAllocInfo.pMappedData, ktxTexture->pData, ktxTexture->dataSize);

    //     d) create command buffer and fence for waiting for cmdBuffer finish execution
		VkFenceCreateInfo fenceOneTimeCI{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		VkFence fenceOneTime{};
		chk(vkCreateFence(device, &fenceOneTimeCI, nullptr, &fenceOneTime));

    // One time command buffer for texture loading
		VkCommandBuffer cbOneTime{};
		VkCommandBufferAllocateInfo cbOneTimeAI{
		  .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		  .commandPool = commandPool,
		  .commandBufferCount = 1
		};
		chk(vkAllocateCommandBuffers(device, &cbOneTimeAI, &cbOneTime));

    //    e) Start recording commands to command buffer 
		VkCommandBufferBeginInfo cbOneTimeBI{
		  .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
		};
	  chk(vkBeginCommandBuffer(cbOneTime, &cbOneTimeBI));

    //    f) First Barrier
	  //       stage/access: NONE/NONE ---> TRANSFER/WRITE, 
    //       layout:       UNDEFINED ---> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier2 barrierTexImage{ // NONE/NONE ---> TRANSFER/WRITE
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
			.srcAccessMask = VK_ACCESS_2_NONE,
			.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,  //0x00001000ULL
			.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.image = textures[i].image,
			.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = ktxTexture->numLevels, .layerCount = 1 }
		};
		VkDependencyInfo barrierTexInfo{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		  .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrierTexImage };
    vkCmdPipelineBarrier2(cbOneTime, &barrierTexInfo);


    //    g) copy the mip map levels from buffer to the textures[i].image
		std::vector<VkBufferImageCopy> copyRegions{};
		for (auto j = 0; j < ktxTexture->numLevels; j++) {
			ktx_size_t mipOffset{0};
			KTX_error_code ret = ktxTexture_GetImageOffset(ktxTexture, j, 0, 0, &mipOffset);
			copyRegions.push_back({
				.bufferOffset = mipOffset,
				.imageSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = (uint32_t)j, .layerCount = 1},
				.imageExtent{.width = ktxTexture->baseWidth >> j, .height = ktxTexture->baseHeight >> j, .depth = 1 },
			});
		}
    // p.128
		vkCmdCopyBufferToImage(cbOneTime, imgSrcBuffer, textures[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copyRegions.size()), copyRegions.data());

    //     h) Second barrier
    //        stage/access: TRANSFER/WRITE ---> FRAGMENT_SHADER/READ
	  //              layout: TRANSFER_DST_OPTIMAL ---> READ_ONLY_OPTIMAL
    // Transition the mip levels from transfer destination to a layout we can read from in our shader

		VkImageMemoryBarrier2 barrierTexRead{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, // 64bit constant
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,  // was enum VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
			.image = textures[i].image,
			.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = ktxTexture->numLevels, .layerCount = 1 }
		};
		barrierTexInfo.pImageMemoryBarriers = &barrierTexRead;
	  vkCmdPipelineBarrier2(cbOneTime, &barrierTexInfo);

    //     i) end cmd buffer
	  chk(vkEndCommandBuffer(cbOneTime));

    //     j) submit the commands and wait for finishing
		VkSubmitInfo oneTimeSI{
		  .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		  .commandBufferCount = 1,
		  .pCommandBuffers = &cbOneTime
		};
		chk(vkQueueSubmit(queue, 1, &oneTimeSI, fenceOneTime));
		chk(vkWaitForFences(device, 1, &fenceOneTime, VK_TRUE, UINT64_MAX));

    //     k) clean up fence and buffer
		vkDestroyFence(device, fenceOneTime, nullptr);
		vmaDestroyBuffer(allocator, imgSrcBuffer, imgSrcAllocation);

		// 13. Sampler
		VkSamplerCreateInfo samplerCI{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
			.anisotropyEnable = VK_TRUE,
			.maxAnisotropy = 8.0f,  // 8 is a widely supported value for max anisotropy
			.maxLod = (float)ktxTexture->numLevels,  // use all levels
		};
		chk(vkCreateSampler(device, &samplerCI, nullptr, &textures[i].sampler));

    // clean up read ktx texture and store the descriptor
		ktxTexture_Destroy(ktxTexture);

		textureDescriptors.push_back({   // VkDescriptorImageInfo
      .sampler = textures[i].sampler,
		  .imageView = textures[i].view,
		  .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL
		});
	} // end for each of 3 textures


  //---------------------------- DESCRIPTORS ----------------------------------

  // 14. Descriptor (indexing)

  // a) Descriptor Set Layout
  //    = INTERFACE between application and shader for accessing resources (textures, buffers, etc.)
  //    slot description - no actual data (updated in descriptor set by vkUpdateDescriptorSets in d)
  //    FS will access 3 textures as combined image + sampler, binding = 0, type = combined image + sampler

  // Here we use a variable-sized descriptor binding (VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT),
  // which allows us to change the number of textures in the descriptor set at runtime.
  // The maximum number of textures is set to 3/16 in the VkDescriptorSetLayoutBinding structure, but we can
  // specify a different number when allocating the descriptor set.

  // For fixed number of textures, drop the flag and use just .descriptorCount = 3 in VkDescriptorSetLayoutBinding
  // - and declare texture [3] in shader
  // - texture[] in shader requires VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT flag
  //    and runtimeDescriptorArray enabled in VkPhysicalDeviceVulkan12Features when creating the logical device

  // This flag can be omitted: just to show the bind-less pattern. We use 3 textures and do not change it
	VkDescriptorBindingFlags descVariableFlag{
    // a variable-sized descriptor binding
    // its size will be specified when a descriptor set is allocated using this layout in c) 
    VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT 
	};

  // array of VkDescriptorBindingFlags structures, one for each binding in the layout
  VkDescriptorSetLayoutBindingFlagsCreateInfo descBindingFlags{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
    .bindingCount = 1,
    .pBindingFlags = &descVariableFlag
  };

  // slot description - no actual data (updated in descriptor set by vkUpdateDescriptorSets)
  // stage = FS, binding = 0, type = combined image + sampler, count = FS will access maximum of 3 textures
  VkDescriptorSetLayoutBinding descLayoutBindingTex{ 
    .binding = 0,  // binding number in shader - that was missing -- implicit 0
    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .descriptorCount = 16, //static_cast<uint32_t>(textures.size()),  // maximum of 3(16) textures
    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT // which shader stage(s) can access this descriptor
  };

  // array of VkDescriptorSetLayoutBinding structures, one for each binding in the layout - here just one 
	VkDescriptorSetLayoutCreateInfo descLayoutTexCI{
	  .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .pNext = &descBindingFlags,  // array of variable-sized descriptor binding - here just one 
	  .bindingCount = 1,
	  .pBindings = &descLayoutBindingTex };

  chk(vkCreateDescriptorSetLayout(device, &descLayoutTexCI, nullptr, &descriptorSetLayoutTex));


  // b) descriptor pool
  //    - pool for allocating maxSets descriptor sets
  //    - .poolSizeCount is the number of elements in pPoolSizes array just updated
  //    - each set can hold .descriptorCount_i descriptors of a given type
  //    - here we have a pool with a single descriptor pool with 3 textures as combined image + sampler

  // Size of space for a single descriptor set with 3 textures as combined image + sampler
	VkDescriptorPoolSize poolSize{
	  .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	  .descriptorCount = static_cast<uint32_t>(textures.size()) // 3x textures
	};
  // array of VkDescriptorPoolSize structures
  // - here just one for a single descriptor set with 3 textures
	VkDescriptorPoolCreateInfo descPoolCI{
	  .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = 1, // maximum number of descriptor sets the pool can hold (here just one)
    .poolSizeCount = 1, // number of elements in pPoolSizes array
    .pPoolSizes = &poolSize  // space for a three image samplers in a single descriptor set
	};
  // pool of descriptors with a single descriptor set. The set can hold 3 textures as combined image + sampler 
	chk(vkCreateDescriptorPool(device, &descPoolCI, nullptr, &descriptorPool));


  // c) Allocate descriptor sets

  // array with the USED number of descriptors in the set - 1set, max 3 textures
  // - if we want to change the number of textures, we can change this variable,
  // - 3 (16) is maximum - set in VkDescriptorSetLayoutBinding descLayoutBindingTex.descriptorCount=3 (16) in a)
	uint32_t variableDescCount{ static_cast<uint32_t>(textures.size()) };  // 3 textures of maximum
	VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescCountAI{
	  .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT,
	  .descriptorSetCount = 1,
    .pDescriptorCounts = &variableDescCount  // << fix the variable number of descriptors in the set - 3 textures
	};
  // array of SetLayout structures - here just one for a single descriptor set with 3 textures
	VkDescriptorSetAllocateInfo texDescSetAlloc{
	  .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .pNext = &variableDescCountAI, // if nNext forgotten, the descriptor set will have 0 texture instead of 3
	  .descriptorPool = descriptorPool,
	  .descriptorSetCount = 1,
	  .pSetLayouts = &descriptorSetLayoutTex
	};
	chk(vkAllocateDescriptorSets(device, &texDescSetAlloc, &descriptorSetTex));

  // d) Update descriptor set = write the actual data (sampler, imageView, imageLayout) for each of 3 textures
	VkWriteDescriptorSet writeDescSet{
	  .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	  .dstSet = descriptorSetTex,
	  .dstBinding = 0,
    .descriptorCount = static_cast<uint32_t>(textureDescriptors.size()), // 3 textures
	  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .pImageInfo = textureDescriptors.data() // array of (sampler, imageView, imageLayout) for each of 3 textures
	};
	vkUpdateDescriptorSets(device, 1, &writeDescSet, 0, nullptr);

  //------------------------------- SHADERS -------------------------------------

  // 15. Initialize Slang shader compiler
  // a) create a global Slang session - connects to slang library
  slang::createGlobalSession(slangGlobalSession.writeRef());

  // b) create a session to define our compilation scope.
	auto slangTargets{ std::to_array<slang::TargetDesc>({ 
    {.format{SLANG_SPIRV},
      .profile{slangGlobalSession->findProfile("spirv_1_4")}
  } })};
	auto slangOptions{ std::to_array<slang::CompilerOptionEntry>({ {
	    slang::CompilerOptionName::EmitSpirvDirectly,
	    {slang::CompilerOptionValueKind::Int, 1}
	  } }) };
	slang::SessionDesc slangSessionDesc{
	  .targets{slangTargets.data()},
	  .targetCount{SlangInt(slangTargets.size())},
	  .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR, // column-wise
	  .compilerOptionEntries{slangOptions.data()},
	  .compilerOptionEntryCount{uint32_t(slangOptions.size())}
	};
  Slang::ComPtr<slang::ISession> slangSession;
  slangGlobalSession->createSession(slangSessionDesc, slangSession.writeRef());

  // 16. Load shader
	Slang::ComPtr<slang::IModule> slangModule{ slangSession->loadModuleFromSource("triangle", "assets/shader.slang", nullptr, nullptr) };
	Slang::ComPtr<ISlangBlob> spirv;
  slangModule->getTargetCode(0, spirv.writeRef()); // writing to spirv blob - as &spirv

  // 17. Create shader module
	VkShaderModuleCreateInfo shaderModuleCI{
	  .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	  .codeSize = spirv->getBufferSize(),
	  .pCode = (uint32_t*)spirv->getBufferPointer()
	};

	VkShaderModule shaderModule{};
	chk(vkCreateShaderModule(device, &shaderModuleCI, nullptr, &shaderModule));

  //----------------------------- PIPELINE ------------------------------------

  // 18. Pipeline
  // a) Pipeline layout (interface between shader and application)

  // Push constants (uniforms) - for each shader stage 
	VkPushConstantRange pushConstantRange{
	  .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
	  .size = sizeof(VkDeviceAddress) // uint64_t
	};
	VkPipelineLayoutCreateInfo pipelineLayoutCI{
	  .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	  .setLayoutCount = 1,
    .pSetLayouts = &descriptorSetLayoutTex, // from 14a) vkCreateDescriptorSetLayout (INTERFACE to theshader resources) - texture (sampler, imageView, imageLayout)
	  .pushConstantRangeCount = 1,
    .pPushConstantRanges = &pushConstantRange // single pointer to uniform buffer for each frame
	};

	chk(vkCreatePipelineLayout(device, &pipelineLayoutCI, nullptr, &pipelineLayout));


  // b) Shader stages
	std::vector<VkPipelineShaderStageCreateInfo> shaderStages{
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
		     .module = shaderModule, .pName = "main"},
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		     .module = shaderModule, .pName = "main" }
	};

  // Vertex input description
  //    see https ://vulkan-tutorial.com/Vertex_buffers/Vertex_input_description

  // c) vertex binding position of vertex attributes
  //    - defines stride and input rate (vertex or instance) for each vertex
	VkVertexInputBindingDescription vertexBinding{
	  .binding = 0,
	  .stride = sizeof(Vertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX  // move to next each vertex shader invocation - attribute
	            // VK_VERTEX_INPUT_RATE_INSTANCE: Move to the next after each instance
	};

  // Define the binding, location, format and offset of each attribute of the vertex
  // - format uses color channels names, size in bytes of each and type (sfloat, int, uint, etc.)
  // - defines implicitly the attribute byte-size
  // - it is not written in the shader - the VSInput must match this description
	std::vector<VkVertexInputAttributeDescription> vertexAttributes{
		{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT },
		{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal) },
		{ .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, uv) },
	};

  // d) fill `VkPipeline*CreateInfo` structures = standard pipeline values
  VkPipelineVertexInputStateCreateInfo vertexInputState{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &vertexBinding,
		.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size()),
		.pVertexAttributeDescriptions = vertexAttributes.data(),
	};

  VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{ // primitive type
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
  };

  // list of parts of the pipeline's fixed-function state are NOT baked into the pipeline - and are set in the command buffer at draw time
  std::vector<VkDynamicState> dynamicStates{
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR
  };
  VkPipelineDynamicStateCreateInfo dynamicState{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    .dynamicStateCount = 2,  // or static_cast<uint32_t>(dynamicStates.size())
    .pDynamicStates = dynamicStates.data()
  };

  VkPipelineViewportStateCreateInfo viewportState{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 }; // numbers matter
    // pViewports, pScissors are ignored because they listed as dynamic states above (are NULL),
    // and will be set in command buffer at draw time by vkCmdSetViewport / vkCmdSetScissor

  // the rest must be set to default values
  VkPipelineRasterizationStateCreateInfo rasterizationState{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth = 1.0f };

  VkPipelineMultisampleStateCreateInfo multisampleState{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };

  VkPipelineDepthStencilStateCreateInfo depthStencilState{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    .depthTestEnable = VK_TRUE,
    .depthWriteEnable = VK_TRUE,
    .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL
  };

  VkPipelineColorBlendAttachmentState blendAttachment{ .colorWriteMask = 0xF };
  VkPipelineColorBlendStateCreateInfo colorBlendState{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blendAttachment };

  VkPipelineRenderingCreateInfo renderingCI{  // -> pNext 
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
    .colorAttachmentCount = 1,
    .pColorAttachmentFormats = &imageFormat, // in 7) Swapchain - format R8G8B8A8_SRGB
    .depthAttachmentFormat = depthFormat // in 8) Depth attachment - format D32_SFLOAT_S8_UINT or D24_UNORM_S8_UINT 
  };

  // e) set all to pipeline Create Info and create pipeline
	VkGraphicsPipelineCreateInfo pipelineCI{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &renderingCI,
		.stageCount = 2,
    .pStages = shaderStages.data(),             // 18b) shader src code compiled to SPIR-V
    .pVertexInputState = &vertexInputState,     // 18d) stride, input rate, attribute location, format, offset
    .pInputAssemblyState = &inputAssemblyState, // primitive type - triangle list
		.pViewportState = &viewportState,           // Number of viewports and scissors [related to dynamicState]
		.pRasterizationState = &rasterizationState,
		.pMultisampleState = &multisampleState,
		.pDepthStencilState = &depthStencilState,
		.pColorBlendState = &colorBlendState,
    .pDynamicState = &dynamicState,             // vector of dynamic states - viewport and scissor [related to viewportState]
		.layout = pipelineLayout
	};
	chk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &pipeline));

  // TEST
  chkFence(vkGetFenceStatus(device, fences[0]), 0);
  chkFence(vkGetFenceStatus(device, fences[1]), 1);


  //--------------------------- RENDER LOOP ----------------------------------
  // 19. Render loop
	uint64_t lastTime{ SDL_GetTicks() };
	bool quit{ false };
	while (!quit) {
    
	  // a) Sync - wait for fence (info, that rendering finished and we can draw next frame)
		chk(vkWaitForFences(device, 1, &fences[frameIndex], true, UINT64_MAX));
		chk(vkResetFences(device, 1, &fences[frameIndex]));

    // TEST
    //chkFence(vkGetFenceStatus(device, fences[0]), 0);
    //chkFence(vkGetFenceStatus(device, fences[1]), 1);

    // b) acquire next image from swapchain
  	// Rendering of frameIndex image:
	  // Ask the swapchain for the next imageIndex to render to
	  // and signal the present semaphore when finished
	  chkSwapchain(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAcquiredSemaphores[frameIndex], VK_NULL_HANDLE, &imageIndex));

    // TEST
    //std::cout << "frameIndex = " << frameIndex << ", ";
    //std::cout << "imageIndex = " << imageIndex << "\n";

	  // c) Update shader data
		shaderData.projection = glm::perspective(glm::radians(45.0f), (float)windowSize.x / (float)windowSize.y, 0.1f, 32.0f);
		shaderData.view = glm::translate(glm::mat4(1.0f), camPos);
		for (auto i = 0; i < 3; i++) {
			auto instancePos = glm::vec3((float)(i - 1) * 3.0f, 0.0f, 0.0f);
			shaderData.model[i] = glm::translate(glm::mat4(1.0f), instancePos) * glm::mat4_cast(glm::quat(objectRotations[i]));
		}
		memcpy(shaderDataBuffers[frameIndex].allocationInfo.pMappedData, &shaderData, sizeof(ShaderData));

	  // d) Build (record) command buffer
		auto cb = commandBuffers[frameIndex];
		chk(vkResetCommandBuffer(cb, 0));  // should be implicit if commandBufferPool TRANSIENT

		VkCommandBufferBeginInfo cbBI {
		  .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		chk(vkBeginCommandBuffer(cb, &cbBI)); // does implicit reset

		std::array<VkImageMemoryBarrier2, 2> outputBarriers{
      // color image barrier
      //   stage/access: COLOR_ATTACHMENT_OUTPUT/NULL ---> COLOR_ATTACHMENT_OUTPUT/COLOR_ATTACHMENT_READ|WRITE
      //   layout: UNDEFINED ---> ATTACHMENT_OPTIMAL
      VkImageMemoryBarrier2{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				.srcAccessMask = 0,
				.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
        .image = swapchainImages[imageIndex],  // image to render to (got from swapchain)
				.subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 }
			},
      // depth image barrier
      //   stage/access: LATE_FRAGMENT_TESTS/DEPTH_STENCIL_ATTACHMENT_WRITE ---> EARLY_FRAGMENT_TESTS/DEPTH_STENCIL_ATTACHMENT_WRITE
      //   layout: UNDEFINED ---> ATTACHMENT_OPTIMAL
      VkImageMemoryBarrier2{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
				.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
				.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
				.image = depthImage,
				.subresourceRange{.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, .levelCount = 1, .layerCount = 1 }
			}
		};
		VkDependencyInfo barrierDependencyInfo{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = outputBarriers.data() };
		vkCmdPipelineBarrier2(cb, &barrierDependencyInfo);


		VkRenderingAttachmentInfo colorAttachmentInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = swapchainImageViews[imageIndex],
			.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue{.color{ 0.0f, 0.0f, 0.0f, 1.0f }}
		};
		VkRenderingAttachmentInfo depthAttachmentInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = depthImageView,
			.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.clearValue = {.depthStencil = {1.0f,  0}}
		};
		VkRenderingInfo renderingInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea{.extent{.width = static_cast<uint32_t>(windowSize.x), .height = static_cast<uint32_t>(windowSize.y) }},
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &colorAttachmentInfo,
			.pDepthAttachment = &depthAttachmentInfo
		};
		vkCmdBeginRendering(cb, &renderingInfo);

		VkViewport vp{ .width = static_cast<float>(windowSize.x), .height = static_cast<float>(windowSize.y), .minDepth = 0.0f, .maxDepth = 1.0f};
		vkCmdSetViewport(cb, 0, 1, &vp);

		VkRect2D scissor{ .extent{ .width = static_cast<uint32_t>(windowSize.x), .height = static_cast<uint32_t>(windowSize.y) } };
    vkCmdSetScissor(cb, 0, 1, &scissor);

	  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSetTex, 0, nullptr);
		VkDeviceSize vOffset{ 0 };

		vkCmdBindVertexBuffers(cb, 0, 1, &vBuffer, &vOffset);
		vkCmdBindIndexBuffer(cb, vBuffer, vBufSize, VK_INDEX_TYPE_UINT16);

		vkCmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(VkDeviceAddress), &shaderDataBuffers[frameIndex].deviceAddress);

		vkCmdDrawIndexed(cb, indexCount, 3, 0, 0, 0);

		vkCmdEndRendering(cb);

		VkImageMemoryBarrier2 barrierPresent{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstAccessMask = 0,
			.oldLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			.image = swapchainImages[imageIndex],
			.subresourceRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 }
		};
		VkDependencyInfo barrierPresentDependencyInfo{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrierPresent };
		vkCmdPipelineBarrier2(cb, &barrierPresentDependencyInfo);
		chk(vkEndCommandBuffer(cb));

	  // e) Submit command buffer to graphics queue
		VkPipelineStageFlags waitStages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo submitInfo{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1,
      .pWaitSemaphores = &imageAcquiredSemaphores[frameIndex],  // wait for image to be available (finished presenting)
			.pWaitDstStageMask = &waitStages,
			.commandBufferCount = 1,
			.pCommandBuffers = &cb, //prepared commands
			.signalSemaphoreCount = 1,
      .pSignalSemaphores = &renderCompleteSemaphores[imageIndex],  // signal when finished rendering image
		};
		chk(vkQueueSubmit(queue, 1, &submitInfo, fences[frameIndex]));
    // 1. wait for image given by presentation engine
    // 2. execute command buffer cb and render to image
    // 3. signal renderCompleteSemaphore when finished rendering image
    // 4. signal fence when finished rendering image to CPU - can start next frame
    // 3. and 4. signal the same event - this submission finished executing on the
    //    GPU - but they wake different waiters:
    //      fence     -> the CPU, so reusing cb and shaderDataBuffers[frameIndex] is safe
    //      semaphore -> the GPU, holds back vkQueuePresentKHR until the image is drawn
    //    A fence reports completion to the CPU, a semaphore reports it to the GPU.

    // f) increment frameIndex for next frame (modulo maxFramesInFlight)
		frameIndex = (frameIndex + 1) % maxFramesInFlight;

    // g) Present the rendered image to the screen
		VkPresentInfoKHR presentInfo{
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &renderCompleteSemaphores[imageIndex],
			.swapchainCount = 1,
			.pSwapchains = &swapchain,
			.pImageIndices = &imageIndex
		};
		chkSwapchain(vkQueuePresentKHR(queue, &presentInfo));

	  // h) Poll events (mouse, keys, window resize)
		float elapsedTime{ (SDL_GetTicks() - lastTime) / 1000.0f };
		lastTime = SDL_GetTicks();
		for (SDL_Event event; SDL_PollEvent(&event);) {
			if (event.type == SDL_EVENT_QUIT) {
				quit = true;
				break;
			}
			if (event.type == SDL_EVENT_MOUSE_MOTION) {
				if (event.button.button == SDL_BUTTON_LEFT) {
					objectRotations[shaderData.selected].x -= (float)event.motion.yrel * elapsedTime;
					objectRotations[shaderData.selected].y += (float)event.motion.xrel * elapsedTime;
				}
			}
			if (event.type == SDL_EVENT_MOUSE_WHEEL) {
				camPos.z += (float)event.wheel.y * elapsedTime * 10.0f;
			}
			if (event.type == SDL_EVENT_KEY_DOWN) {
				if (event.key.key == SDLK_PLUS || event.key.key == SDLK_KP_PLUS) {
					shaderData.selected = (shaderData.selected < 2) ? shaderData.selected + 1 : 0;
				}
				if (event.key.key == SDLK_MINUS || event.key.key == SDLK_KP_MINUS) {
					shaderData.selected = (shaderData.selected > 0) ? shaderData.selected - 1 : 2;
				}
			}

		  // i) Window resize
			if (event.type == SDL_EVENT_WINDOW_RESIZED) {
        updateSwapchain = true; // will recreate swapchain and depth image - pipeline will remain the same
			}
		}
    // 20. Recreate swapchain
		if (updateSwapchain) {
			chk(SDL_GetWindowSize(window, &windowSize.x, &windowSize.y));
			updateSwapchain = false;
			chk(vkDeviceWaitIdle(device));

			chk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(devices[deviceIndex], surface, &surfaceCaps));
			swapchainCI.oldSwapchain = swapchain;
			swapchainCI.imageExtent = { .width = static_cast<uint32_t>(windowSize.x), .height = static_cast<uint32_t>(windowSize.y)};
			chk(vkCreateSwapchainKHR(device, &swapchainCI, nullptr, &swapchain));

			for (auto i = 0; i < imageCount; i++) {
				vkDestroyImageView(device, swapchainImageViews[i], nullptr);
			}
			chk(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr));
			swapchainImages.resize(imageCount);
			chk(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data()));

			swapchainImageViews.resize(imageCount);
			for (auto i = 0; i < imageCount; i++) {
				VkImageViewCreateInfo viewCI{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = swapchainImages[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = imageFormat, .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1} };
				chk(vkCreateImageView(device, &viewCI, nullptr, &swapchainImageViews[i]));
			}

			for (auto& semaphore : renderCompleteSemaphores) {
				vkDestroySemaphore(device, semaphore, nullptr);
			}

			renderCompleteSemaphores.resize(imageCount);
			for (auto& semaphore : renderCompleteSemaphores) {
				chk(vkCreateSemaphore(device, &semaphoreCI, nullptr, &semaphore));
			}

			vkDestroySwapchainKHR(device, swapchainCI.oldSwapchain, nullptr);
			vmaDestroyImage(allocator, depthImage, depthImageAllocation);
			vkDestroyImageView(device, depthImageView, nullptr);

			depthImageCI.extent = { .width = static_cast<uint32_t>(windowSize.x), .height = static_cast<uint32_t>(windowSize.y), .depth = 1 };
			VmaAllocationCreateInfo allocCI{ .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT, .usage = VMA_MEMORY_USAGE_AUTO };
			chk(vmaCreateImage(allocator, &depthImageCI, &allocCI, &depthImage, &depthImageAllocation, nullptr));

			VkImageViewCreateInfo viewCI{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = depthImage, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = depthFormat, .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1 } };
			chk(vkCreateImageView(device, &viewCI, nullptr, &depthImageView));
		}
	}

	// 21. Tear down - cleanup
	chk(vkDeviceWaitIdle(device));

	for (auto i = 0; i < maxFramesInFlight; i++) {
		vkDestroyFence(device, fences[i], nullptr);
		vkDestroySemaphore(device, imageAcquiredSemaphores[i], nullptr);
		vmaDestroyBuffer(allocator, shaderDataBuffers[i].buffer, shaderDataBuffers[i].allocation);
	}
	for (auto i = 0; i < renderCompleteSemaphores.size(); i++) {
		vkDestroySemaphore(device, renderCompleteSemaphores[i], nullptr);
	}

	vmaDestroyImage(allocator, depthImage, depthImageAllocation);
	vkDestroyImageView(device, depthImageView, nullptr);

	for (auto i = 0; i < swapchainImageViews.size(); i++) {
		vkDestroyImageView(device, swapchainImageViews[i], nullptr);
	}

  vmaDestroyBuffer(allocator, vBuffer, vBufferAllocation);

  for (auto i = 0; i < textures.size(); i++) {
		vkDestroyImageView(device, textures[i].view, nullptr);
		vkDestroySampler(device, textures[i].sampler, nullptr);
		vmaDestroyImage(allocator, textures[i].image, textures[i].allocation);
	}

  vkDestroyDescriptorSetLayout(device, descriptorSetLayoutTex, nullptr);
	vkDestroyDescriptorPool(device, descriptorPool, nullptr);

	vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
	vkDestroyPipeline(device, pipeline, nullptr);

	vkDestroySwapchainKHR(device, swapchain, nullptr);
	vkDestroySurfaceKHR(instance, surface, nullptr);
	vkDestroyCommandPool(device, commandPool, nullptr);
	// vkDestroyCommandPool(device, commandPool, nullptr); // 2nd call should generate an error
	vkDestroyShaderModule(device, shaderModule, nullptr);

	vmaDestroyAllocator(allocator);
	SDL_DestroyWindow(window);
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	SDL_Quit();

	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
}