#include "pakon_cmd.h"

#include <string.h>

pakon_result pakon_cmd(pakon_dev *dev, const pakon_packet *cmd,
                       pakon_packet *reply, unsigned timeout_ms)
{
    if (!dev || !cmd)
        return PAKON_ERR_PARAM;

    uint8_t wire[PAKON_PACKET_SIZE];
    size_t wlen = 0;
    pakon_result r = pakon_packet_serialize(cmd, wire, sizeof(wire), &wlen);
    if (r != PAKON_OK)
        return r;

    size_t sent = 0;
    r = pakon_usb_send(dev, PAKON_EP_CMD_OUT, wire, wlen, &sent, timeout_ms);
    if (r != PAKON_OK)
        return r;
    if (sent != wlen)
        return PAKON_ERR_USB;

    if (!reply)
        return PAKON_OK;

    /* Replies are short frames; a max-struct-sized buffer is plenty. */
    uint8_t buf[PAKON_PACKET_SIZE];
    size_t got = 0;
    r = pakon_usb_recv(dev, PAKON_EP_CMD_IN, buf, sizeof(buf), &got, timeout_ms);
    if (r != PAKON_OK)
        return r;

    return pakon_packet_parse(reply, buf, got);
}

pakon_result pakon_cmd_raw(pakon_dev *dev, uint8_t type,
                           const uint8_t *data, size_t dlen,
                           pakon_packet *reply, unsigned timeout_ms)
{
    pakon_packet cmd;
    pakon_result r = pakon_packet_build(&cmd, type, data, dlen);
    if (r != PAKON_OK)
        return r;
    return pakon_cmd(dev, &cmd, reply, timeout_ms);
}

void pakon_bridge_open(pakon_dev *dev, unsigned timeout_ms)
{
    /* HostReset: 04 03 10 00 85 */
    const uint8_t host_reset[] = {AD_HOST, 0x00, 0x85};
    /* HostSetMode: 02 04 10 01 8f 00 */
    const uint8_t host_set_mode[] = {AD_HOST, 0x01, 0x8f, 0x00};
    pakon_packet reply;

    /* Reply timeouts are expected on every open after the first in a power
     * session (see header); results are deliberately ignored. */
    (void)pakon_cmd_raw(dev, PH_CMD, host_reset, sizeof(host_reset),
                        &reply, timeout_ms);
    (void)pakon_cmd_raw(dev, PH_WRITE, host_set_mode, sizeof(host_set_mode),
                        &reply, timeout_ms);
}

pakon_pic_state pakon_probe_pic(pakon_dev *dev, uint8_t pic_address,
                                uint8_t *status_out, unsigned timeout_ms)
{
    const uint8_t probe[] = {pic_address, 0x00, 0x00};
    pakon_packet reply;

    if (status_out)
        *status_out = PS_NONE;
    if (pakon_cmd_raw(dev, PH_CMD, probe, sizeof(probe), &reply,
                      timeout_ms) != PAKON_OK)
        return PAKON_PIC_ERROR;
    if (reply.type != PH_RESPONSE || reply.count < 2 ||
        pakon_packet_addr(&reply) != pic_address)
        return PAKON_PIC_ERROR;

    uint8_t status = pakon_packet_status(&reply);
    if (status_out)
        *status_out = status;
    if (status == PS_SUCCESS)
        return PAKON_PIC_PRESENT;
    if (status == PS_NOT_ACKED)
        return PAKON_PIC_ABSENT;
    return PAKON_PIC_ERROR;
}
