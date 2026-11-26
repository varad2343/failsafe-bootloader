/* Host tests on a flash model that can lose power during any erase or program operation. */
#include "boot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t mem[REGION_SIZE]; long ops, fail_at; int dead; unsigned seed; } sim_t;
static sim_t sim;
static long checks;

#define CHECK(c) do { checks++; if (!(c)) { printf("FAILED line %d (fail_at %ld): %s\n", __LINE__, sim.fail_at, #c); exit(1); } } while (0)

static unsigned rnd(void) { sim.seed = sim.seed * 1103515245u + 12345u; return sim.seed >> 16; }

static int sim_erase(void *ctx, uint32_t page)
{
    (void)ctx;
    if (sim.dead) return -1;
    CHECK(page < REGION_SIZE / PAGE_SIZE);
    if (++sim.ops == sim.fail_at) {           /* torn erase: random bytes erased, the rest kept */
        for (uint32_t i = 0; i < PAGE_SIZE; i++) if (rnd() & 1u) sim.mem[page * PAGE_SIZE + i] = 0xFF;
        sim.dead = 1;
        return -1;
    }
    memset(&sim.mem[page * PAGE_SIZE], 0xFF, PAGE_SIZE);
    return 0;
}

static int sim_program(void *ctx, uint32_t offset, const uint8_t data[8])
{
    (void)ctx;
    if (sim.dead) return -1;
    CHECK(offset % 8u == 0 && offset + 8u <= REGION_SIZE);
    for (int i = 0; i < 8; i++) CHECK(sim.mem[offset + i] == 0xFF);   /* never program twice */
    if (++sim.ops == sim.fail_at) {           /* torn program: only some bits get cleared */
        for (int i = 0; i < 8; i++) sim.mem[offset + i] &= (uint8_t)(data[i] | rnd());
        sim.dead = 1;
        return -1;
    }
    memcpy(&sim.mem[offset], data, 8);
    return 0;
}

static const flash_t flash = { sim.mem, sim_erase, sim_program, NULL };
static uint8_t image[MAX_IMAGE];

static void make_image(uint32_t size, uint32_t version)
{
    for (uint32_t i = 0; i < size; i++) image[i] = (uint8_t)(i * 31u + version * 7u);
}

static int do_update(uint32_t size, uint32_t version)
{
    update_t u;
    make_image(size, version);
    int e = update_begin(&u, &flash, size, version, crc32_update(0, image, size));
    for (uint32_t at = 0; !e && at < size; at += 61) e = update_write(&u, image + at, size - at < 61 ? size - at : 61);
    return e ? e : update_finish(&u);
}

/* Version of the image in a slot, but only if its contents are exactly what was sent. */
static uint32_t version_in(int slot)
{
    uint32_t h[4];
    memcpy(h, sim.mem + SLOT_OFFSET(slot), sizeof h);
    CHECK(h[2] <= MAX_IMAGE);
    make_image(h[2], h[1]);
    CHECK(memcmp(image, sim.mem + SLOT_OFFSET(slot) + HEADER_SPACE, h[2]) == 0);
    return h[1];
}

/* Blank chip, then version 1 installed by a programmer into slot 0. */
static void factory(void)
{
    memset(&sim, 0, sizeof sim);
    memset(sim.mem, 0xFF, sizeof sim.mem);
    sim.seed = 1;
    make_image(3000, 1);
    uint32_t h[4] = { 0x31474D49u, 1, 3000, crc32_update(0, image, 3000) };
    memcpy(sim.mem + SLOT_OFFSET(0), h, sizeof h);
    memcpy(sim.mem + SLOT_OFFSET(0) + HEADER_SPACE, image, 3000);
}

static void power_cycle(void) { sim.dead = 0; sim.fail_at = 0; }

