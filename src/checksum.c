/*
 * checksum.c — CRC32, Adler-32, FNV, workbook digest
 *
 * Copyright (c) 2024-2025 Sheetcalc Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "../include/sc_internal.h"

#include <string.h>

uint32_t sc_hash_combine(uint32_t h1, uint32_t h2);

/* IEEE 802.3 reflected CRC32 */
static const uint32_t sc_crc32_tab[256] = {
    0x00000000u,0x77073096u,0xee0e612cu,0x990951bau,0x076dc419u,0x706af48fu,0xe963a535u,0x9e6495a3u,
    0x0edb8832u,0x79dcb8a4u,0xe0d5e91eu,0x97d2d988u,0x09b64c2bu,0x7eb17cbdu,0xe7b82d07u,0x90bf1d91u,
    0x1db71064u,0x6ab020f2u,0xf3b97148u,0x84be41deu,0x1adad47du,0x6ddde4ebu,0xf4d4b551u,0x83d385c7u,
    0x136c9856u,0x646ba8c0u,0xfd62f97au,0x8a65c9ecu,0x14015c4fu,0x63066cd9u,0xfa0f3d63u,0x8d080df5u,
    0x3b6e20c8u,0x4c69105eu,0xd56041e4u,0xa2677172u,0x3c03e4d1u,0x4b04d447u,0xd20d85fdu,0xa50ab56bu,
    0x35b5a8fau,0x42b2986cu,0xdbbbc9d6u,0xacbcf940u,0x32d86ce3u,0x45df5c75u,0xdcd60dcfu,0xabd13d59u,
    0x26d930acu,0x51de003au,0xc8d75180u,0xbfd06116u,0x21b4f4b5u,0x56b3c423u,0xcfba9599u,0xb8bda50fu,
    0x2802b89eu,0x5f058808u,0xc60cd9b2u,0xb10be924u,0x2f6f7c87u,0x58684c11u,0xc1611dabu,0xb6662d3du,
    0x76dc4190u,0x01db7106u,0x98d220bcu,0xefd5102au,0x71b18589u,0x06b6b51fu,0x9fbfe4a5u,0xe8b8d433u,
    0x7807c9a2u,0x0f00f934u,0x9609a88eu,0xe10e9818u,0x7f6a0dbbu,0x086d3d2du,0x91646c97u,0xe6635c01u,
    0x6b6b51f4u,0x1c6c6162u,0x856530d8u,0xf262004eu,0x6c0695edu,0x1b01a57bu,0x8208f4c1u,0xf50fc457u,
    0x65b0d9c6u,0x12b7e950u,0x8bbeb8eau,0xfcb9887cu,0x62dd1ddfu,0x15da2d49u,0x8cd37cf3u,0xfbd44c65u,
    0x4db26158u,0x3ab551ceu,0xa3bc0074u,0xd4bb30e2u,0x4adfa541u,0x3dd895d7u,0xa4d1c46du,0xd3d6f4fbu,
    0x4369e96au,0x346ed9fcu,0xad678846u,0xda60b8d0u,0x44042d73u,0x33031de5u,0xaa0a4c5fu,0xdd0d7cc9u,
    0x5005713cu,0x270241aau,0xbe0b1010u,0xc90c2086u,0x5768b525u,0x206f85b3u,0xb966d409u,0xce61e49fu,
    0x5edef90eu,0x29d9c998u,0xb0d09822u,0xc7d7a8b4u,0x59b33d17u,0x2eb40d81u,0xb7bd5c3bu,0xc0ba6cadu,
    0xedb88320u,0x9abfb3b6u,0x03b6e20cu,0x74b1d29au,0xead54739u,0x9dd277afu,0x04db2615u,0x73dc1683u,
    0xe3630b12u,0x94643b84u,0x0d6d6a3eu,0x7a6a5aa8u,0xe40ecf0bu,0x9309ff9du,0x0a00ae27u,0x7d079eb1u,
    0xf00f9344u,0x8708a3d2u,0x1e01f268u,0x6906c2feu,0xf762575du,0x806567cbu,0x196c3671u,0x6e6b06e7u,
    0xfed41b76u,0x89d32be0u,0x10da7a5au,0x67dd4accu,0xf9b9df6fu,0x8ebeeff9u,0x17b7be43u,0x60b08ed5u,
    0xd6d6a3e8u,0xa1d1937eu,0x38d8c2c4u,0x4fdff252u,0xd1bb67f1u,0xa6bc5767u,0x3fb506ddu,0x48b2364bu,
    0xd80d2bdau,0xaf0a1b4cu,0x36034af6u,0x41047a60u,0xdf60efc3u,0xa867df55u,0x316e8eefu,0x4669be79u,
    0xcb61b38cu,0xbc66831au,0x256fd2a0u,0x5268e236u,0xcc0c7795u,0xbb0b4703u,0x220216b9u,0x5505262fu,
    0xc5ba3bbeu,0xb2bd0b28u,0x2bb45a92u,0x5cb36a04u,0xc2d7ffa7u,0xb5d0cf31u,0x2cd99e8bu,0x5bdeae1du,
    0x9b64c2b0u,0xec63f226u,0x756aa39cu,0x026d930au,0x9c0906a9u,0xeb0e363fu,0x72076785u,0x05005713u,
    0x95bf4a82u,0xe2b87a14u,0x7bb12baeu,0x0cb61b38u,0x92d28e9bu,0xe5d5be0du,0x7cdcefb7u,0x0bdbdf21u,
    0x86d3d2d4u,0xf1d4e242u,0x68ddb3f8u,0x1fda836eu,0x81be16cdu,0xf6b9265bu,0x6fb077e1u,0x18b74777u,
    0x88085ae6u,0xff0f6a70u,0x66063bcau,0x11010b5cu,0x8f659effu,0xf862ae69u,0x616bffd3u,0x166ccf45u,
    0xa00ae278u,0xd70dd2eeu,0x4ec04954u,0x3903b3c2u,0xa7672661u,0xd06016f7u,0x4969474du,0x3e6e77dbu,
    0xaed16a4au,0xd9d65adcu,0x40df0b66u,0x37d83bf0u,0xa9bcae53u,0xdebb9ec5u,0x47b2cf7fu,0x30b5ffe9u,
    0xbdbdf21cu,0xcabac28au,0x53b39330u,0x24b4a3a6u,0xbad03605u,0xcdd70693u,0x54de5729u,0x23d967bfu,
    0xb3667a2eu,0xc4614ab8u,0x5d681b02u,0x2a6f2b94u,0xb40bbe37u,0xc30c8ea1u,0x5a05df1bu,0x2d02ef8du
};

