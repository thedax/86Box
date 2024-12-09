/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Common code to handle all sorts of hard disk images.
 *
 *
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>
 *          Fred N. van Kempen, <decwiz@yahoo.com>
 *
 *          Copyright 2016-2019 Miran Grca.
 *          Copyright 2017-2019 Fred N. van Kempen.
 */
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <wchar.h>
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/ui.h>
#include <86box/hdd.h>
#include <86box/cdrom.h>
#include <86box/video.h>
#include "cpu.h"

#define HDD_OVERHEAD_TIME 50.0

hard_disk_t hdd[HDD_NUM];

int
hdd_init(void)
{
    /* Clear all global data. */
    memset(hdd, 0x00, sizeof(hdd));

    return 0;
}

int
hdd_string_to_bus(char *str, int cdrom)
{
    if (!strcmp(str, "none"))
        return HDD_BUS_DISABLED;

    if (!strcmp(str, "mfm")) {
        if (cdrom) {
no_cdrom:
            ui_msgbox_header(MBX_ERROR, plat_get_string(STRING_INVALID_CONFIG), plat_get_string(STRING_NO_ST506_ESDI_CDROM));
            return 0;
        }

        return HDD_BUS_MFM;
    }

    if (!strcmp(str, "esdi")) {
        if (cdrom)
            goto no_cdrom;

        return HDD_BUS_ESDI;
    }

    if (!strcmp(str, "ide"))
        return HDD_BUS_IDE;

    if (!strcmp(str, "atapi"))
        return HDD_BUS_ATAPI;

    if (!strcmp(str, "xta"))
        return HDD_BUS_XTA;

    if (!strcmp(str, "scsi"))
        return HDD_BUS_SCSI;

    return 0;
}

char *
hdd_bus_to_string(int bus, UNUSED(int cdrom))
{
    char *s = "none";

    switch (bus) {
        default:
        case HDD_BUS_DISABLED:
            break;

        case HDD_BUS_MFM:
            s = "mfm";
            break;

        case HDD_BUS_XTA:
            s = "xta";
            break;

        case HDD_BUS_ESDI:
            s = "esdi";
            break;

        case HDD_BUS_IDE:
            s = "ide";
            break;

        case HDD_BUS_ATAPI:
            s = "atapi";
            break;

        case HDD_BUS_SCSI:
            s = "scsi";
            break;
    }

    return s;
}

int
hdd_is_valid(int c)
{
    if (hdd[c].bus == HDD_BUS_DISABLED)
        return 0;

    if (strlen(hdd[c].fn) == 0)
        return 0;

    if ((hdd[c].tracks == 0) || (hdd[c].hpc == 0) || (hdd[c].spt == 0))
        return 0;

    return 1;
}

double
hdd_seek_get_time(hard_disk_t *hdd, uint32_t dst_addr, uint8_t operation, uint8_t continuous, double max_seek_time)
{
    if (!hdd->speed_preset)
        return HDD_OVERHEAD_TIME;

    const hdd_zone_t *zone = NULL;
    if (hdd->num_zones <= 0) {
        fatal("hdd_seek_get_time(): hdd->num_zones < 0)\n");
        return 0.0;
    }
    for (uint32_t i = 0; i < hdd->num_zones; i++) {
        zone = &hdd->zones[i];
        if (zone->end_sector >= dst_addr)
            break;
    }

    double continuous_times[2][2] = {
        {hdd->head_switch_usec,   hdd->cyl_switch_usec  },
        { zone->sector_time_usec, zone->sector_time_usec}
    };
    double times[2] = { HDD_OVERHEAD_TIME, hdd->avg_rotation_lat_usec };

    uint32_t new_track     = zone->start_track + ((dst_addr - zone->start_sector) / zone->sectors_per_track);
    uint32_t new_cylinder  = new_track / hdd->phy_heads;
    uint32_t cylinder_diff = abs((int) hdd->cur_cylinder - (int) new_cylinder);

    bool sequential = dst_addr == hdd->cur_addr + 1;
    continuous      = continuous && sequential;

    double seek_time = 0.0;
    if (continuous)
        seek_time = continuous_times[new_track == hdd->cur_track][!!cylinder_diff];
    else {
        if (!cylinder_diff)
            seek_time = times[operation != HDD_OP_SEEK];
        else {
            seek_time = hdd->cyl_switch_usec + (hdd->full_stroke_usec * (double) cylinder_diff / (double) hdd->phy_cyl) + ((operation != HDD_OP_SEEK) * hdd->avg_rotation_lat_usec);
        }
    }

    if (!max_seek_time || seek_time <= max_seek_time) {
        hdd->cur_addr     = dst_addr;
        hdd->cur_track    = new_track;
        hdd->cur_cylinder = new_cylinder;
    }

    return seek_time;
}