static void basic_tests(void)
{
    CHECK(crc32_update(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    factory();
    memset(sim.mem + SLOT_OFFSET(0), 0xFF, SLOT_SIZE);
    CHECK(boot_select(&flash) == BOOT_ERR_NO_IMAGE);

    factory();
    CHECK(boot_select(&flash) == 0);
    CHECK(do_update(5001, 2) == BOOT_OK);
    CHECK(boot_select(&flash) == 1 && version_in(1) == 2);
    CHECK(do_update(100, 3) == BOOT_ERR_STATE);          /* unconfirmed: fallback protected */
    CHECK(boot_confirm(&flash) == BOOT_OK);
    for (int i = 0; i < 10; i++) CHECK(boot_select(&flash) == 1);

    CHECK(do_update(MAX_IMAGE, 3) == BOOT_OK);           /* largest image */
    CHECK(boot_select(&flash) == 0 && version_in(0) == 3);
}

static void argument_tests(void)
{
    update_t u;
    factory();
    CHECK(boot_select(&flash) == 0);
    CHECK(update_begin(&u, &flash, 0, 2, 0) == BOOT_ERR_ARG);
    CHECK(update_begin(&u, &flash, MAX_IMAGE + 1, 2, 0) == BOOT_ERR_ARG);
    CHECK(update_begin(&u, &flash, 16, 2, 0x12345678u) == BOOT_OK);
    CHECK(update_finish(&u) == BOOT_ERR_STATE);          /* incomplete */
    CHECK(update_write(&u, image, 17) == BOOT_ERR_ARG);  /* too long */
    CHECK(update_write(&u, image, 16) == BOOT_OK);
    CHECK(update_finish(&u) == BOOT_ERR_CRC);
    CHECK(boot_select(&flash) == 0);
}

/* A new image that is never confirmed must be replaced by the old one. */
static void rollback_test(void)
{
    factory();
    CHECK(boot_select(&flash) == 0);
    CHECK(do_update(4000, 2) == BOOT_OK);
    for (unsigned i = 0; i < MAX_TRIES; i++) CHECK(boot_select(&flash) == 1);
    CHECK(boot_select(&flash) == 0 && version_in(0) == 1);
    CHECK(boot_select(&flash) == 0);
}

/* A confirmed image that later gets corrupted must not be started. */
static void corruption_test(void)
{
    factory();
    CHECK(boot_select(&flash) == 0);
    CHECK(do_update(4000, 2) == BOOT_OK);
    CHECK(boot_select(&flash) == 1 && boot_confirm(&flash) == BOOT_OK);
    sim.mem[SLOT_OFFSET(1) + HEADER_SPACE + 100] ^= 1u;
    CHECK(boot_select(&flash) == 0 && version_in(0) == 1);
}

/* Losing power while counting trial boots must never turn an untested image into a confirmed one. */
static long trial_power_fail_sweep(void)
{
    long cases = 0;
    for (long k = 1;; k++) {
        factory();
        CHECK(boot_select(&flash) == 0);
        CHECK(do_update(4000, 2) == BOOT_OK);
        sim.ops = 0;
        sim.fail_at = k;
        sim.seed = (unsigned)k;
        for (unsigned i = 0; i <= MAX_TRIES && !sim.dead; i++) (void)boot_select(&flash);
        int interrupted = sim.dead;
        power_cycle();
        int slot = -1;
        for (unsigned i = 0; i < MAX_TRIES + 2u; i++) {
            slot = boot_select(&flash);
            CHECK(slot == 0 || slot == 1);
        }
        CHECK(slot == 0 && version_in(0) == 1);
        if (!interrupted) break;
        cases++;
    }
    return cases;
}

/* Cut power at every single flash operation of a whole update cycle. */
static long power_fail_sweep(void)
{
    long cases = 0;
    for (long k = 1;; k++) {
        factory();
        CHECK(boot_select(&flash) == 0);
        sim.ops = 0;
        sim.fail_at = k;
        sim.seed = (unsigned)k;
        int e = do_update(4001, 2);
        if (!e) { int s = boot_select(&flash); if (s >= 0) e = boot_confirm(&flash); }
        int interrupted = sim.dead;
        power_cycle();

        int slot = boot_select(&flash);                   /* must always find a good image */
        CHECK(slot == 0 || slot == 1);
        uint32_t v = version_in(slot);
        CHECK((slot == 0 && v == 1) || (slot == 1 && v == 2));

        /* the device must still be updatable afterwards */
        CHECK(boot_confirm(&flash) == BOOT_OK);
        CHECK(do_update(2000, 9) == BOOT_OK);
        int next = boot_select(&flash);
        CHECK(next >= 0 && next != slot && version_in(next) == 9);
        if (!interrupted) break;                          /* k is past the last operation */
        cases++;
    }
    return cases;
}

int main(void)
{
    basic_tests();
    argument_tests();
    rollback_test();
    corruption_test();
    long cases = power_fail_sweep();
    CHECK(cases > 500);
    long trial = trial_power_fail_sweep();
    CHECK(trial >= 12);
    cases += trial;
    printf("ALL TESTS PASSED (%ld power-failure points, %ld checks)\n", cases, checks);
    return 0;
}
