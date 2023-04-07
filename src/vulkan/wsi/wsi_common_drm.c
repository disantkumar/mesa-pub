/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "wsi_common_private.h"
#include "wsi_common_drm.h"
#include "util/macros.h"
#include "util/os_file.h"
#include "util/log.h"
#include "util/xmlconfig.h"
#include "vk_device.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "drm-uapi/drm_fourcc.h"
#include "drm-uapi/dma-buf.h"
#include "drm-uapi/virtgpu_drm.h"

#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <xf86drm.h>

#include <../../virtio/virtio-gpu/virgl_hw.h>
#include <../../virtio/virtio-gpu/virgl_protocol.h>


static VkResult
wsi_dma_buf_export_sync_file(int dma_buf_fd, int *sync_file_fd)
{
   /* Don't keep trying an IOCTL that doesn't exist. */
   static bool no_dma_buf_sync_file = false;
   if (no_dma_buf_sync_file)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   struct dma_buf_export_sync_file export = {
      .flags = DMA_BUF_SYNC_RW,
      .fd = -1,
   };
   int ret = drmIoctl(dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export);
   if (ret) {
      if (errno == ENOTTY || errno == EBADF || errno == ENOSYS) {
         no_dma_buf_sync_file = true;
         return VK_ERROR_FEATURE_NOT_PRESENT;
      } else {
         mesa_loge("MESA: failed to export sync file '%s'", strerror(errno));
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   *sync_file_fd = export.fd;

   return VK_SUCCESS;
}

static VkResult
wsi_dma_buf_import_sync_file(int dma_buf_fd, int sync_file_fd)
{
   /* Don't keep trying an IOCTL that doesn't exist. */
   static bool no_dma_buf_sync_file = false;
   if (no_dma_buf_sync_file)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   struct dma_buf_import_sync_file import = {
      .flags = DMA_BUF_SYNC_RW,
      .fd = sync_file_fd,
   };
   int ret = drmIoctl(dma_buf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import);
   if (ret) {
      if (errno == ENOTTY || errno == EBADF || errno == ENOSYS) {
         no_dma_buf_sync_file = true;
         return VK_ERROR_FEATURE_NOT_PRESENT;
      } else {
         mesa_loge("MESA: failed to import sync file '%s'", strerror(errno));
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   return VK_SUCCESS;
}

static VkResult
prepare_signal_dma_buf_from_semaphore(struct wsi_swapchain *chain,
                                      const struct wsi_image *image)
{
   VkResult result;

   if (!(chain->wsi->semaphore_export_handle_types &
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT))
      return VK_ERROR_FEATURE_NOT_PRESENT;

   int sync_file_fd = -1;
   result = wsi_dma_buf_export_sync_file(image->dma_buf_fd, &sync_file_fd);
   if (result != VK_SUCCESS)
      return result;

   result = wsi_dma_buf_import_sync_file(image->dma_buf_fd, sync_file_fd);
   close(sync_file_fd);
   if (result != VK_SUCCESS)
      return result;

   /* If we got here, all our checks pass.  Create the actual semaphore */
   const VkExportSemaphoreCreateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   const VkSemaphoreCreateInfo semaphore_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &export_info,
   };
   result = chain->wsi->CreateSemaphore(chain->device, &semaphore_info,
                                        &chain->alloc,
                                        &chain->dma_buf_semaphore);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

VkResult
wsi_prepare_signal_dma_buf_from_semaphore(struct wsi_swapchain *chain,
                                          const struct wsi_image *image)
{
   VkResult result;

   /* We cache result - 1 in the swapchain */
   if (unlikely(chain->signal_dma_buf_from_semaphore == 0)) {
      result = prepare_signal_dma_buf_from_semaphore(chain, image);
      assert(result <= 0);
      chain->signal_dma_buf_from_semaphore = (int)result - 1;
   } else {
      result = (VkResult)(chain->signal_dma_buf_from_semaphore + 1);
   }

   return result;
}

VkResult
wsi_signal_dma_buf_from_semaphore(const struct wsi_swapchain *chain,
                                  const struct wsi_image *image)
{
   VkResult result;

   const VkSemaphoreGetFdInfoKHR get_fd_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .semaphore = chain->dma_buf_semaphore,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   int sync_file_fd = -1;
   result = chain->wsi->GetSemaphoreFdKHR(chain->device, &get_fd_info,
                                          &sync_file_fd);
   if (result != VK_SUCCESS)
      return result;

   result = wsi_dma_buf_import_sync_file(image->dma_buf_fd, sync_file_fd);
   close(sync_file_fd);
   return result;
}

static const struct vk_sync_type *
get_sync_file_sync_type(struct vk_device *device,
                        enum vk_sync_features req_features)
{
   for (const struct vk_sync_type *const *t =
        device->physical->supported_sync_types; *t; t++) {
      if (req_features & ~(*t)->features)
         continue;

      if ((*t)->import_sync_file != NULL)
         return *t;
   }

   return NULL;
}

VkResult
wsi_create_sync_for_dma_buf_wait(const struct wsi_swapchain *chain,
                                 const struct wsi_image *image,
                                 enum vk_sync_features req_features,
                                 struct vk_sync **sync_out)
{
   VK_FROM_HANDLE(vk_device, device, chain->device);
   VkResult result;

   const struct vk_sync_type *sync_type =
      get_sync_file_sync_type(device, req_features);
   if (sync_type == NULL)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   int sync_file_fd = -1;
   result = wsi_dma_buf_export_sync_file(image->dma_buf_fd, &sync_file_fd);
   if (result != VK_SUCCESS)
      return result;

   struct vk_sync *sync = NULL;
   result = vk_sync_create(device, sync_type, VK_SYNC_IS_SHAREABLE, 0, &sync);
   if (result != VK_SUCCESS)
      goto fail_close_sync_file;

   result = vk_sync_import_sync_file(device, sync, sync_file_fd);
   if (result != VK_SUCCESS)
      goto fail_destroy_sync;

   close(sync_file_fd);
   *sync_out = sync;

   return VK_SUCCESS;

fail_destroy_sync:
   vk_sync_destroy(device, sync);
fail_close_sync_file:
   close(sync_file_fd);

   return result;
}

bool
wsi_common_drm_devices_equal(int fd_a, int fd_b)
{
   drmDevicePtr device_a, device_b;
   int ret;

   ret = drmGetDevice2(fd_a, 0, &device_a);
   if (ret)
      return false;

   ret = drmGetDevice2(fd_b, 0, &device_b);
   if (ret) {
      drmFreeDevice(&device_a);
      return false;
   }

   bool result = drmDevicesEqual(device_a, device_b);

   drmFreeDevice(&device_a);
   drmFreeDevice(&device_b);

   return result;
}

static bool
is_virtgpu(int fd) {
   drmVersionPtr drm_version;
   drm_version = drmGetVersion(fd);
   
   if (!drm_version) {
      return false;
   }

   if (!strcmp(drm_version->name, "virtio_gpu")) {
      drmFreeVersion(drm_version);
      return true;
   }

   drmFreeVersion(drm_version);

   return false;
}

#define VIRGL_DRM_CAPSET_VIRGL2 2

static VkResult
virtgpu_init_context(int fd)
{
   int ret;
   struct drm_virtgpu_context_init init = { 0 };
   struct drm_virtgpu_context_set_param ctx_set_param = { 0 };

   ctx_set_param.param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
   ctx_set_param.value = VIRGL_DRM_CAPSET_VIRGL2;

   init.ctx_set_params = (unsigned long)(void *)&ctx_set_param;
   init.num_params = 1;

   ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &init);
   /*
    * EEXIST happens when a compositor does DUMB_CREATE before initializing
    * virgl.
    */
   if (ret && errno != EEXIST) {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

int
virtgpu_alloc_and_export(int fd, uint32_t linear_stride, uint32_t linear_size);

/* from gallium/include/p_defines.h 
   why this is not part of the protocol? */
enum pipe_texture_target
{
   PIPE_BUFFER,
   PIPE_TEXTURE_1D,
   PIPE_TEXTURE_2D,
   PIPE_TEXTURE_3D,
   PIPE_TEXTURE_CUBE,
   PIPE_TEXTURE_RECT,
   PIPE_TEXTURE_1D_ARRAY,
   PIPE_TEXTURE_2D_ARRAY,
   PIPE_TEXTURE_CUBE_ARRAY,
   PIPE_MAX_TEXTURE_TYPES,
};


static int32_t blob_id = 0;
int
virtgpu_alloc_and_export(int fd, uint32_t linear_stride, uint32_t linear_size)
{
   int ret;
   struct drm_virtgpu_resource_create_blob drm_rc_blob;
   struct drm_virtgpu_execbuffer eb;
   struct drm_virtgpu_3d_wait waitcmd;
   struct drm_prime_handle prime_handle;
   struct drm_gem_close gem_close;
   uint32_t cmd[VIRGL_PIPE_RES_CREATE_SIZE + 1];

   cmd[0] = VIRGL_CMD0(VIRGL_CCMD_PIPE_RESOURCE_CREATE, 0, VIRGL_PIPE_RES_CREATE_SIZE);
   cmd[VIRGL_PIPE_RES_CREATE_FORMAT] = VIRGL_FORMAT_B8G8R8X8_UNORM; //   pipe_to_virgl_format(format);
   // 0x54000a
   // 22,20,18,3,1
   cmd[VIRGL_PIPE_RES_CREATE_BIND] = VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SCANOUT | VIRGL_BIND_LINEAR | VIRGL_BIND_SHARED;
   cmd[VIRGL_PIPE_RES_CREATE_TARGET] = PIPE_TEXTURE_2D;
   cmd[VIRGL_PIPE_RES_CREATE_WIDTH] = linear_stride >> 2;
   cmd[VIRGL_PIPE_RES_CREATE_HEIGHT] = linear_size / linear_stride;
   cmd[VIRGL_PIPE_RES_CREATE_DEPTH] = 1;
   cmd[VIRGL_PIPE_RES_CREATE_ARRAY_SIZE] = 1;
   cmd[VIRGL_PIPE_RES_CREATE_LAST_LEVEL] = 0;
   cmd[VIRGL_PIPE_RES_CREATE_NR_SAMPLES] = 0;
   cmd[VIRGL_PIPE_RES_CREATE_FLAGS] = 0; // VIRGL_RESOURCE_FLAG_MAP_PERSISTENT;
   cmd[VIRGL_PIPE_RES_CREATE_BLOB_ID] = ++blob_id;

   memset(&drm_rc_blob, 0, sizeof(drm_rc_blob));
   drm_rc_blob.cmd = (uintptr_t)cmd;
   drm_rc_blob.cmd_size = 4 * (VIRGL_PIPE_RES_CREATE_SIZE + 1);
   drm_rc_blob.size = linear_size;
   drm_rc_blob.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
   drm_rc_blob.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE | VIRTGPU_BLOB_FLAG_USE_SHAREABLE | VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE;
   drm_rc_blob.blob_id = blob_id;

   ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &drm_rc_blob);
   if (ret != 0) {
      return ret;
   }
   /* WORKAROUND
    * send empty execbuffer and a wait, to prevent race between virtio-gpu and virtio-iommu in crosvm resulting in
    * [devices/src/virtio/iommu.rs:625] execute_request failed: memory mapper failed: failed to find host address
   */
   cmd[0] = 0;
   memset(&eb, 0, sizeof(eb));
   eb.command = (uintptr_t)cmd;
   eb.size = 0;
   eb.num_bo_handles = 1;
   eb.bo_handles = (uintptr_t)&drm_rc_blob.bo_handle;

   ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &eb);
   if (ret == -1) {
      //_debug_printf("failed to send execbuffer: %s", strerror(errno));
   }

   memset(&waitcmd, 0, sizeof(waitcmd));
   waitcmd.handle = drm_rc_blob.bo_handle;

   ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &waitcmd);
   if (ret) {
      // _debug_printf("waiting got error - %d, slow gpu or hang?\n", errno);
   }

   // export the dma_buf

   memset(&prime_handle, 0, sizeof(prime_handle));
   prime_handle.handle = drm_rc_blob.bo_handle;

   ret = drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle);
   if (ret < 0)
      return ret;

   // we can close the handle now
   memset(&gem_close, 0, sizeof(gem_close));
   gem_close.handle = drm_rc_blob.bo_handle;

   ret = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
   if (ret) {
      /* Weird! */
   }

   return prime_handle.fd;
}

bool
wsi_device_matches_drm_fd(const struct wsi_device *wsi, int drm_fd)
{
   if (wsi->can_present_on_device)
      return wsi->can_present_on_device(wsi->pdevice, drm_fd);

   drmDevicePtr fd_device;
   int ret = drmGetDevice2(drm_fd, 0, &fd_device);
   if (ret)
      return false;

   bool match = false;
   switch (fd_device->bustype) {
   case DRM_BUS_PCI:
      match = wsi->pci_bus_info.pciDomain == fd_device->businfo.pci->domain &&
              wsi->pci_bus_info.pciBus == fd_device->businfo.pci->bus &&
              wsi->pci_bus_info.pciDevice == fd_device->businfo.pci->dev &&
              wsi->pci_bus_info.pciFunction == fd_device->businfo.pci->func;
      break;

   default:
      break;
   }

   drmFreeDevice(&fd_device);

   return match;
}

static uint32_t
prime_select_buffer_memory_type(const struct wsi_device *wsi,
                                uint32_t type_bits)
{
   return wsi_select_memory_type(wsi, 0 /* req_props */,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 type_bits);
}

static const struct VkDrmFormatModifierPropertiesEXT *
get_modifier_props(const struct wsi_image_info *info, uint64_t modifier)
{
   for (uint32_t i = 0; i < info->modifier_prop_count; i++) {
      if (info->modifier_props[i].drmFormatModifier == modifier)
         return &info->modifier_props[i];
   }
   return NULL;
}

static VkResult
wsi_create_native_image_mem(const struct wsi_swapchain *chain,
                            const struct wsi_image_info *info,
                            struct wsi_image *image);

static VkResult
wsi_configure_native_image(const struct wsi_swapchain *chain,
                           const VkSwapchainCreateInfoKHR *pCreateInfo,
                           uint32_t num_modifier_lists,
                           const uint32_t *num_modifiers,
                           const uint64_t *const *modifiers,
                           struct wsi_image_info *info)
{
   const struct wsi_device *wsi = chain->wsi;

   VkExternalMemoryHandleTypeFlags handle_type =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

   VkResult result = wsi_configure_image(chain, pCreateInfo, handle_type, info);
   if (result != VK_SUCCESS)
      return result;
   info->display_device_fd = -1;
   info->display_device_is_virtgpu = false;

   if (num_modifier_lists == 0) {
      /* If we don't have modifiers, fall back to the legacy "scanout" flag */
      info->wsi.scanout = true;
   } else {
      /* The winsys can't request modifiers if we don't support them. */
      assert(wsi->supports_modifiers);
      struct VkDrmFormatModifierPropertiesListEXT modifier_props_list = {
         .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
      };
      VkFormatProperties2 format_props = {
         .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
         .pNext = &modifier_props_list,
      };
      wsi->GetPhysicalDeviceFormatProperties2(wsi->pdevice,
                                              pCreateInfo->imageFormat,
                                              &format_props);
      assert(modifier_props_list.drmFormatModifierCount > 0);
      info->modifier_props =
         vk_alloc(&chain->alloc,
                  sizeof(*info->modifier_props) *
                  modifier_props_list.drmFormatModifierCount,
                  8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (info->modifier_props == NULL)
         goto fail_oom;

      modifier_props_list.pDrmFormatModifierProperties = info->modifier_props;
      wsi->GetPhysicalDeviceFormatProperties2(wsi->pdevice,
                                              pCreateInfo->imageFormat,
                                              &format_props);

      /* Call GetImageFormatProperties with every modifier and filter the list
       * down to those that we know work.
       */
      info->modifier_prop_count = 0;
      for (uint32_t i = 0; i < modifier_props_list.drmFormatModifierCount; i++) {
         VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = info->modifier_props[i].drmFormatModifier,
            .sharingMode = pCreateInfo->imageSharingMode,
            .queueFamilyIndexCount = pCreateInfo->queueFamilyIndexCount,
            .pQueueFamilyIndices = pCreateInfo->pQueueFamilyIndices,
         };
         VkPhysicalDeviceImageFormatInfo2 format_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .format = pCreateInfo->imageFormat,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = pCreateInfo->imageUsage,
            .flags = info->create.flags,
         };

         VkImageFormatListCreateInfo format_list;
         if (info->create.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
            format_list = info->format_list;
            format_list.pNext = NULL;
            __vk_append_struct(&format_info, &format_list);
         }

         struct wsi_image_create_info wsi_info = (struct wsi_image_create_info) {
            .sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA,
            .pNext = NULL,
         };
         __vk_append_struct(&format_info, &wsi_info);

         VkImageFormatProperties2 format_props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
            .pNext = NULL,
         };
         __vk_append_struct(&format_info, &mod_info);
         result = wsi->GetPhysicalDeviceImageFormatProperties2(wsi->pdevice,
                                                               &format_info,
                                                               &format_props);
         if (result == VK_SUCCESS &&
             pCreateInfo->imageExtent.width <= format_props.imageFormatProperties.maxExtent.width &&
             pCreateInfo->imageExtent.height <= format_props.imageFormatProperties.maxExtent.height)
            info->modifier_props[info->modifier_prop_count++] = info->modifier_props[i];
      }

      uint32_t max_modifier_count = 0;
      for (uint32_t l = 0; l < num_modifier_lists; l++)
         max_modifier_count = MAX2(max_modifier_count, num_modifiers[l]);

      uint64_t *image_modifiers =
         vk_alloc(&chain->alloc, sizeof(*image_modifiers) * max_modifier_count,
                  8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!image_modifiers)
         goto fail_oom;

      uint32_t image_modifier_count = 0;
      for (uint32_t l = 0; l < num_modifier_lists; l++) {
         /* Walk the modifier lists and construct a list of supported
          * modifiers.
          */
         for (uint32_t i = 0; i < num_modifiers[l]; i++) {
            if (get_modifier_props(info, modifiers[l][i]))
               image_modifiers[image_modifier_count++] = modifiers[l][i];
         }

         /* We only want to take the modifiers from the first list */
         if (image_modifier_count > 0)
            break;
      }

      if (image_modifier_count > 0) {
         info->create.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
         info->drm_mod_list = (VkImageDrmFormatModifierListCreateInfoEXT) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
            .drmFormatModifierCount = image_modifier_count,
            .pDrmFormatModifiers = image_modifiers,
         };
         image_modifiers = NULL;
         __vk_append_struct(&info->create, &info->drm_mod_list);
      } else {
         vk_free(&chain->alloc, image_modifiers);
         /* TODO: Add a proper error here */
         assert(!"Failed to find a supported modifier!  This should never "
                 "happen because LINEAR should always be available");
         goto fail_oom;
      }
   }

   info->create_mem = wsi_create_native_image_mem;

   return VK_SUCCESS;

