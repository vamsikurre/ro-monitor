"""Compile the RS485 frame maths from both ends on the host and assert it agrees.

A master and a slave that disagree about the CRC talk past each other with no
symptom except silence, so this lifts the real code out of both sketches --
node crc16() from ro_node.ino, master crc16() and levelPercent() from
esp32_hub_test.ino, and the SAME TWO from the production hub firmware
(app_rs485.c, app_cal.c) -- builds them with gcc and checks:

  * both CRCs match the CRC-16/Modbus known answer for "123456789" (0x4B37)
  * both agree byte-for-byte over a spread of frames
  * a built request frame verifies, and one flipped bit fails it
  * the hub's tank level maths holds at its boundaries, including uncalibrated
    and inverted calibration (the nodes no longer compute this at all)
  * the production firmware agrees with the bench sketch on both, over a sweep.
    Three copies of this maths now exist, and a copy no checker knows about is
    exactly how IN_ALARM sat in three documents and no firmware for a day.

    python docs/check_frame.py          # from the repo root
"""
import io
import os
import re
import subprocess
import sys
import tempfile

NODE = 'firmware/ro_node/ro_node.ino'
HUB = 'firmware/esp32_hub_test/esp32_hub_test.ino'
PROD_RS485 = 'firmware/hub_prod/main/app_rs485.c'
PROD_CAL = 'firmware/hub_prod/main/app_cal.c'

HARNESS = """
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- lifted from ro_node.ino --- */
%s

/* --- lifted from esp32_hub_test.ino (crc renamed to keep both) --- */
%s

/* --- lifted from the production firmware (renamed again to keep all three) --- */
%s

static int failures = 0;

static void fail(const char *what) { printf("%%s\\n", what); failures++; }

/* Build a request the way the hub does: AA 55 ADDR CMD LEN payload CRC_L CRC_H */
static uint8_t build(uint8_t *f, uint8_t addr, uint8_t cmd, const uint8_t *p, uint8_t len) {
    f[0] = 0xAA; f[1] = 0x55; f[2] = addr; f[3] = cmd; f[4] = len;
    memcpy(&f[5], p, len);
    uint16_t c = crc16(f, (uint8_t)(5 + len));
    f[5 + len] = (uint8_t)(c & 0xFF);
    f[6 + len] = (uint8_t)(c >> 8);
    return (uint8_t)(7 + len);
}

/* Verify the way the node does */
static int verify(const uint8_t *f, uint8_t total) {
    uint8_t len = f[4];
    if (total != (uint8_t)(7 + len)) return 0;
    uint16_t want = (uint16_t)f[5 + len] | ((uint16_t)f[6 + len] << 8);
    return crc16(f, (uint8_t)(5 + len)) == want;
}

int main(void) {
    const uint8_t known[] = "123456789";
    if (crc16(known, 9) != 0x4B37)     fail("node crc16 fails the Modbus known answer");
    if (hub_crc16(known, 9) != 0x4B37) fail("hub crc16 fails the Modbus known answer");

    /* the two implementations must not diverge on any frame we might send */
    uint8_t buf[40];
    for (int seed = 0; seed < 512; seed++) {
        uint8_t n = (uint8_t)(1 + seed %% 39);
        for (uint8_t i = 0; i < n; i++) buf[i] = (uint8_t)(seed * 31 + i * 17);
        if (crc16(buf, n) != hub_crc16(buf, n)) { fail("node and hub crc16 disagree"); break; }
    }

    /* a real poll: CMD_READ_LEVEL to 0x02, no payload */
    uint8_t frame[40];
    uint8_t total = build(frame, 0x02, 0x02, NULL, 0);
    if (total != 7)        fail("empty-payload request should be 7 bytes");
    if (!verify(frame, 7)) fail("freshly built frame does not verify");

    /* every single-bit flip in it must be caught */
    for (uint8_t byte = 0; byte < total; byte++) {
        for (uint8_t bit = 0; bit < 8; bit++) {
            frame[byte] ^= (uint8_t)(1 << bit);
            int ok = verify(frame, total);
            frame[byte] ^= (uint8_t)(1 << bit);
            if (ok && byte != 4) { fail("a flipped bit still verified"); byte = total; break; }
        }
    }

    /* a 10-byte CMD_READ_LEVEL reply round-trips */
    uint8_t payload[10] = { 0x01, 0x2C, 0x01, 0x30, 55, 100, 0, 0, 0x00, 0x2A };
    total = build(frame, 0x02, 0x82, payload, 10);
    if (total != 17)         fail("10-byte reply should be 17 bytes on the wire");
    if (!verify(frame, 17))  fail("10-byte reply does not verify");
    if (frame[4] != 10)      fail("LEN field wrong");

    /* the hub's level maths, per tank, with calibration passed in */
    if (levelPercent(0, 300, 1500) != 255)     fail("no echo should be invalid, not a level");
    if (levelPercent(299, 300, 1500) != 255)   fail("inside the blind zone should be invalid");
    if (levelPercent(300, 300, 1500) != 100)   fail("full should read 100");
    if (levelPercent(1500, 300, 1500) != 0)    fail("empty should read 0");
    if (levelPercent(3000, 300, 1500) != 0)    fail("past empty should clamp to 0, not wrap");
    if (levelPercent(900, 300, 1500) != 50)    fail("halfway should read 50");
    if (levelPercent(500, 0, 0) != 255)        fail("uncalibrated tank must not read a level");
    if (levelPercent(500, 1500, 300) != 255)   fail("inverted calibration must not read a level");
    if (levelPercent(500, 300, 300) != 255)    fail("zero span must not divide by zero");

    /* a short dosing tank and a deep water tank must both behave */
    int prev = 101;
    for (uint16_t mm = 300; mm <= 1500; mm++) {
        int pct = levelPercent(mm, 300, 1500);
        if (pct > prev || pct > 100) { fail("level is not monotonic in distance"); break; }
        prev = pct;
    }
    if (levelPercent(250, 250, 900) != 100)    fail("dosing full should read 100");
    if (levelPercent(575, 250, 900) != 50)     fail("dosing halfway should read 50");
    if (levelPercent(880, 250, 900) >= 20)     fail("nearly empty dosing should be under the low mark");

    /* --- the production firmware must not have drifted from either --- */
    if (prod_crc16(known, 9) != 0x4B37) fail("production crc16 fails the Modbus known answer");
    for (int seed = 0; seed < 512; seed++) {
        uint8_t n = (uint8_t)(1 + seed %% 39);
        for (uint8_t i = 0; i < n; i++) buf[i] = (uint8_t)(seed * 7 + i * 13);
        if (prod_crc16(buf, n) != crc16(buf, n)) { fail("production and node crc16 disagree"); break; }
    }

    /* Every calibration the /cal page can store, plus the ones it refuses, swept
     * against the sketch's copy. Boundary equality is not enough here: these two
     * are separate source files now and only a sweep catches a divergence in the
     * middle of the range. */
    for (uint16_t full = 200; full <= 400; full += 50) {
        for (uint16_t empty = 500; empty <= 2000; empty += 100) {
            for (uint16_t mm = 0; mm <= 2200; mm += 7) {
                if (prod_levelPercent(mm, full, empty) != levelPercent(mm, full, empty)) {
                    fail("production and sketch level maths disagree");
                    full = 500; empty = 3000; break;
                }
            }
        }
    }
    /* and the refusals, explicitly, because these are the ones that matter */
    if (prod_levelPercent(0, 300, 1500) != 255)      fail("production: no echo must not be a level");
    if (prod_levelPercent(500, 0, 0) != 255)         fail("production: uncalibrated must not be a level");
    if (prod_levelPercent(500, 1500, 300) != 255)    fail("production: inverted must not be a level");
    if (prod_levelPercent(500, 300, 300) != 255)     fail("production: zero span must not divide by zero");

    if (failures) return 1;
    printf("OK: node, bench hub and production hub agree on CRC-16/Modbus;\\n");
    printf("    framing and level maths hold across all three.\\n");
    return 0;
}
"""


