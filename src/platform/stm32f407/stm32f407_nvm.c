/*
 * stm32f407_nvm.c - 参数存储的 STM32F407 Flash 后端。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 把 fw_param 的 read/program/erase 映射到 Flash 地址；
 *   2. 用 HAL_FLASHEx_Erase 按扇区擦除；
 *   3. 用 HAL_FLASH_Program 按 32 位字编程并回读校验。
 *
 * 上下文：任务上下文；Flash 擦除/编程期间 CPU 会停顿，不能从 ISR 调用。
 * 依赖：stm32f407_nvm.h、board_config.h、stm32f4xx_hal.h、string.h。
 */
#include "stm32f407_nvm.h"
#include "board_config.h"
#include "stm32f4xx_hal.h"
#include <string.h>

#if defined(BOARD_NVM_ENABLED) && BOARD_NVM_ENABLED

static fw_param_backend_t s_backend;
static bool s_ready;

static uint32_t nvm_slot_size(void)
{
    return (uint32_t)BOARD_NVM_SLOT_KB * 1024U;
}

static uint32_t nvm_total_size(void)
{
    return nvm_slot_size() * 2U;
}

/* Flash 地址 -> F407 扇区号；越界返回 -1。 */
static int sector_for_address(uint32_t address)
{
    uint32_t offset;
    if (address < BOARD_FLASH_ORIGIN) return -1;
    offset = address - BOARD_FLASH_ORIGIN;
    if (offset < 0x10000U) return (int)(offset / 0x4000U);
    if (offset < 0x20000U) return 4;
    if (offset < 0x40000U) return 5;
    if (offset < 0x60000U) return 6;
    if (offset < 0x80000U) return 7;
    if (offset < 0xA0000U) return 8;
    if (offset < 0xC0000U) return 9;
    if (offset < 0xE0000U) return 10;
    if (offset < 0x100000U) return 11;
    return -1;
}

static int nvm_read(void *ctx, uint32_t offset, void *buf, size_t length)
{
    (void)ctx;
    if (buf == NULL) return FW_EINVAL;
    if (offset > nvm_total_size() || length > nvm_total_size() - offset)
        return FW_EINVAL;
    if (length == 0U) return FW_OK;
    memcpy(buf, (const void *)(BOARD_NVM_BASE + offset), length);
    return FW_OK;
}

static int nvm_program(void *ctx, uint32_t offset, const void *buf,
                       size_t length)
{
    const uint8_t *source = (const uint8_t *)buf;
    uint32_t address;
    size_t i;
    int result = FW_OK;

    (void)ctx;
    if (buf == NULL || offset > nvm_total_size() ||
        length > nvm_total_size() - offset ||
        (offset % 4U) != 0U || (length % 4U) != 0U)
        return FW_EINVAL;
    if (length == 0U) return FW_OK;

    address = BOARD_NVM_BASE + offset;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    for (i = 0U; i < length; i += 4U) {
        uint32_t word;
        memcpy(&word, &source[i], sizeof(word));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + (uint32_t)i,
                              word) != HAL_OK ||
            *(volatile uint32_t *)(address + (uint32_t)i) != word) {
            result = FW_EIO;
            break;
        }
    }
    HAL_FLASH_Lock();
    return result;
}

static int nvm_erase(void *ctx, uint32_t offset, size_t length)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0U;
    int sector;
    int result = FW_OK;

    (void)ctx;
    if (offset > nvm_total_size() || length > nvm_total_size() - offset)
        return FW_EINVAL;
    if (nvm_slot_size() != (uint32_t)BOARD_NVM_SECTOR_KB * 1024U)
        return FW_ENOTSUP;
    if (length != nvm_slot_size() || (offset % nvm_slot_size()) != 0U)
        return FW_EINVAL;

    sector = sector_for_address(BOARD_NVM_BASE + offset);
    if (sector < 0) return FW_EINVAL;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Banks = 0U;
    erase.Sector = (uint32_t)sector;
    erase.NbSectors = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
        result = FW_EIO;
    HAL_FLASH_Lock();
    return result;
}

fw_status_t stm32f407_nvm_init(void)
{
    if (BOARD_NVM_SLOT_KB == 0U || BOARD_NVM_SECTOR_KB == 0U)
        return FW_ENOTSUP;
    if (nvm_slot_size() != (uint32_t)BOARD_NVM_SECTOR_KB * 1024U)
        return FW_ENOTSUP;

    s_backend.read = nvm_read;
    s_backend.program = nvm_program;
    s_backend.erase = nvm_erase;
    s_backend.slot_size = nvm_slot_size();
    s_backend.ctx = NULL;
    s_ready = true;
    return FW_OK;
}

const fw_param_backend_t *stm32f407_nvm_backend(void)
{
    return s_ready ? &s_backend : NULL;
}

#else /* !BOARD_NVM_ENABLED */

fw_status_t stm32f407_nvm_init(void)
{
    return FW_ENOTSUP;
}

const fw_param_backend_t *stm32f407_nvm_backend(void)
{
    return NULL;
}

#endif