fail_oom:
   wsi_destroy_image_info(chain, info);
   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult
wsi_init_image_dmabuf_fd(const struct wsi_swapchain *chain,
                          struct wsi_image *image,
                          bool linear)
{
   const struct wsi_device *wsi = chain->wsi;
   const VkMemoryGetFdInfoKHR memory_get_fd_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .pNext = NULL,
      .memory = linear ? image->blit.memory : image->memory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };

   return wsi->GetMemoryFdKHR(chain->device, &memory_get_fd_info,
                              &image->dma_buf_fd);
}

static VkResult
wsi_create_native_image_mem(const struct wsi_swapchain *chain,
                            const struct wsi_image_info *info,
                            struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;

   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(chain->device, image->image, &reqs);

   const struct wsi_memory_allocate_info memory_wsi_info = {
      .sType = VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA,
      .pNext = NULL,
      .implicit_sync = true,
   };
   const VkExportMemoryAllocateInfo memory_export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = &memory_wsi_info,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkMemoryDedicatedAllocateInfo memory_dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &memory_export_info,
      .image = image->image,
      .buffer = VK_NULL_HANDLE,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &memory_dedicated_info,
      .allocationSize = reqs.size,
      .memoryTypeIndex =
         wsi_select_device_memory_type(wsi, reqs.memoryTypeBits),
   };
   result = wsi->AllocateMemory(chain->device, &memory_info,
                                &chain->alloc, &image->memory);
   if (result != VK_SUCCESS)
      return result;

   result = wsi_init_image_dmabuf_fd(chain, image, false);
   if (result != VK_SUCCESS)
      return result;

   if (info->drm_mod_list.drmFormatModifierCount > 0) {
      VkImageDrmFormatModifierPropertiesEXT image_mod_props = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
      };
      result = wsi->GetImageDrmFormatModifierPropertiesEXT(chain->device,
                                                           image->image,
                                                           &image_mod_props);
      if (result != VK_SUCCESS)
         return result;

      image->drm_modifier = image_mod_props.drmFormatModifier;
      assert(image->drm_modifier != DRM_FORMAT_MOD_INVALID);

      const struct VkDrmFormatModifierPropertiesEXT *mod_props =
         get_modifier_props(info, image->drm_modifier);
      image->num_planes = mod_props->drmFormatModifierPlaneCount;

      for (uint32_t p = 0; p < image->num_planes; p++) {
         const VkImageSubresource image_subresource = {
            .aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT << p,
            .mipLevel = 0,
            .arrayLayer = 0,
         };
         VkSubresourceLayout image_layout;
         wsi->GetImageSubresourceLayout(chain->device, image->image,
                                        &image_subresource, &image_layout);
         image->sizes[p] = image_layout.size;
         image->row_pitches[p] = image_layout.rowPitch;
         image->offsets[p] = image_layout.offset;
      }
   } else {
      const VkImageSubresource image_subresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .mipLevel = 0,
         .arrayLayer = 0,
      };
      VkSubresourceLayout image_layout;
      wsi->GetImageSubresourceLayout(chain->device, image->image,
                                     &image_subresource, &image_layout);

      image->drm_modifier = DRM_FORMAT_MOD_INVALID;
      image->num_planes = 1;
      image->sizes[0] = reqs.size;
      image->row_pitches[0] = image_layout.rowPitch;
      image->offsets[0] = 0;
   }

   return VK_SUCCESS;
}

