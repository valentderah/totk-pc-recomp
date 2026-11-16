// Internal snapshot leases for object/bone history. Include after vk.h.
#pragma once
int vk_mvprev_self(int index, uint64_t addr, uint64_t size, uint64_t fence,
                   VkDescriptorBufferInfo *out, const uint8_t **bytes);
void vk_mvprev_clear(void);
void vk_mvprev_storage(int key, uint64_t identity, uint64_t addr, uint64_t size);
