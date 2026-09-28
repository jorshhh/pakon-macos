/*
 * pakon_proto.c — packet framing layer (STUBS for Phase 0).
 *
 * Only the pure name-lookup helpers are real here. Checksum, build, and parse
 * are intentionally unimplemented: the checksum algorithm must first be
 * derived and validated against real sample packets in Phase 3 before any of
 * these can be trusted.
 */
#include "pakon_proto.h"

const char *pakon_status_str(pakon_status s)
{
    switch (s) {
    case PS_SUCCESS:      return "success";
    case PS_NOT_ACKED:    return "not acked";
    case PS_INVALID_PKT:  return "invalid packet";
    case PS_BAD_CHECKSUM: return "bad checksum";
    case PS_USB_4:        return "usb error (4)";
    case PS_USB_5:        return "usb error (5)";
    case PS_USB_6:        return "usb error (6)";
    case PS_HOST_ALGO:    return "host algorithm error";
    case PS_SUCCESS_8:    return "success (8)";
    case PS_BUS_ERROR:    return "bus error";
    case PS_NONE:         return "no status (reply too short)";
    }
    return "unknown status";
}

const char *pakon_addr_str(pakon_addr a)
{
    switch (a) {
    case AD_HOST:           return "AD_HOST";
    case AD_PICL:           return "AD_PICL";
    case AD_BOOT_PICL:      return "AD_BOOT_PICL";
    case AD_PICM:           return "AD_PICM";
    case AD_BOOT_PICM:      return "AD_BOOT_PICM";
    case AD_PICL_PLUS:      return "AD_PICL_PLUS";
    case AD_BOOT_PICL_PLUS: return "AD_BOOT_PICL_PLUS";
    case AD_PICM_PLUS:      return "AD_PICM_PLUS";
    case AD_BOOT_PICM_PLUS: return "AD_BOOT_PICM_PLUS";
    }
    return "AD_?";
}

#include <string.h>

pakon_result pakon_packet_build(pakon_packet *pkt, uint8_t type,
                                const uint8_t *data, size_t dlen)
{
    if (!pkt || (dlen && !data) || dlen > PAKON_DATA_MAX)
        return PAKON_ERR_PARAM;
    memset(pkt, 0, sizeof(*pkt));
    pkt->type = type;
    pkt->count = (uint8_t)dlen;
    if (dlen)
        memcpy(pkt->data, data, dlen);
    return PAKON_OK;
}

pakon_result pakon_packet_serialize(const pakon_packet *pkt,
                                    uint8_t *out, size_t out_cap,
                                    size_t *out_len)
{
    if (!pkt || !out)
        return PAKON_ERR_PARAM;
    if (pkt->count > PAKON_DATA_MAX)
        return PAKON_ERR_PROTO;
    size_t wlen = pakon_wire_len(pkt);
    if (out_cap < wlen)
        return PAKON_ERR_PARAM;
    out[0] = pkt->type;
    out[1] = pkt->count;
    memcpy(out + 2, pkt->data, pkt->count);
    if (out_len)
        *out_len = wlen;
    return PAKON_OK;
}

pakon_result pakon_packet_parse(pakon_packet *pkt, const uint8_t *raw,
                                size_t rawlen)
{
    if (!pkt || !raw)
        return PAKON_ERR_PARAM;
    if (rawlen < 2)
        return PAKON_ERR_PROTO;
    uint8_t count = raw[1];
    if (count > PAKON_DATA_MAX || rawlen != (size_t)2 + count)
        return PAKON_ERR_PROTO;   /* not a self-consistent frame */
    memset(pkt, 0, sizeof(*pkt));
    pkt->type = raw[0];
    pkt->count = count;
    memcpy(pkt->data, raw + 2, count);
    return PAKON_OK;
}