#define WSI_PRIME_LINEAR_STRIDE_ALIGN 256

static VkResult
wsi_create_prime_image_mem(const struct wsi_swapchain *chain,
                           const struct wsi_image_info *info,
                           struct wsi_image *image)
{
   VkResult result =
      wsi_create_buffer_blit_context(chain, info, image,
                                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                     true);
   if (result != VK_SUCCESS)
      return result;

   result = wsi_init_image_dmabuf_fd(chain, image, true);
   if (result != VK_SUCCESS)
      return result;

   image->drm_modifier = info->prime_use_linear_modifier ?
                         DRM_FORMAT_MOD_LINEAR : DRM_FORMAT_MOD_INVALID;

   return VK_SUCCESS;
}

static VkResult
wsi_configure_prime_image(UNUSED const struct wsi_swapchain *chain,
                          const VkSwapchainCreateInfoKHR *pCreateInfo,
                          bool use_modifier,
                          wsi_memory_type_select_cb select_buffer_memory_type,
                          const struct wsi_drm_image_params *params,
                          struct wsi_image_info *info)
{
   VkResult result = wsi_configure_image(chain, pCreateInfo,
                                         0 /* handle_types */, info);
   if (result != VK_SUCCESS)
      return result;

   wsi_configure_buffer_image(chain, pCreateInfo,
                              WSI_PRIME_LINEAR_STRIDE_ALIGN, 4096,
                              info);
   info->prime_use_linear_modifier = use_modifier;