static void
hdd_readahead_update(hard_disk_t *hdd)
{
    uint64_t elapsed_cycles;
    double   elapsed_us;
    double   seek_time;
    int32_t  max_read_ahead;
    uint32_t space_needed;

    hdd_cache_t *cache = &hdd->cache;
    if (cache->ra_ongoing) {
        hdd_cache_seg_t *segment = &cache->segments[cache->ra_segment];

        elapsed_cycles = tsc - cache->ra_start_time;
        elapsed_us     = (double) elapsed_cycles / cpuclock * 1000000.0;
        /* Do not overwrite data not yet read by host */
        max_read_ahead = (segment->host_addr + cache->segment_size) - segment->ra_addr;

        seek_time = 0.0;

        for (int32_t i = 0; i < max_read_ahead; i++) {
            seek_time += hdd_seek_get_time(hdd, segment->ra_addr, HDD_OP_READ, 1, elapsed_us - seek_time);
            if (seek_time > elapsed_us)
                break;

            segment->ra_addr++;
        }

        if (segment->ra_addr > segment->lba_addr + cache->segment_size) {
            space_needed = segment->ra_addr - (segment->lba_addr + cache->segment_size);
            segment->lba_addr += space_needed;
        }
    }
}

static double
hdd_writecache_flush(hard_disk_t *hdd)
{
    double seek_time = 0.0;

    while (hdd->cache.write_pending) {
        seek_time += hdd_seek_get_time(hdd, hdd->cache.write_addr, HDD_OP_WRITE, 1, 0);
        hdd->cache.write_addr++;
        hdd->cache.write_pending--;
    }

    return seek_time;
}

static void
hdd_writecache_update(hard_disk_t *hdd)
{
    uint64_t elapsed_cycles;
    double   elapsed_us;
    double   seek_time;

    if (hdd->cache.write_pending) {
        elapsed_cycles = tsc - hdd->cache.write_start_time;
        elapsed_us     = (double) elapsed_cycles / cpuclock * 1000000.0;
        seek_time      = 0.0;

        while (hdd->cache.write_pending) {
            seek_time += hdd_seek_get_time(hdd, hdd->cache.write_addr, HDD_OP_WRITE, 1, elapsed_us - seek_time);
            if (seek_time > elapsed_us)
                break;

            hdd->cache.write_addr++;
            hdd->cache.write_pending--;
        }
    }
}

double
hdd_timing_write(hard_disk_t *hdd, uint32_t addr, uint32_t len)
{
    double   seek_time = 0.0;
    uint32_t flush_needed;

    if (!hdd->speed_preset)
        return HDD_OVERHEAD_TIME;

    hdd_readahead_update(hdd);
    hdd_writecache_update(hdd);

    hdd->cache.ra_ongoing = 0;

    if (hdd->cache.write_pending && (addr != (hdd->cache.write_addr + hdd->cache.write_pending))) {
        /* New request is not sequential to existing cache, need to flush it */
        seek_time += hdd_writecache_flush(hdd);
    }

    if (!hdd->cache.write_pending) {
        /* Cache is empty */
        hdd->cache.write_addr = addr;
    }

    hdd->cache.write_pending += len;
    if (hdd->cache.write_pending > hdd->cache.write_size) {
        /* If request is bigger than free cache, flush some data first */
        flush_needed = hdd->cache.write_pending - hdd->cache.write_size;
        for (uint32_t i = 0; i < flush_needed; i++) {
            seek_time += hdd_seek_get_time(hdd, hdd->cache.write_addr, HDD_OP_WRITE, 1, 0);
            hdd->cache.write_addr++;
        }
    }

    hdd->cache.write_start_time = tsc + (uint32_t) (seek_time * cpuclock / 1000000.0);

    return seek_time;
}