uint32_t cs_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    size_t i;

    for (i = 0; i < len; i++)
        crc = sc_crc32_tab[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

uint32_t cs_adler32(const uint8_t *data, size_t len)
{
    uint32_t a = 1u, b = 0u;
    size_t i;

    for (i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

uint32_t cs_fnv32(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static void digest_u32(uint32_t v, uint32_t *h1, uint32_t *h2)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    *h1 = sc_hash_combine(*h1, cs_fnv32(b, 4));
    *h2 = cs_crc32(b, 4) ^ (*h2 >> 3);
}

static void digest_bytes(const void *p, size_t n, uint32_t *h1, uint32_t *h2)
{
    const uint8_t *b = (const uint8_t *)p;
    if (!b || n == 0)
        return;
    *h1 = sc_hash_combine(*h1, cs_fnv32(b, n));
    *h2 ^= cs_crc32(b, n);
}

static void digest_val(const ScVal *v, uint32_t *h1, uint32_t *h2)
{
    size_t i;
    double num;

    if (!v)
        return;
    digest_u32((uint32_t)v->type, h1, h2);
    switch (v->type) {
    case SC_VAL_NUM:
        num = v->as.num;
        digest_bytes(&num, sizeof(num), h1, h2);
        break;
    case SC_VAL_STR:
        digest_u32((uint32_t)v->as.str.len, h1, h2);
        digest_bytes(v->as.str.s, v->as.str.len, h1, h2);
        break;
    case SC_VAL_BOOL:
        digest_u32(v->as.boolean ? 1u : 0u, h1, h2);
        break;
    case SC_VAL_ERR:
        digest_u32((uint32_t)v->as.err, h1, h2);
        break;
    case SC_VAL_LIST:
        digest_u32((uint32_t)v->as.list.count, h1, h2);
        for (i = 0; i < v->as.list.count; i++)
            digest_val(&v->as.list.items[i], h1, h2);
        break;
    default:
        break;
    }
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int meta_find_integrity(const ScWorkbook *wb, uint32_t stored[2])
{
    const uint8_t *d;
    uint32_t sec_cnt, i;

    if (!wb || !wb->raw || wb->raw_len < 16)
        return 0;
    d = wb->raw;
    if (d[0] != 'C' || d[1] != 'S' || d[2] != 'B' || d[3] != '\0')
        return 0;
    sec_cnt = rd_le32(d + 12);
    for (i = 0; i < sec_cnt; i++) {
        size_t ent = 16 + (size_t)i * 12u, mp, j;
        uint32_t off, sz, kv_cnt;
        if (rd_le32(d + ent) != 7u)
            continue;
        off = rd_le32(d + ent + 4);
        sz = rd_le32(d + ent + 8);
        if ((uint64_t)off + sz > wb->raw_len)
            return 0;
        mp = (size_t)off;
        kv_cnt = rd_le32(d + mp);
        mp += 4;
        for (j = 0; j < kv_cnt; j++) {
            uint16_t klen = (uint16_t)d[mp] | ((uint16_t)d[mp + 1] << 8);
            uint32_t vlen;
            mp += 2;
            if (mp + klen + 4 > wb->raw_len)
                return 0;
            vlen = rd_le32(d + mp + klen);
            if (klen == 9 && memcmp(d + mp, "integrity", 9) == 0 && vlen >= 8) {
                mp += klen + 4;
                stored[0] = rd_le32(d + mp);
                stored[1] = rd_le32(d + mp + 4);
                return 1;
            }
            mp += klen + 4 + vlen;
        }
        return 0;
    }
    return 0;
}

int sc_digest_workbook(const ScWorkbook *wb, uint32_t *out)
{
    uint32_t h1 = 2166136261u, h2 = 0x811c9dc5u;
    int si, r, c;

    if (!wb || !out)
        return SC_ERR_INTERNAL;

    digest_u32(wb->version, &h1, &h2);
    digest_u32((uint32_t)wb->nsheets, &h1, &h2);

    for (si = 0; si < wb->nsheets; si++) {
        const ScSheet *sh = &wb->sheets[si];
        digest_u32((uint32_t)sh->nrows, &h1, &h2);
        digest_u32((uint32_t)sh->ncols, &h1, &h2);
        digest_u32(sh->flags, &h1, &h2);
        if (sh->name)
            digest_bytes(sh->name, sh->name_len ? sh->name_len : strlen(sh->name), &h1, &h2);
        if (!sh->cells)
            continue;
        for (r = 0; r < sh->nrows; r++) {
            for (c = 0; c < sh->ncols; c++) {
                const ScCell *cl = &sh->cells[SC_CELL_IDX(sh, r, c)];
                digest_u32((uint32_t)r, &h1, &h2);
                digest_u32((uint32_t)c, &h1, &h2);
                digest_u32(cl->style_id, &h1, &h2);
                digest_val(&cl->value, &h1, &h2);
                if (cl->formula && cl->formula->text) {
                    digest_u32((uint32_t)cl->formula->text_len, &h1, &h2);
                    digest_bytes(cl->formula->text, cl->formula->text_len, &h1, &h2);
                }
            }
        }
    }

    out[0] = h1;
    out[1] = h2;
    return SC_OK;
}

int cs_verify_integrity(const ScWorkbook *wb)
{
    uint32_t computed[2], stored[2];

    if (!wb)
        return SC_ERR_INTERNAL;
    if (sc_digest_workbook(wb, computed) != SC_OK)
        return SC_ERR_INTERNAL;
    if (!meta_find_integrity(wb, stored))
        return SC_OK; /* no stored digest — nothing to verify */

    if (computed[0] != stored[0] || computed[1] != stored[1])
        return SC_ERR_CORRUPT;
    return SC_OK;
}
