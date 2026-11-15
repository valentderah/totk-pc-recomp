// Internal snapshot leases for object/bone history. Include after vk.h.
#pragma once
void *vk_usnap_retain(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out, const uint8_t **bytes);
void vk_usnap_release(void *lease);
void vk_usnap_use(void *lease, uint64_t fence);
int vk_mvprev_self(int index, uint64_t addr, uint64_t size, uint64_t fence,
                   VkDescriptorBufferInfo *out, const uint8_t **bytes);
void vk_mvprev_clear(void);
void vk_mvprev_storage(int key, uint64_t identity, uint64_t addr, uint64_t size);
