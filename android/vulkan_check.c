/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Query the real Android Vulkan loader/device before building the renderer. */
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
static void version(const char *label,uint32_t value) {
    printf("%s: %u.%u.%u\n",label,VK_VERSION_MAJOR(value),VK_VERSION_MINOR(value),VK_VERSION_PATCH(value));
}
int main(void) {
    uint32_t api=VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enumerate=(PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL,"vkEnumerateInstanceVersion");
    if (enumerate && enumerate(&api)!=VK_SUCCESS) return 1;
    version("Vulkan loader",api);
    VkApplicationInfo application={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Tempest Android check",.apiVersion=api};
    VkInstanceCreateInfo create={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&application};
    VkInstance instance;
    VkResult result=vkCreateInstance(&create,NULL,&instance);
    if (result!=VK_SUCCESS) { printf("vkCreateInstance: %d\n",result); return 1; }
    uint32_t count=0;
    result=vkEnumeratePhysicalDevices(instance,&count,NULL);
    if (result!=VK_SUCCESS || !count) { printf("Vulkan devices: %u (result %d)\n",count,result); vkDestroyInstance(instance,NULL); return 1; }
    VkPhysicalDevice *devices=calloc(count,sizeof(*devices));
    if (!devices) { vkDestroyInstance(instance,NULL); return 1; }
    result=vkEnumeratePhysicalDevices(instance,&count,devices);
    int supported=0;
    if (result==VK_SUCCESS) for (uint32_t i=0;i<count;++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i],&properties);
        printf("GPU: %s\n",properties.deviceName);
        version("GPU Vulkan API",properties.apiVersion);
        printf("GPU vendor: 0x%x, driver: 0x%x\n",properties.vendorID,properties.driverVersion);
        supported|=properties.apiVersion>=VK_API_VERSION_1_3;
    }
    free(devices); vkDestroyInstance(instance,NULL);
    puts(supported ? "Tempest Vulkan 1.3 requirement: met" : "Tempest Vulkan 1.3 requirement: needs a compatible driver");
    return supported ? 0 : 20;
}
