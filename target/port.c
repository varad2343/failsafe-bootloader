/* STM32G0B1 port: flash driver and hand-over to the application. Not yet tested on hardware. */
#include "boot.h"
#include <stddef.h>

#define REG(a)     (*(volatile uint32_t *)(a))
#define FLASH_KEYR REG(0x40022008u)
#define FLASH_SR   REG(0x40022010u)
#define FLASH_CR   REG(0x40022014u)
#define SCB_VTOR   REG(0xE000ED08u)
#define SR_BUSY    (7u << 16)          /* BSY1, BSY2, CFGBSY */
#define SR_ERRORS  0x3FAu
#define CR_PG      (1u << 0)
#define CR_PER     (1u << 1)
#define CR_STRT    (1u << 16)
#define CR_LOCK    (1u << 31)

#define REGION_BASE 0x08008000u        /* after the 32 KB reserved for the bootloader */
#define FIRST_PAGE  ((REGION_BASE - 0x08000000u) / PAGE_SIZE)

static void unlock(void)
{
    while (FLASH_SR & SR_BUSY) { }
    if (FLASH_CR & CR_LOCK) { FLASH_KEYR = 0x45670123u; FLASH_KEYR = 0xCDEF89ABu; }
    FLASH_SR = SR_ERRORS;
}

static int finish(void)
{
    while (FLASH_SR & SR_BUSY) { }
    int error = (FLASH_SR & SR_ERRORS) != 0;
    FLASH_CR = CR_LOCK;
    return error ? -1 : 0;
}

static int port_erase(void *ctx, uint32_t page)
{
    (void)ctx;
    unlock();
    FLASH_CR = CR_PER | ((FIRST_PAGE + page) << 3);
    FLASH_CR |= CR_STRT;
    return finish();
}

static int port_program(void *ctx, uint32_t offset, const uint8_t d[8])
{
    (void)ctx;
    uint32_t low  = d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    uint32_t high = d[4] | (uint32_t)d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
    unlock();
    FLASH_CR = CR_PG;
    REG(REGION_BASE + offset) = low;
    __asm volatile ("isb");
    REG(REGION_BASE + offset + 4u) = high;
    return finish();
}

int main(void)
{
    static const flash_t flash = { (const uint8_t *)REGION_BASE, port_erase, port_program, NULL };
    int slot = boot_select(&flash);
    if (slot < 0) for (;;) { }         /* nothing valid to run */
    const uint32_t *vectors = (const uint32_t *)(REGION_BASE + SLOT_OFFSET(slot) + HEADER_SPACE);
    SCB_VTOR = (uint32_t)vectors;
    __asm volatile ("msr msp, %0\n bx %1" :: "r" (vectors[0]), "r" (vectors[1]));
    for (;;) { }
}
