/* Power-fail-safe A/B firmware update logic. Portable C: flash access goes through flash_t. */
#ifndef BOOT_H
#define BOOT_H
#include <stdint.h>

#define PAGE_SIZE     2048u
#define SLOT_PAGES    8u
#define SLOT_SIZE     (SLOT_PAGES * PAGE_SIZE)
#define HEADER_SPACE  256u                           /* keeps the app vector table aligned */
#define MAX_IMAGE     (SLOT_SIZE - HEADER_SPACE)
#define MAX_TRIES     3u
#define REGION_SIZE   ((2u + 2u * SLOT_PAGES) * PAGE_SIZE)   /* 2 record pages + 2 slots */
#define SLOT_OFFSET(s) ((2u + (uint32_t)(s) * SLOT_PAGES) * PAGE_SIZE)

typedef struct {
    const uint8_t *base;                             /* memory-mapped view of the region */
    int (*erase)(void *ctx, uint32_t page);          /* 0 on success */
    int (*program)(void *ctx, uint32_t offset, const uint8_t data[8]);   /* once per erase */
    void *ctx;
} flash_t;

typedef struct {
    const flash_t *flash;
    uint32_t slot, size, version, crc, written;
    uint8_t  pending[8];
} update_t;

enum { BOOT_OK = 0, BOOT_ERR_FLASH = -1, BOOT_ERR_ARG = -2, BOOT_ERR_STATE = -3, BOOT_ERR_CRC = -4,
       BOOT_ERR_NO_IMAGE = -5 };

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);   /* start with 0 */

/* Call at every reset. Returns the slot to run (0 or 1) or a negative error. */
int boot_select(const flash_t *f);
/* The running application calls this once its self-test passes. */
int boot_confirm(const flash_t *f);

int update_begin(update_t *u, const flash_t *f, uint32_t size, uint32_t version, uint32_t crc);
int update_write(update_t *u, const uint8_t *data, uint32_t len);
int update_finish(update_t *u);                      /* verifies, then activates on trial */

#endif