   info->create_mem = wsi_create_prime_image_mem;
   info->select_blit_dst_memory_type = select_buffer_memory_type;
   info->select_image_memory_type = wsi_select_device_memory_type;
   info->display_device_fd = -1;
   info->display_device_is_virtgpu = false;
   if (is_virtgpu(params->display_device_fd)) {
      result = virtgpu_init_context(params->display_device_fd);
      if (result != VK_SUCCESS)
         return result;
      info->display_device_fd = params->display_device_fd;
      info->display_device_is_virtgpu = true;
   }

   return VK_SUCCESS;
}

bool
wsi_drm_image_needs_buffer_blit(const struct wsi_device *wsi,
                                const struct wsi_drm_image_params *params)
{
   if (!params->same_gpu)
      return true;

   if (params->num_modifier_lists > 0 || wsi->supports_scanout)
      return false;

   return true;
}

VkResult
wsi_drm_configure_image(const struct wsi_swapchain *chain,
                        const VkSwapchainCreateInfoKHR *pCreateInfo,
                        const struct wsi_drm_image_params *params,
                        struct wsi_image_info *info)
{
   assert(params->base.image_type == WSI_IMAGE_TYPE_DRM);

   if (chain->blit.type == WSI_SWAPCHAIN_BUFFER_BLIT) {
      bool use_modifier = params->num_modifier_lists > 0;
      wsi_memory_type_select_cb select_buffer_memory_type =
         params->same_gpu ? wsi_select_device_memory_type :
                            prime_select_buffer_memory_type;
      return wsi_configure_prime_image(chain, pCreateInfo, use_modifier,
                                       select_buffer_memory_type, params, info);
   } else {
      return wsi_configure_native_image(chain, pCreateInfo,
                                        params->num_modifier_lists,
                                        params->num_modifiers,
                                        params->modifiers,
                                        info);
   }
}
