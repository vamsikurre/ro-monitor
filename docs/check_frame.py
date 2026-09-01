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
  * the node's Dallas CRC-8, which gates every DS18B20 reading
  * the node's 4-20 mA pressure conversion (the TWT fallback sensor) lands on
    RANGE at 4 mA and near zero at 20 mA, and refuses a broken loop -- the
    numbers it returns are indistinguishable from an ultrasonic distance, which
    is exactly why they have to be right
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
PROD_PRIV = 'firmware/hub_prod/main/app_priv.h'

HARNESS = """
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Both copies of levelPercent() reference this, so the harness must define it.
   It is the AJ-SR04M's minimum usable range - WIRING.md section 9.0. */
#define BLIND_ZONE_MM 200

/* Same for the TDS limits, lifted from app_priv.h by the extractor below so the
   checker cannot disagree with the firmware about where "out of range" is. */
%s

/* --- lifted from the production firmware: the TDS maths --- */
%s

/* --- lifted from ro_node.ino --- */
%s

/* --- lifted from esp32_hub_test.ino (crc renamed to keep both) --- */
%s

/* --- lifted from the production firmware (renamed again to keep all three) --- */
%s

static int failures = 0;

static void fail(const char *what) { printf("%%s\\n", what); failures++; }

/* microamps -> ADC counts, the inverse of what pressureMM() does */
static uint16_t counts_for_ua(uint32_t uA) {
    uint32_t mV = uA * PRESS_SENSE_OHMS / 1000UL;
    return (uint16_t)((mV * 1023UL + PRESS_ADC_MV / 2) / PRESS_ADC_MV);
}

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
    /* 299 mm against full=300 is NOT the blind zone - it is 1 mm over-full, and a
       real measurement. This assertion used to demand 255 and its own name gave
       the confusion away: the two cases shared a branch. */
    if (levelPercent(299, 300, 1500) != 100)   fail("1mm over-full should read 100, not invalid");
    if (levelPercent(201, 300, 1500) != 100)   fail("just outside the blind zone should read 100");
    if (levelPercent(199, 300, 1500) != 255)   fail("inside the blind zone must stay invalid");
    if (levelPercent(1, 300, 1500) != 255)     fail("hard against the transducer must stay invalid");
    /* The dosing barrel is the case that prompted this: full 250 against a 200 mm
       blind zone leaves only 50 mm in which "over-full" can be distinguished from
       "fouled", so both edges of that narrow band are pinned here. */
    if (levelPercent(249, 250, 900) != 100)    fail("over-filled dosing barrel should read 100");
    if (levelPercent(200, 250, 900) != 100)    fail("dosing at the blind-zone edge should read 100");
    if (levelPercent(199, 250, 900) != 255)    fail("dosing inside the blind zone must be invalid");
    if (levelPercent(300, 300, 1500) != 100)   fail("full should read 100");
    if (levelPercent(1500, 300, 1500) != 0)    fail("empty should read 0");
    if (levelPercent(3000, 300, 1500) != 0)    fail("past empty should clamp to 0, not wrap");
    if (levelPercent(900, 300, 1500) != 50)    fail("halfway should read 50");
    if (levelPercent(500, 0, 0) != 255)        fail("uncalibrated tank must not read a level");
    if (levelPercent(500, 1500, 300) != 255)   fail("inverted calibration must not read a level");
    if (levelPercent(500, 300, 300) != 255)    fail("zero span must not divide by zero");

    /* a short dosing tank and a deep water tank must both behave */
    int prev = 101;
    for (uint16_t mm = BLIND_ZONE_MM; mm <= 1500; mm++) {
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

    /* --- the node's pressure fallback, whose output the hub cannot tell apart
       from an ultrasonic distance. The counts are DERIVED from the sketch's own
       PRESS_SENSE_OHMS rather than written in: that resistor is a tuning knob
       sized against the loop's voltage headroom (WIRING.md 9.4.2), and a test
       that hardcodes "4 mA = 123 counts" breaks the moment someone turns it. --- */
    if (pressureMM(0) != 0)      fail("pressure: a dead loop must not read as a full tank");
    if (pressureMM(counts_for_ua(2000)) != 0)  fail("pressure: 2 mA is a broken loop, not a level");
    if (pressureMM(counts_for_ua(4000)) != PRESS_RANGE_MM) fail("pressure: 4 mA must read empty, not 0");
    if (pressureMM(1023) != 0)   fail("pressure: over-range must read as no reading");
    {   /* 12 mA is half scale. Allow a couple of counts of slop either way. */
        int mid = pressureMM(counts_for_ua(12000)), want = PRESS_RANGE_MM / 2;
        if (mid < want - 20 || mid > want + 20) fail("pressure: 12 mA is not half scale");
        /* and it must fall as the tank fills, or the hub's maths runs backwards */
        int prev = PRESS_RANGE_MM + 1;
        for (uint16_t c = counts_for_ua(4000); c <= counts_for_ua(20000); c++) {
            int mm = pressureMM(c);
            if (mm > prev) { fail("pressure: distance must shrink as the loop current rises"); break; }
            prev = mm;
        }
    }
    /* the whole point of the live zero: empty and cut-cable are different states */
    if (pressureMM(counts_for_ua(4000)) == pressureMM(0)) fail("pressure: empty and a cut cable must differ");

    /* The DS18B20's Dallas CRC-8 is not the SHT30's, despite the same polynomial:
       it is reflected, and getting that wrong rejects every scratchpad the probe
       ever sends. The failure is total and silent - no temperature, therefore no
       TDS either - so it gets the same known-answer test the Modbus CRC gets.
       CRC-8/MAXIM of "123456789" is 0xA1. */
    if (owCrc8(known, 9) != 0xA1) fail("1-Wire crc8 fails the Dallas/MAXIM known answer");

    /* --- TDS: a number that will be shown as water quality, so the refusals
       matter more than the arithmetic. k=100 is the uncalibrated default. --- */
    if (tdsPPM(0, 250, 100) != TDS_INVALID)     fail("tds: an unpowered probe must not read as pure water");
    if (tdsPPM(3000, 250, 100) != TDS_INVALID)  fail("tds: over-range mV must not become a reading");
    if (tdsPPM(500, 250, 0) != TDS_INVALID)     fail("tds: a zero k factor must not read a level");
    if (tdsPPM(500, -50, 100) != TDS_INVALID)   fail("tds: sub-zero water is a faulty sensor");
    if (tdsPPM(500, 850, 100) != TDS_INVALID)   fail("tds: the DS18B20 85 C default must not be a reading");
    {
        /* Monotonic in millivolts: more conductive water must never read lower. */
        int prev = -1;
        for (uint16_t mv = 1; mv <= TDS_MV_MAX; mv++) {
            int p = tdsPPM(mv, 250, 100);
            if (p == TDS_INVALID) continue;
            if (p < prev) { fail("tds: ppm is not monotonic in millivolts"); break; }
            prev = p;
        }
        if (prev <= 0) fail("tds: nothing in the probe's range produced a reading");

        /* Temperature compensation must actually compensate. The SAME water at
           15 C and 35 C is more conductive when warm, so the raw millivolts
           differ - and the compensated ppm must not. This is the whole reason
           the DS18B20 is in the bill of materials. */
        int cold = tdsPPM(400, 150, 100);
        int warm = tdsPPM(400, 350, 100);
        if (cold == TDS_INVALID || warm == TDS_INVALID) {
            fail("tds: plain readings at 15 C and 35 C should both be valid");
        } else if (cold <= warm) {
            fail("tds: compensation runs backwards - warm water must need a smaller correction");
        }

        /* And the k factor has to scale it, or calibration does nothing. */
        int k100 = tdsPPM(400, 250, 100), k150 = tdsPPM(400, 250, 150);
        if (k150 <= k100) fail("tds: the calibration factor does not scale the result");
    }

    /* --- rejection: the number that says whether the membrane is healthy --- */
    if (rejectionPercent(TDS_INVALID, 20) != -1) fail("rejection: an invalid feed must not yield a figure");
    if (rejectionPercent(600, TDS_INVALID) != -1) fail("rejection: an invalid permeate must not yield a figure");
    if (rejectionPercent(10, 2) != -1)           fail("rejection: too dilute a feed is not a measurement");
    if (rejectionPercent(600, 600) != 0)         fail("rejection: equal readings are zero rejection");
    if (rejectionPercent(600, 900) != 0)         fail("rejection: permeate above feed must not go negative");
    if (rejectionPercent(1000, 50) != 95)        fail("rejection: 1000 -> 50 is 95 %%");
    if (rejectionPercent(600, 300) != 50)        fail("rejection: half the feed is 50 %%");

    if (failures) return 1;
    printf("OK: node, bench hub and production hub agree on CRC-16/Modbus;\\n");
    printf("    framing, level and TDS maths hold across all three.\\n");
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

    # The PRESS_* defines come across with the function that uses them, so the
    # checker cannot drift from the sketch's constants.
    node_src = (
        '\n'.join(re.findall(r'^#define PRESS_\w+.*$', node, re.M)) + '\n\n'
        + func(node, 'uint16_t crc16(const uint8_t *buf, uint8_t len)')
        + func(node, 'uint16_t pressureMM(uint16_t counts)')
        + func(node, 'uint8_t owCrc8(const uint8_t *d, uint8_t n)')
    )
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

    # The TDS maths and the constants it is bounded by, taken from the firmware
    # rather than restated here - a checker with its own copy of a limit proves
    # nothing about the firmware's.
    priv_defines = re.findall(r'^#define TDS_\w+\s+\d+.*$', read(PROD_PRIV), re.M)
    tds_defines = chr(10).join(priv_defines) + chr(10) + '#define TDS_INVALID 0xFFFF'
    tds_src = (
        func(prod_cal, 'uint16_t tdsPPM(uint16_t mv, int16_t water_temp_deci_c, uint16_t k_x100)')
        + func(prod_cal, 'int16_t rejectionPercent(uint16_t feed_ppm, uint16_t permeate_ppm)')
    )

    tmp = tempfile.mkdtemp()
    c_path = os.path.join(tmp, 'frame.c')
    exe = os.path.join(tmp, 'frame.exe')
    io.open(c_path, 'w', encoding='utf-8').write(
        HARNESS % (tds_defines, tds_src, node_src, hub_src, prod_src))

    if subprocess.call(['gcc', '-Wall', '-Wextra', '-Werror', '-o', exe, c_path]) != 0:
        print('gcc refused the extracted code -- that is the finding.')
        return 1
    return subprocess.call([exe])


if __name__ == '__main__':
    sys.exit(main())