def read(path):
    return io.open(path, encoding='utf-8').read()


def func(text, signature, rename=None):
    """One function, verbatim, from its signature to its closing brace."""
    start = text.index(signature)
    end = text.index('\n}', start) + 2
    body = text[start:end] + '\n'
    return body.replace(signature, rename, 1) if rename else body


def main():
    node, hub = read(NODE), read(HUB)
    prod_rs485, prod_cal = read(PROD_RS485), read(PROD_CAL)

    node_src = func(node, 'uint16_t crc16(const uint8_t *buf, uint8_t len)')
    hub_src = (
        func(hub, 'uint16_t crc16(const uint8_t *buf, uint8_t len)',
             'uint16_t hub_crc16(const uint8_t *buf, uint8_t len)')
        + func(hub, 'uint8_t levelPercent(uint16_t distanceMM, uint16_t fullMM, uint16_t emptyMM)')
    )

    prod_src = (
        func(prod_rs485, 'uint16_t crc16(const uint8_t *buf, uint8_t len)',
             'uint16_t prod_crc16(const uint8_t *buf, uint8_t len)')
        + func(prod_cal, 'uint8_t levelPercent(uint16_t distanceMM, uint16_t fullMM, uint16_t emptyMM)',
               'uint8_t prod_levelPercent(uint16_t distanceMM, uint16_t fullMM, uint16_t emptyMM)')
    )

    tmp = tempfile.mkdtemp()
    c_path = os.path.join(tmp, 'frame.c')
    exe = os.path.join(tmp, 'frame.exe')
    io.open(c_path, 'w', encoding='utf-8').write(HARNESS % (node_src, hub_src, prod_src))

    if subprocess.call(['gcc', '-Wall', '-Wextra', '-Werror', '-o', exe, c_path]) != 0:
        print('gcc refused the extracted code -- that is the finding.')
        return 1
    return subprocess.call([exe])


if __name__ == '__main__':
    sys.exit(main())
