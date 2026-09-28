/*
 * test_proto — hardware-free unit tests for the framing layer.
 *
 * Phase 0 deliverable: assert the wire struct is exactly 36 bytes and that its
 * fields land at the documented offsets. Checksum round-trip tests are added
 * in Phase 3 once pakon_checksum is real (they are sketched here as a skipped
 * placeholder so the harness is ready).
 */
#include "pakon_proto.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
        if (cond) {                                             \
            printf("ok   - %s\n", (msg));                       \
        } else {                                                \
            printf("FAIL - %s\n", (msg));                       \
            failures++;                                         \
        }                                                       \
    } while (0)

int main(void)
{
    /* The core Phase 0 assertion. */
    CHECK(sizeof(pakon_packet) == 36, "pakon_packet is 36 bytes");
    CHECK(PAKON_PACKET_SIZE == 36, "PAKON_PACKET_SIZE == 36");
    CHECK(PAKON_DATA_MAX == 34, "PAKON_DATA_MAX == 34");

    /* Field offsets match the documented frame layout. */
    CHECK(offsetof(pakon_packet, type) == 0, "type at byte 0");
    CHECK(offsetof(pakon_packet, count) == 1, "count at byte 1");
    CHECK(offsetof(pakon_packet, data) == 2, "data starts at byte 2");

    /* Documented enum values are wired up correctly. */
    CHECK(AD_HOST == 0x10, "AD_HOST == 0x10");
    CHECK(AD_BOOT_PICM_PLUS == 0x46, "AD_BOOT_PICM_PLUS == 0x46");
    CHECK(PS_SUCCESS == 0, "PS_SUCCESS == 0");
    CHECK(PS_BUS_ERROR == 9, "PS_BUS_ERROR == 9");

    /* Framing: wire length is 2 + count (confirmed from capture), not 36. */
    {
        /* the real open packet: 04 03 10 00 85 */
        const uint8_t open_data[] = {0x10, 0x00, 0x85};
        pakon_packet pkt;
        CHECK(pakon_packet_build(&pkt, 0x04, open_data, 3) == PAKON_OK,
              "build open frame");
        CHECK(pkt.type == 0x04 && pkt.count == 3, "open type/count set");
        CHECK(pakon_wire_len(&pkt) == 5, "open wire length == 5");
        CHECK(pakon_packet_addr(&pkt) == AD_HOST, "open addr == AD_HOST");

        uint8_t wire[36];
        size_t wlen = 0;
        CHECK(pakon_packet_serialize(&pkt, wire, sizeof(wire), &wlen) == PAKON_OK
              && wlen == 5, "serialize open frame");
        const uint8_t expect[] = {0x04, 0x03, 0x10, 0x00, 0x85};
        CHECK(wlen == 5 && memcmp(wire, expect, 5) == 0,
              "serialized bytes == 04 03 10 00 85");
    }
    {
        /* parse the open reply 07 02 10 00 and read its status byte */
        const uint8_t reply[] = {0x07, 0x02, 0x10, 0x00};
        pakon_packet pkt;
        CHECK(pakon_packet_parse(&pkt, reply, 4) == PAKON_OK,
              "parse reply frame");
        CHECK(pkt.type == 0x07 && pkt.count == 2, "reply type/count");
        CHECK(pakon_packet_status(&pkt) == PS_SUCCESS, "reply status success");
    }
    {
        /* self-consistency: reject a frame whose length != 2 + count */
        const uint8_t bad[] = {0x8e, 0x01, 0x00, 0x00, 0x66};  /* a 0xA9 read */
        pakon_packet pkt;
        CHECK(pakon_packet_parse(&pkt, bad, 5) == PAKON_ERR_PROTO,
              "reject inconsistent frame (len != 2+count)");
    }
    {
        /* a reply too short to carry a status byte must NOT read as success
         * (pollers break on PS_SUCCESS; a truncated reply is not "ready") */
        const uint8_t short_reply[] = {0x07, 0x01, 0x10};  /* count 1: addr only */
        pakon_packet pkt;
        CHECK(pakon_packet_parse(&pkt, short_reply, 3) == PAKON_OK,
              "parse short (addr-only) reply");
        CHECK(pakon_packet_status(&pkt) == PS_NONE,
              "short reply status is PS_NONE, not PS_SUCCESS");
    }
    {
        /* confirmed wire type values (docs/LIBPAKON_COMPARISON.md §1 +
         * our own captures) */
        CHECK(PH_READ == 1 && PH_WRITE == 2 && PH_READ_STATUS == 3 &&
              PH_CMD == 4 && PH_RESPONSE == 7, "confirmed PH_ type values");
    }

    if (failures) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall tests passed\n");
    return 0;
}