double
hdd_timing_read(hard_disk_t *hdd, uint32_t addr, uint32_t len)
{
    double seek_time = 0.0;

    if (!hdd->speed_preset)
        return HDD_OVERHEAD_TIME;

    hdd_readahead_update(hdd);
    hdd_writecache_update(hdd);

    seek_time += hdd_writecache_flush(hdd);

    hdd_cache_t     *cache      = &hdd->cache;
    hdd_cache_seg_t *active_seg = &cache->segments[0];

    for (uint32_t i = 0; i < cache->num_segments; i++) {
        hdd_cache_seg_t *segment = &cache->segments[i];
        if (!segment->valid) {
            active_seg = segment;
            continue;
        }

        if (segment->lba_addr <= addr && (segment->lba_addr + cache->segment_size) >= addr) {
            /* Cache HIT */
            segment->host_addr = addr;
            active_seg         = segment;
            if (addr + len > segment->ra_addr) {
                uint32_t need_read = (addr + len) - segment->ra_addr;
                for (uint32_t j = 0; j < need_read; j++) {
                    seek_time += hdd_seek_get_time(hdd, segment->ra_addr, HDD_OP_READ, 1, 0.0);
                    segment->ra_addr++;
                }
            }
            if (addr + len > segment->lba_addr + cache->segment_size) {
                /* Need to erase some previously cached data */
                uint32_t space_needed = (addr + len) - (segment->lba_addr + cache->segment_size);
                segment->lba_addr += space_needed;
            }
            goto update_lru;
        } else {
            if (segment->lru > active_seg->lru)
                active_seg = segment;
        }
    }

    /* Cache MISS */
    active_seg->lba_addr  = addr;
    active_seg->valid     = 1;
    active_seg->host_addr = addr;
    active_seg->ra_addr   = addr;

    for (uint32_t i = 0; i < len; i++) {
        seek_time += hdd_seek_get_time(hdd, active_seg->ra_addr, HDD_OP_READ, i != 0, 0.0);
        active_seg->ra_addr++;
    }

update_lru:
    for (uint32_t i = 0; i < cache->num_segments; i++)
        cache->segments[i].lru++;

    active_seg->lru = 0;

    cache->ra_ongoing    = 1;
    cache->ra_segment    = active_seg->id;
    cache->ra_start_time = tsc + (uint32_t) (seek_time * cpuclock / 1000000.0);

    return seek_time;
}

static void
hdd_cache_init(hard_disk_t *hdd)
{
    hdd_cache_t *cache = &hdd->cache;

    cache->ra_segment    = 0;
    cache->ra_ongoing    = 0;
    cache->ra_start_time = 0;

    for (uint32_t i = 0; i < cache->num_segments; i++) {
        cache->segments[i].valid     = 0;
        cache->segments[i].lru       = 0;
        cache->segments[i].id        = i;
        cache->segments[i].ra_addr   = 0;
        cache->segments[i].host_addr = 0;
    }
}

static void
hdd_zones_init(hard_disk_t *hdd)
{
    uint32_t    lba = 0;
    uint32_t    track = 0;
    uint32_t    tracks;
    double      revolution_usec = 60.0 / (double) hdd->rpm * 1000000.0;
    hdd_zone_t *zone;

    for (uint32_t i = 0; i < hdd->num_zones; i++) {
        zone                   = &hdd->zones[i];
        zone->start_sector     = lba;
        zone->start_track      = track;
        zone->sector_time_usec = revolution_usec / (double) zone->sectors_per_track;
        tracks                 = zone->cylinders * hdd->phy_heads;
        lba += tracks * zone->sectors_per_track;
        zone->end_sector = lba - 1;
        track += tracks - 1;
    }
}

