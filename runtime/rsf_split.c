#include "rsf_split.h"
#include <stdlib.h>
#include <string.h>

VkResult rsf_split_submit(const VkSubmitInfo2 *original, const RsfSplitSection *parts,
                         uint32_t count, RsfSplitSubmit submit, void *user) {
    if (!original || !submit) return VK_ERROR_INITIALIZATION_FAILED;
    if (!count) return submit(original, user);
    if (!parts || !original->commandBufferInfoCount ||
        parts[count - 1].commands != original->pCommandBufferInfos[original->commandBufferInfoCount - 1].commandBuffer)
        return VK_ERROR_INITIALIZATION_FAILED;
    // Validate the entire graph before submitting any section.
    for (uint32_t i = 0; i < count; ++i)
        if (!parts[i].commands || !parts[i].timeline ||
            (i + 1 < count && !parts[i].signal_value) || (i && !parts[i].wait_value))
            return VK_ERROR_INITIALIZATION_FAILED;
    size_t nw = (size_t)original->waitSemaphoreInfoCount + 1;
    size_t ns = (size_t)original->signalSemaphoreInfoCount + 1;
    VkSemaphoreSubmitInfo *waits = calloc(nw, sizeof *waits), *signals = calloc(ns, sizeof *signals);
    VkCommandBufferSubmitInfo *commands = calloc(original->commandBufferInfoCount, sizeof *commands);
    if (!waits || !signals || !commands) { free(waits); free(signals); free(commands); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < count && result == VK_SUCCESS; ++i) {
        VkSubmitInfo2 section = *original;
        section.waitSemaphoreInfoCount = i ? 0 : original->waitSemaphoreInfoCount;
        section.signalSemaphoreInfoCount = i + 1 == count ? original->signalSemaphoreInfoCount : 0;
        if (section.waitSemaphoreInfoCount) memcpy(waits, original->pWaitSemaphoreInfos, section.waitSemaphoreInfoCount * sizeof *waits);
        if (section.signalSemaphoreInfoCount) memcpy(signals, original->pSignalSemaphoreInfos, section.signalSemaphoreInfoCount * sizeof *signals);
        VkSemaphoreSubmitInfo dependency = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = parts[i].timeline, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        if (parts[i].wait_value) { dependency.value = parts[i].wait_value; waits[section.waitSemaphoreInfoCount++] = dependency; }
        if (parts[i].signal_value) { dependency.value = parts[i].signal_value; signals[section.signalSemaphoreInfoCount++] = dependency; }
        section.pWaitSemaphoreInfos = waits; section.pSignalSemaphoreInfos = signals;
        section.commandBufferInfoCount = i ? 1 : original->commandBufferInfoCount;
        if (!i) memcpy(commands, original->pCommandBufferInfos, section.commandBufferInfoCount * sizeof *commands);
        else commands[0] = original->pCommandBufferInfos[original->commandBufferInfoCount - 1];
        commands[section.commandBufferInfoCount - 1].commandBuffer = parts[i].commands;
        section.pCommandBufferInfos = commands;
        result = submit(&section, user);
    }
    free(waits); free(signals); free(commands);
    return result;
}
