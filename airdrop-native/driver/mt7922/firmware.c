/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Adapted from mt76_connac_mcu.c and mt7925/mcu.c (MediaTek, 2020/2023).
 * Adapted for MT7922 from the pinned Windows MT7925 firmware core.
 * Bounds validation and callback-based sequencing are Windows port additions.
 * No hardware operations occur without caller-supplied transport callbacks.
 */
#include "firmware.h"
static int span32(uint64_t address,uint32_t size) {
    return size && address<=0xffffffffULL && (uint64_t)(size-1)<=0xffffffffULL-address;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static int span(uint32_t offset, uint32_t length, uint32_t total) {
    return offset <= total && length <= total - offset;
}
int mt2_fw_validate(const uint8_t *data, uint32_t size, uint32_t kind, mt2_fw_plan *out) {
    uint32_t count, table, minimum, i, j, offset = 0, entry = 0;
    const uint8_t *p;
    mt2_fw_region r;
    if (!out) return MT2_INVALID;
    out->count = 0; out->kind = 0; out->entry = 0;
    if (!data || size > MT2_FW_MAX_SIZE) return MT2_INVALID;
    if (kind == MT2_FW_PATCH) {
        if (size < 96) return MT2_INVALID;
        count = be32(data + 44); table = 96;
        if (!count || count > MT2_FW_MAX_REGIONS || count > (size - 96) / 64) return MT2_INVALID;
        minimum = 96 + count * 64;
    } else if (kind == MT2_FW_RAM) {
        if (size < 36) return MT2_INVALID;
        count = data[size - 34];
        if (!count || count > MT2_FW_MAX_REGIONS || count > (size - 36) / 40) return MT2_INVALID;
        table = size - 36 - count * 40; minimum = 0;
    } else return MT2_INVALID;
    for (i=0; i<count; ++i) {
        r.skip = 0; r.override_address = 0; r.mode = 0x80000000u;
        if (kind == MT2_FW_PATCH) {
            uint32_t encryption, section_size, key;
            p = data + table + i * 64;
            if ((be32(p) & 0xffffu) != 2) return MT2_INVALID;
            r.offset = be32(p+4); section_size = be32(p+8);
            r.address = be32(p+12); r.length = be32(p+16); key = be32(p+20);
            if (r.offset < minimum || !span(r.offset, section_size, size) || r.length > section_size)
                return MT2_INVALID;
            encryption = key == 0xffffffffu ? 0 : key >> 24;
            if (encryption == 1) r.mode |= 9u | ((key & 3u) << 1);
            else if (encryption == 2) r.mode |= 0x49u;
            else if (encryption) return MT2_INVALID;
        } else {
            uint8_t feature;
            p = data + table + i * 40;
            r.offset = offset; r.address = le32(p+16); r.length = le32(p+20); feature = p[24];
            if (!span(offset, r.length, table)) return MT2_INVALID;
            r.skip = !!(feature & 0x40); r.override_address = !!(feature & 0x20);
            r.mode |= feature & 6u;
            if (feature & 1u) r.mode |= 9u;
            if (feature & 0x10u) r.mode |= 0x40u;
            offset += r.length;
        }
        if (!r.length || !span(r.offset, r.length, size) ||
            (!r.skip && !span32(r.address, r.length))) return MT2_INVALID;
        for (j=0; j<i; ++j) {
            const mt2_fw_region *old = &out->regions[j];
            if ((uint64_t)r.offset < (uint64_t)old->offset + old->length &&
                (uint64_t)old->offset < (uint64_t)r.offset + r.length) return MT2_INVALID;
        }
        out->regions[i] = r;
        if (!r.skip && r.override_address) entry = r.address;
    }
    out->kind = kind; out->count = count; out->entry = entry;
    return MT2_OK;
}

static int command(const mt2_fw_transport *t, uint32_t cmd, const uint8_t *data,
                    uint32_t length, int wait, uint32_t *reply) {
    *reply = 0xffffffffu;
    return t->send(t->context, cmd, data, length, wait, reply);
}
static int download(const uint8_t *data, const mt2_fw_plan *plan,
                     const mt2_fw_transport *t, mt2_fw_result *result) {
    uint32_t i, position, chunk, reply, cmd;
    uint8_t request[12];
    int error;
    for (i=0; i<plan->count; ++i) {
        const mt2_fw_region *r = &plan->regions[i];
        if (r->skip) continue;
        put32(request, r->address); put32(request+4, r->length); put32(request+8, r->mode);
        cmd = (r->address == 0x200000 || r->address == 0x900000) ? 5u : 1u;
        error = command(t, cmd, request, 12, 1, &reply);
        if (error) return error;
        if (reply) return MT2_FW_PROTOCOL;
        for (position=0; position<r->length; position+=chunk) {
            chunk = r->length-position;
            if (chunk > MT2_FW_CHUNK) chunk = MT2_FW_CHUNK;
            error = command(t, MT2_FW_SCATTER, data+r->offset+position, chunk, 0, &reply);
            if (error) return error;
            result->bytes_sent += chunk;
        }
        error = t->drain(t->context);
        if (error) return error;
        ++result->regions_sent;
    }
    return MT2_OK;
}

int mt2_fw_boot(const uint8_t *patch, uint32_t patch_size, const uint8_t *ram,
               uint32_t ram_size, const mt2_fw_transport *t, mt2_fw_result *result) {
    mt2_fw_plan pp, rp;
    uint32_t reply;
    uint8_t request[8];
    int error, release;
    if (!result) return MT2_INVALID;
    result->patch_present=0; result->regions_sent=0; result->bytes_sent=0;
    result->start_acked=0; result->ready=0; result->primary_error=0; result->release_error=0;
    if (!t || !t->send || !t->drain || !t->wait_state) { error=MT2_INVALID; goto done; }
    /* Preflight BOTH images before any hardware callback or firmware restart. */
    error = mt2_fw_validate(patch, patch_size, MT2_FW_PATCH, &pp);
    if (error) goto done;
    error = mt2_fw_validate(ram, ram_size, MT2_FW_RAM, &rp);
    if (error) goto done;
    put32(request, 1); put32(request+4, 0);
    error = command(t, 4, request, 4, 0, &reply);
    if (error) goto done;
    error = t->wait_state(t->context, 0);
    if (error) goto done;
    error = command(t, 0x10, request, 4, 1, &reply);
    if (error) goto done;
    if (reply == 1) result->patch_present = 1;
    else if (reply == 2) {
        error = download(patch, &pp, t, result);
        if (!error) {
            request[0] = 0;
            error = command(t, 7, request, 4, 1, &reply);
            if (!error && reply) error = MT2_FW_PROTOCOL;
        }
        /* A successfully acquired semaphore is released on every exit path. */
        request[0] = 0;
        release = command(t, 0x10, request, 4, 1, &reply);
        if (!release && reply != 3) release = MT2_FW_PROTOCOL;
        result->release_error = release;
        if (error) goto done;
        if (release) { error=release; goto done; }
    } else { error=MT2_FW_PROTOCOL; goto done; }
    error = download(ram, &rp, t, result);
    if (error) goto done;
    put32(request, rp.entry ? 1u : 0u); put32(request+4, rp.entry);
    error = command(t, 2, request, 8, 1, &reply);
    if (error) goto done;
    if (reply) { error=MT2_FW_PROTOCOL; goto done; }
    result->start_acked = 1;
    error = t->wait_state(t->context, 1);
    if (!error) result->ready = 1;
done:
    result->primary_error = error;
    return error;
}