static hdd_preset_t hdd_speed_presets[] = {
  // clang-format off
    { .name = "RAM Disk (max. speed)",                            .internal_name = "ramdisk",                                                                                                        .rcache_num_seg = 16, .rcache_seg_size = 128, .max_multiple = 32 },
    { .name = "[1989] 3500 RPM",                                  .internal_name = "1989_3500rpm", .zones =  1,  .avg_spt = 35, .heads = 2, .rpm = 3500, .full_stroke_ms = 40, .track_seek_ms = 8,   .rcache_num_seg =  1, .rcache_seg_size =  16, .max_multiple =  8 },
    { .name = "[1992] 3600 RPM",                                  .internal_name = "1992_3600rpm", .zones =  1,  .avg_spt = 45, .heads = 2, .rpm = 3600, .full_stroke_ms = 30, .track_seek_ms = 6,   .rcache_num_seg =  4, .rcache_seg_size =  16, .max_multiple =  8 },
    { .name = "[1994] 4500 RPM",                                  .internal_name = "1994_4500rpm", .zones =  8,  .avg_spt = 80, .heads = 4, .rpm = 4500, .full_stroke_ms = 26, .track_seek_ms = 5,   .rcache_num_seg =  4, .rcache_seg_size =  32, .max_multiple = 16 },
    { .name = "[1996] 5400 RPM",                                  .internal_name = "1996_5400rpm", .zones = 16, .avg_spt = 135, .heads = 4, .rpm = 5400, .full_stroke_ms = 24, .track_seek_ms = 3,   .rcache_num_seg =  4, .rcache_seg_size =  64, .max_multiple = 16 },
    { .name = "[1997] 5400 RPM",                                  .internal_name = "1997_5400rpm", .zones = 16, .avg_spt = 185, .heads = 6, .rpm = 5400, .full_stroke_ms = 20, .track_seek_ms = 2.5, .rcache_num_seg =  8, .rcache_seg_size =  64, .max_multiple = 32 },
    { .name = "[1998] 5400 RPM",                                  .internal_name = "1998_5400rpm", .zones = 16, .avg_spt = 300, .heads = 8, .rpm = 5400, .full_stroke_ms = 20, .track_seek_ms = 2,   .rcache_num_seg =  8, .rcache_seg_size = 128, .max_multiple = 32 },
    { .name = "[2000] 7200 RPM",                                  .internal_name = "2000_7200rpm", .zones = 16, .avg_spt = 350, .heads = 6, .rpm = 7200, .full_stroke_ms = 15, .track_seek_ms = 2,   .rcache_num_seg = 16, .rcache_seg_size = 128, .max_multiple = 32 },
    { .name = "[ESDI] Fujitsu M2263E",                            .internal_name = "M2263E", .model = "FUJITSU M2263E", .zones =  1,  .avg_spt = 160, .heads = 8, .rpm = 3600, .full_stroke_ms = 30, .track_seek_ms = 4,   .rcache_num_seg =  4, .rcache_seg_size =  16, .max_multiple =  1 },
    { .name = "[PIO IDE] IBM WDA-L42",                            .internal_name = "WDAL42", .model = "IBM-WDA-L42", .zones =  1,  .avg_spt = 85, .heads = 2, .rpm = 3600, .full_stroke_ms = 33, .track_seek_ms = 2.5,   .rcache_num_seg =  1, .rcache_seg_size =  32, .max_multiple =  1 },
    { .name = "[ATA-1] Conner CP3024",                            .internal_name = "CP3024", .model = "Conner Peripherals 20MB - CP3024", .zones =  1,  .avg_spt = 33, .heads = 2, .rpm = 3500, .full_stroke_ms = 50, .track_seek_ms = 8,   .rcache_num_seg =  1, .rcache_seg_size =  8, .max_multiple =  8 },
    { .name = "[ATA-1] Conner CP3044",                            .internal_name = "CP3044", .model = "Conner Peripherals 40MB - CP3044", .zones =  1,  .avg_spt = 40, .heads = 2, .rpm = 3500, .full_stroke_ms = 50, .track_seek_ms = 8,   .rcache_num_seg =  1, .rcache_seg_size =  8, .max_multiple =  8 },
    { .name = "[ATA-1] Conner CP3104",                            .internal_name = "CP3104", .model = "Conner Peripherals 104MB - CP3104", .zones =  1,  .avg_spt = 33, .heads = 8, .rpm = 3500, .full_stroke_ms = 45, .track_seek_ms = 8,   .rcache_num_seg =  4, .rcache_seg_size =  8, .max_multiple =  8 },
    { .name = "[ATA-1] IBM H3256-A3",                             .internal_name = "H3256A3", .model = "IBM-H3256-A3", .zones =  1,  .avg_spt = 140, .heads = 2, .rpm = 3600, .full_stroke_ms = 32, .track_seek_ms = 4,   .rcache_num_seg =  4, .rcache_seg_size =  96, .max_multiple =  8 },
    { .name = "[ATA-1] Maxtor 7131AT",                            .internal_name = "7131AT", .model = "Maxtor 7131AT", .zones =  2,  .avg_spt = 154, .heads = 2, .rpm = 3551, .full_stroke_ms = 27, .track_seek_ms = 4.5,   .rcache_num_seg =  1, .rcache_seg_size =  64, .max_multiple =  8 },
    { .name = "[ATA-1] Maxtor 7213AT",                            .internal_name = "7213AT", .model = "Maxtor 7213AT", .zones =  4,  .avg_spt = 155, .heads = 4, .rpm = 3551, .full_stroke_ms = 28, .track_seek_ms = 6.5,   .rcache_num_seg =  1, .rcache_seg_size =  64, .max_multiple =  8 },
    { .name = "[ATA-1] Maxtor 7245AT",                            .internal_name = "7245AT", .model = "Maxtor 7245AT", .zones =  4,  .avg_spt = 149, .heads = 4, .rpm = 3551, .full_stroke_ms = 27, .track_seek_ms = 4.4,   .rcache_num_seg =  8, .rcache_seg_size =  64, .max_multiple =  16 },
    { .name = "[ATA-2] IBM DBOA-2720",                            .internal_name = "DBOA2720", .model = "IBM-DBOA-2720", .zones =  2,  .avg_spt = 135, .heads = 2, .rpm = 4000, .full_stroke_ms = 30, .track_seek_ms = 5,   .rcache_num_seg =  4, .rcache_seg_size =  64, .max_multiple =  16 },
    { .name = "[ATA-2] Maxtor 7850AV",                            .internal_name = "7850AV", .model = "Maxtor 7850AV", .zones =  4,  .avg_spt = 120, .heads = 4, .rpm = 3551, .full_stroke_ms = 31, .track_seek_ms = 3.7,   .rcache_num_seg =  4, .rcache_seg_size =  64, .max_multiple =  8 },
    { .name = "[ATA-2] Maxtor 71336AP",                           .internal_name = "71336AP", .model = "Maxtor 71336AP", .zones =  4,  .avg_spt = 105, .heads = 4, .rpm = 4480, .full_stroke_ms = 12, .track_seek_ms = 3.4,   .rcache_num_seg =  8, .rcache_seg_size =  128, .max_multiple =  16 },
    { .name = "[ATA-2] Quantum Bigfoot 1.2AT",                    .internal_name = "BF12A011", .model = "QUANTUM BIGFOOT BF1.2A", .zones =  2,  .avg_spt = 155, .heads = 2, .rpm = 3600, .full_stroke_ms = 30, .track_seek_ms = 3.5,   .rcache_num_seg =  4, .rcache_seg_size =  128, .max_multiple =  16 },
    { .name = "[ATA-2] Quantum Bigfoot (CY4320A)",                .internal_name = "CY4320A", .model = "QUANTUM BIGFOOT_CY4320A", .zones =  2,  .avg_spt = 130, .heads = 2, .rpm = 4000, .full_stroke_ms = 29, .track_seek_ms = 2,   .rcache_num_seg =  8, .rcache_seg_size =  128, .max_multiple =  32 },
    { .name = "[ATA-2] Quantum Fireball CR4.3AT",                 .internal_name = "CR43A013", .model = "QUANTUM FIREBALL CR4.3A", .zones =  2,  .avg_spt = 110, .heads = 2, .rpm = 5400, .full_stroke_ms = 22, .track_seek_ms = 2.5,   .rcache_num_seg =  8, .rcache_seg_size =  512, .max_multiple =  32 },
    { .name = "[ATA-2] Samsung PLS-31274A",                       .internal_name = "PLS31274A", .model = "SAMSUNG PLS-31274A", .zones =  4,  .avg_spt = 110, .heads = 4, .rpm = 4500, .full_stroke_ms = 45, .track_seek_ms = 4.5,   .rcache_num_seg =  4, .rcache_seg_size =  256, .max_multiple =  8 },
    { .name = "[ATA-2] Samsung Winner-1",                         .internal_name = "WNR31601A", .model = "SAMSUNG WNR-31601A", .zones =  8,  .avg_spt = 110, .heads = 4, .rpm = 5400, .full_stroke_ms = 22, .track_seek_ms = 3,   .rcache_num_seg =  8, .rcache_seg_size =  128, .max_multiple =  16 },
    { .name = "[ATA-2] Seagate Medalist (ST3780A)",               .internal_name = "ST3780A", .model = "ST3780A", .zones =  8,  .avg_spt = 120, .heads = 4, .rpm = 4500, .full_stroke_ms = 25, .track_seek_ms = 3.5,   .rcache_num_seg =  4, .rcache_seg_size =  256, .max_multiple =  16 },
    { .name = "[ATA-2] Seagate Medalist (ST31220A)",              .internal_name = "ST31220A", .model = "ST31220A", .zones =  8,  .avg_spt = 140, .heads = 6, .rpm = 4500, .full_stroke_ms = 27, .track_seek_ms = 3.5,   .rcache_num_seg =  4, .rcache_seg_size =  256, .max_multiple =  16 },
    { .name = "[ATA-2] Seagate Medalist 210xe",                   .internal_name = "ST3250A", .model = "ST3250A", .zones =  4,  .avg_spt = 148, .heads = 2, .rpm = 3811, .full_stroke_ms = 30, .track_seek_ms = 4.1,   .rcache_num_seg =  8, .rcache_seg_size =  120, .max_multiple =  8 },
    { .name = "[ATA-2] Seagate Medalist 275xe",                   .internal_name = "ST3295A", .model = "ST3295A", .zones =  4,  .avg_spt = 130, .heads = 2, .rpm = 3811, .full_stroke_ms = 30, .track_seek_ms = 3.4,   .rcache_num_seg =  3, .rcache_seg_size =  120, .max_multiple =  8 },
    { .name = "[ATA-2] Seagate Medalist 1270SL",                  .internal_name = "ST51270A", .model = "ST51270A", .zones =  8,  .avg_spt = 105, .heads = 3, .rpm = 5736, .full_stroke_ms = 25, .track_seek_ms = 2,   .rcache_num_seg =  8, .rcache_seg_size =  128, .max_multiple =  16 },
    { .name = "[ATA-2] Western Digital Caviar 2850",              .internal_name = "AC2850", .model = "WDC WDAC2850-00F", .zones =  4,  .avg_spt = 115, .heads = 2, .rpm = 4500, .full_stroke_ms = 12, .track_seek_ms = 4,   .rcache_num_seg =  8, .rcache_seg_size =  128, .max_multiple =  8 },
    { .name = "[ATA-2] Western Digital Caviar 31200",             .internal_name = "WDAC31200", .model = "WDC WDAC31200-00F", .zones =  8,  .avg_spt = 110, .heads = 4, .rpm = 4500, .full_stroke_ms = 12, .track_seek_ms = 4,   .rcache_num_seg =  8, .rcache_seg_size =  64, .max_multiple =  16 },
    { .name = "[ATA-3] Samsung Winner 5X",                        .internal_name = "WU33205A", .model = "SAMSUNG WU33205A", .zones =  16,  .avg_spt = 100, .heads = 4, .rpm = 5400, .full_stroke_ms = 20, .track_seek_ms = 3,   .rcache_num_seg =  8, .rcache_seg_size =  128, .max_multiple =  16 },
    { .name = "[ATA-4] Fujitsu MPD3043AT",                        .internal_name = "MPD3043AT", .model = "FUJITSU MPD3043AT", .zones =  5,  .avg_spt = 95, .heads = 2, .rpm = 5400, .full_stroke_ms = 29, .track_seek_ms = 1.5,   .rcache_num_seg =  8, .rcache_seg_size =  512, .max_multiple =  16 },
    { .name = "[ATA-4] Fujitsu MPD3064AT",                        .internal_name = "MPD3064AT", .model = "FUJITSU MPD3064AT", .zones =  7,  .avg_spt = 95, .heads = 3, .rpm = 5400, .full_stroke_ms = 30, .track_seek_ms = 1.5,   .rcache_num_seg =  8, .rcache_seg_size =  512, .max_multiple =  16 },
    { .name = "[ATA-4] Maxtor DiamondMax 2160",                   .internal_name = "86480D6", .model = "Maxtor 86480D6", .zones =  8,  .avg_spt = 97, .heads = 4, .rpm = 5200, .full_stroke_ms = 18, .track_seek_ms = 1,   .rcache_num_seg =  8, .rcache_seg_size =  512, .max_multiple =  32 },
    { .name = "[ATA-4] Maxtor DiamondMax 2880",                   .internal_name = "90432D3", .model = "Maxtor 90432D3", .zones =  16,  .avg_spt = 90, .heads = 3, .rpm = 5400, .full_stroke_ms = 18, .track_seek_ms = 1,   .rcache_num_seg =  8, .rcache_seg_size =  256, .max_multiple =  32 },
    { .name = "[ATA-4] Quantum Bigfoot TX4.3AT",                  .internal_name = "TX043A011", .model = "QUANTUM BIGFOOT TX4.3A", .zones =  2,  .avg_spt = 120, .heads = 2, .rpm = 4000, .full_stroke_ms = 30, .track_seek_ms = 2.5,   .rcache_num_seg =  8, .rcache_seg_size =  128, .max_multiple =  32 },
    { .name = "[ATA-4] Toshiba MK4006MAV",                        .internal_name = "MK4006MAV", .model = "TOSHIBA MK4006MAV", .zones =  8,  .avg_spt = 130, .heads = 6, .rpm = 4200, .full_stroke_ms = 25, .track_seek_ms = 3,   .rcache_num_seg =  8, .rcache_seg_size =  512, .max_multiple =  32 },
    { .name = "[ATA-4] Western Digital Caviar 33200",             .internal_name = "AC33200", .model = "WDC AC33200-00LA", .zones =  16,  .avg_spt = 110, .heads = 5, .rpm = 5200, .full_stroke_ms = 40, .track_seek_ms = 3,   .rcache_num_seg =  8, .rcache_seg_size =  256, .max_multiple =  32 },
    { .name = "[ATA-5] Samsung SpinPoint V6800",                  .internal_name = "SV0682D", .model = "SAMSUNG SV0682D", .zones =  2,  .avg_spt = 95, .heads = 2, .rpm = 5400, .full_stroke_ms = 18, .track_seek_ms = 1.3,   .rcache_num_seg =  16, .rcache_seg_size =  512, .max_multiple =  32 },
    { .name = "[ATA-5] Western Digital Caviar 102AA",             .internal_name = "WD102AA", .model = "WDC WD102AA-00ANA0", .zones =  8,  .avg_spt = 95, .heads = 8, .rpm = 5400, .full_stroke_ms = 12, .track_seek_ms = 1.5,   .rcache_num_seg =  16, .rcache_seg_size =  512, .max_multiple =  32 },
   // clang-format on
};

