// Exercises the actual submit expander without loading Vulkan or an SDK.
#include <assert.h>
#include <stdint.h>
#include "rsf_split.h"
static unsigned calls, fail_at;
static VkSemaphore external, acquire, retirement;
static VkCommandBuffer producer, continuation, prologue;
static VkResult inspect(const VkSubmitInfo2 *si, void *user) {
    assert(user == (void *)123);
    assert(si->pNext == (void *)456);
    unsigned index = calls++;
    assert(si->commandBufferInfoCount == (index ? 1 : 2));
    assert(si->pCommandBufferInfos[index ? 0 : 1].commandBuffer == (index ? continuation : producer));
    if (!index) {
        assert(si->pCommandBufferInfos[0].commandBuffer == prologue);
        assert(si->waitSemaphoreInfoCount == 2);
        assert(si->pWaitSemaphoreInfos[0].semaphore == acquire && si->pWaitSemaphoreInfos[0].value == 0);
        assert(si->pWaitSemaphoreInfos[1].semaphore == external && si->pWaitSemaphoreInfos[1].value == 8);
        assert(si->signalSemaphoreInfoCount == 1 && si->pSignalSemaphoreInfos[0].value == 9);
    } else {
        assert(si->waitSemaphoreInfoCount == 1 && si->pWaitSemaphoreInfos[0].value == 10);
        assert(si->signalSemaphoreInfoCount == 1 && si->pSignalSemaphoreInfos[0].semaphore == retirement);
        assert(si->pSignalSemaphoreInfos[0].value == 77);
    }
    assert(si->pWaitSemaphoreInfos[si->waitSemaphoreInfoCount - 1].stageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    return fail_at == calls ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
int main(void) {
    external = (VkSemaphore)(uintptr_t)1; acquire = (VkSemaphore)(uintptr_t)2; retirement = (VkSemaphore)(uintptr_t)3;
    producer = (void *)4; continuation = (void *)5; prologue = (void *)6;
    VkCommandBufferSubmitInfo cb[] = {{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = prologue},
        {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = continuation}};
    VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = acquire};
    VkSemaphoreSubmitInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = retirement, .value = 77};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .pNext = (void *)456,
        .commandBufferInfoCount = 2, .pCommandBufferInfos = cb, .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait,
        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
    RsfSplitSection sections[] = {{producer, external, 8, 9}, {continuation, external, 10, 0}};
    assert(rsf_split_submit(&si, sections, 2, inspect, (void *)123) == VK_SUCCESS && calls == 2);
    calls = 0; fail_at = 1;
    assert(rsf_split_submit(&si, sections, 2, inspect, (void *)123) == VK_ERROR_DEVICE_LOST && calls == 1);
    calls = 0; sections[1].commands = producer;
    assert(rsf_split_submit(&si, sections, 2, inspect, (void *)123) != VK_SUCCESS && !calls);
    assert(cb[1].commandBuffer == continuation && wait.value == 0 && signal.value == 77);
    return 0;
}
