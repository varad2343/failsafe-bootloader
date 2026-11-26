#include "boot.h"
#include <stddef.h>
#include <string.h>

#define IMAGE_MAGIC  0x31474D49u        /* "IMG1" */
#define RECORD_MAGIC 0x31434552u        /* "REC1" */
enum { CONFIRMED = 1, TRIAL = 2 };

typedef struct { uint32_t magic, version, size, crc; } header_t;
typedef struct { uint32_t magic, seq, slot, state, tries, crc; } record_t;   /* 24 bytes */

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int image_valid(const flash_t *f, uint32_t slot, header_t *out)
{
    header_t h;
    memcpy(&h, f->base + SLOT_OFFSET(slot), sizeof h);
    if (h.magic != IMAGE_MAGIC || h.size == 0 || h.size > MAX_IMAGE) return 0;
    if (crc32_update(0, f->base + SLOT_OFFSET(slot) + HEADER_SPACE, h.size) != h.crc) return 0;
    if (out) *out = h;
    return 1;
}

/* Newest intact record of the two pages. Returns its page, or -1 when there is none. */
static int read_record(const flash_t *f, record_t *out)
{
    int found = -1;
    for (int page = 0; page < 2; page++) {
        record_t r;
        memcpy(&r, f->base + (uint32_t)page * PAGE_SIZE, sizeof r);
        if (r.magic != RECORD_MAGIC || r.slot > 1) continue;
        if (crc32_update(0, (const uint8_t *)&r, offsetof(record_t, crc)) != r.crc) continue;
        if (found < 0 || r.seq > out->seq) { *out = r; found = page; }
    }
    return found;
}

/* Always writes the page that does NOT hold the newest record, so a torn write loses nothing. */
static int write_record(const flash_t *f, uint32_t slot, uint32_t state, uint32_t tries)
{
    record_t old, r;
    int page = read_record(f, &old);
    uint32_t target = (page == 0) ? 1u : 0u;
    r.magic = RECORD_MAGIC;
    r.seq = (page < 0) ? 1u : old.seq + 1u;
    r.slot = slot;
    r.state = state;
    r.tries = tries;
    r.crc = crc32_update(0, (const uint8_t *)&r, offsetof(record_t, crc));
    if (f->erase(f->ctx, target)) return BOOT_ERR_FLASH;
    for (uint32_t i = 0; i < sizeof r; i += 8)
        if (f->program(f->ctx, target * PAGE_SIZE + i, (const uint8_t *)&r + i)) return BOOT_ERR_FLASH;
    return BOOT_OK;
}

int boot_select(const flash_t *f)
{
    record_t r;
    header_t h[2];
    int valid[2] = { image_valid(f, 0, &h[0]), image_valid(f, 1, &h[1]) };

    if (read_record(f, &r) < 0) {                     /* factory state: newest valid image */
        if (!valid[0] && !valid[1]) return BOOT_ERR_NO_IMAGE;
        int slot = (valid[1] && (!valid[0] || h[1].version > h[0].version)) ? 1 : 0;
        return write_record(f, (uint32_t)slot, CONFIRMED, 0) ? BOOT_ERR_FLASH : slot;
    }
    int slot = (int)r.slot;
    int other = 1 - slot;
    int give_up = !valid[slot] || (r.state == TRIAL && r.tries >= MAX_TRIES);
    if (give_up) {                                    /* roll back */
        if (!valid[other]) return BOOT_ERR_NO_IMAGE;
        return write_record(f, (uint32_t)other, CONFIRMED, 0) ? BOOT_ERR_FLASH : other;
    }
    if (r.state == TRIAL && write_record(f, r.slot, TRIAL, r.tries + 1u)) return BOOT_ERR_FLASH;
    return slot;
}

int boot_confirm(const flash_t *f)
{
    record_t r;
    if (read_record(f, &r) < 0) return BOOT_ERR_STATE;
    if (r.state == CONFIRMED) return BOOT_OK;
    return write_record(f, r.slot, CONFIRMED, 0);
}

int update_begin(update_t *u, const flash_t *f, uint32_t size, uint32_t version, uint32_t crc)
{
    record_t r;
    if (size == 0 || size > MAX_IMAGE) return BOOT_ERR_ARG;
    /* An unconfirmed image must keep its fallback: refuse to erase it. */
    if (read_record(f, &r) < 0 || r.state != CONFIRMED) return BOOT_ERR_STATE;
    u->flash = f;
    u->slot = 1u - r.slot;
    u->size = size;
    u->version = version;
    u->crc = crc;
    u->written = 0;
    for (uint32_t p = 0; p < SLOT_PAGES; p++)
        if (f->erase(f->ctx, SLOT_OFFSET(u->slot) / PAGE_SIZE + p)) return BOOT_ERR_FLASH;
    return BOOT_OK;
}

int update_write(update_t *u, const uint8_t *data, uint32_t len)
{
    if (len > u->size - u->written) return BOOT_ERR_ARG;
    while (len--) {
        u->pending[u->written % 8u] = *data++;
        u->written++;
        if (u->written % 8u == 0 || u->written == u->size) {
            uint32_t start = (u->written - 1u) / 8u * 8u;
            for (uint32_t i = u->written - start; i < 8u; i++) u->pending[i] = 0xFF;
            if (u->flash->program(u->flash->ctx, SLOT_OFFSET(u->slot) + HEADER_SPACE + start, u->pending))
                return BOOT_ERR_FLASH;
        }
    }
    return BOOT_OK;
}

int update_finish(update_t *u)
{
    const flash_t *f = u->flash;
    if (u->written != u->size) return BOOT_ERR_STATE;
    if (crc32_update(0, f->base + SLOT_OFFSET(u->slot) + HEADER_SPACE, u->size) != u->crc) return BOOT_ERR_CRC;
    header_t h = { IMAGE_MAGIC, u->version, u->size, u->crc };    /* header goes last */
    for (uint32_t i = 0; i < sizeof h; i += 8)
        if (f->program(f->ctx, SLOT_OFFSET(u->slot) + i, (const uint8_t *)&h + i)) return BOOT_ERR_FLASH;
    return write_record(f, u->slot, TRIAL, 0);
}