int
hdd_preset_get_num(void)
{
    return sizeof(hdd_speed_presets) / sizeof(hdd_preset_t);
}

const char *
hdd_preset_getname(int preset)
{
    return hdd_speed_presets[preset].name;
}

const char *
hdd_preset_get_internal_name(int preset)
{
    return hdd_speed_presets[preset].internal_name;
}

int
hdd_preset_get_from_internal_name(char *s)
{
    int c = 0;

    for (int i = 0; i < (sizeof(hdd_speed_presets) / sizeof(hdd_preset_t)); i++) {
        if (!strcmp(hdd_speed_presets[c].internal_name, s))
            return c;
        c++;
    }

    return 0;
}

void
hdd_preset_apply(int hdd_id)
{
    hard_disk_t *hd = &hdd[hdd_id];
    double       revolution_usec;
    double       zone_percent;
    uint32_t     disk_sectors;
    uint32_t     sectors_per_surface;
    uint32_t     cylinders;
    uint32_t     cylinders_per_zone;
    uint32_t     total_sectors = 0;
    uint32_t     spt;
    uint32_t     zone_sectors;

    if (hd->speed_preset >= hdd_preset_get_num())
        hd->speed_preset = 0;

    const hdd_preset_t *preset = &hdd_speed_presets[hd->speed_preset];

    hd->cache.num_segments = preset->rcache_num_seg;
    hd->cache.segment_size = preset->rcache_seg_size;
    hd->max_multiple_block = preset->max_multiple;
    if (preset->model)
        hd->model = preset->model;

    if (!hd->speed_preset)
        return;

    hd->phy_heads = preset->heads;
    hd->rpm       = preset->rpm;

    revolution_usec           = 60.0 / (double) hd->rpm * 1000000.0;
    hd->avg_rotation_lat_usec = revolution_usec / 2;
    hd->full_stroke_usec      = preset->full_stroke_ms * 1000;
    hd->head_switch_usec      = preset->track_seek_ms * 1000;
    hd->cyl_switch_usec       = preset->track_seek_ms * 1000;

    hd->cache.write_size = 64;

    hd->num_zones = preset->zones;

    disk_sectors        = hd->tracks * hd->hpc * hd->spt;
    sectors_per_surface = (uint32_t) ceil((double) disk_sectors / (double) hd->phy_heads);
    cylinders           = (uint32_t) ceil((double) sectors_per_surface / (double) preset->avg_spt);
    hd->phy_cyl         = cylinders;
    cylinders_per_zone  = cylinders / preset->zones;

    for (uint32_t i = 0; i < preset->zones; i++) {
        zone_percent = i * 100 / (double) preset->zones;

        if (i < preset->zones - 1) {
            /* Function for realistic zone sector density */
            double spt_percent = -0.00341684 * pow(zone_percent, 2) - 0.175811 * zone_percent + 118.48;
            spt                = (uint32_t) ceil((double) preset->avg_spt * spt_percent / 100);
        } else
            spt = (uint32_t) ceil((double) (disk_sectors - total_sectors) / (double) (cylinders_per_zone * preset->heads));

        zone_sectors = spt * cylinders_per_zone * preset->heads;
        total_sectors += zone_sectors;

        hd->zones[i].cylinders         = cylinders_per_zone;
        hd->zones[i].sectors_per_track = spt;
    }

    hdd_zones_init(hd);
    hdd_cache_init(hd);
}
