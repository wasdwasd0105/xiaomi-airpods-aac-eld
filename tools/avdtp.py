#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
"""Decodes AVDTP signalling out of a btsnoop HCI capture.

Finds the AVDTP signalling channel by following L2CAP connection setup for
PSM 0x0019, then prints each signalling message with its codec capability
elements decoded.
"""
import struct, sys

PSM_AVDTP = 0x0019

SIG = {0x01:'DISCOVER',0x02:'GET_CAPABILITIES',0x03:'SET_CONFIGURATION',
       0x04:'GET_CONFIGURATION',0x05:'RECONFIGURE',0x06:'OPEN',0x07:'START',
       0x08:'CLOSE',0x09:'SUSPEND',0x0a:'ABORT',0x0b:'SECURITY_CONTROL',
       0x0c:'GET_ALL_CAPABILITIES',0x0d:'DELAYREPORT'}
MT = {0:'cmd',1:'general-reject',2:'accept',3:'REJECT'}
CAT = {0x01:'MediaTransport',0x02:'Reporting',0x03:'Recovery',0x04:'ContentProtection',
       0x05:'HeaderCompression',0x06:'Multiplexing',0x07:'MediaCodec',0x08:'DelayReporting'}
ERR = {0x01:'BAD_HEADER_FORMAT',0x11:'BAD_LENGTH',0x12:'BAD_ACP_SEID',0x13:'SEP_IN_USE',
       0x14:'SEP_NOT_IN_USE',0x17:'BAD_SERV_CATEGORY',0x18:'BAD_PAYLOAD_FORMAT',
       0x19:'NOT_SUPPORTED_COMMAND',0x1a:'INVALID_CAPABILITIES',
       0x1b:'BAD_RECOVERY_TYPE',0x1c:'BAD_MEDIA_TRANSPORT_FORMAT',
       0x1e:'BAD_RECOVERY_FORMAT',0x1f:'BAD_ROHC_FORMAT',0x20:'BAD_CP_FORMAT',
       0x21:'BAD_MULTIPLEXING_FORMAT',0x22:'UNSUPPORTED_CONFIGURATION',
       0x23:'BAD_STATE',0x25:'UNSUPPORTED_CONFIGURATION',
       0x29:'INVALID_CODEC_TYPE',0x2a:'NOT_SUPPORTED_CODEC_TYPE'}

def codec_name(body):
    if len(body) < 2: return '?', ''
    ct = body[1]
    if ct == 0x00: return 'SBC', ''
    if ct == 0x02: return 'MPEG-2,4 AAC', ''
    if ct == 0xff and len(body) >= 8:
        v = int.from_bytes(body[2:6], 'little')
        c = int.from_bytes(body[6:8], 'little')
        if (v, c) == (0x4c, 0x8001):
            extra = ''
            if len(body) >= 16:
                obj = int.from_bytes(body[8:10], 'big')
                fc = int.from_bytes(body[10:12], 'big')
                rate = int.from_bytes(body[13:16], 'big')
                extra = (f'  obj=0x{obj:04x} freq=0x{fc>>4:03x} chan=0x{fc & 0xF:x} '
                         f'vbr={"1" if rate & 0x800000 else "0"} rate={rate & 0x7fffff}')
            return 'Apple AAC-ELD', extra
        return f'vendor 0x{v:x}/0x{c:04x}', ''
    return f'codec type 0x{ct:02x}', ''

def load(path):
    f = open(path, 'rb'); f.read(16)
    out = []
    while True:
        rh = f.read(24)
        if len(rh) < 24: break
        olen, ilen, flags, drops, ts = struct.unpack('>IIIIq', rh)
        p = f.read(ilen)
        if len(p) < ilen: break
        out.append((flags & 1, p))
    return out

def main(path):
    recs = load(path)
    asm = {}
    pdus = []
    for d, p in recs:
        if not p or p[0] != 0x02: continue
        acl = p[1:]
        if len(acl) < 4: continue
        h, dlen = struct.unpack('<HH', acl[:4])
        handle = h & 0xFFF; pb = (h >> 12) & 0x3
        body = acl[4:4+dlen]
        key = handle
        if pb == 0x1:
            asm.setdefault(key, bytearray()).extend(body)
        else:
            asm[key] = bytearray(body)
        buf = asm.get(key)
        if buf and len(buf) >= 4:
            plen, cid = struct.unpack('<HH', bytes(buf[:4]))
            if len(buf) >= 4 + plen:
                pdus.append((d, handle, cid, bytes(buf[4:4+plen])))
                del asm[key]

    # Follow L2CAP signalling to learn which CIDs carry AVDTP.
    avdtp_cids = set()
    pending = {}
    for d, handle, cid, pdu in pdus:
        if cid != 0x0001 or len(pdu) < 4: continue
        code, ident, plen = pdu[0], pdu[1], int.from_bytes(pdu[2:4], 'little')
        data = pdu[4:4+plen]
        if code == 0x02 and len(data) >= 4:          # CONNECTION_REQ
            psm = int.from_bytes(data[0:2], 'little')
            scid = int.from_bytes(data[2:4], 'little')
            pending[(handle, ident)] = (psm, scid)
            if psm == PSM_AVDTP: avdtp_cids.add(scid)
        elif code == 0x03 and len(data) >= 4:        # CONNECTION_RSP
            dcid = int.from_bytes(data[0:2], 'little')
            scid = int.from_bytes(data[2:4], 'little')
            got = pending.get((handle, ident))
            if got and got[0] == PSM_AVDTP:
                avdtp_cids.add(dcid); avdtp_cids.add(scid)

    print(f"AVDTP channels: {sorted(hex(c) for c in avdtp_cids)}\n")

    n = 0
    for d, handle, cid, pdu in pdus:
        if cid not in avdtp_cids or len(pdu) < 2: continue
        mt = pdu[0] & 0x3
        if (pdu[0] >> 2) & 0x3 != 0:   # only single-packet messages
            continue
        sig = pdu[1] & 0x3f
        if sig not in SIG: continue
        who = 'phone -> pods' if d == 0 else 'pods  -> phone'
        n += 1
        head = f"{n:3d}  {who}  {SIG[sig]:21s} {MT[mt]}"
        if sig in (0x03, 0x05) and mt == 0:
            off = 4 if sig == 0x03 else 3
            if sig == 0x03:
                print(head + f"  acp_seid={pdu[2] >> 2} int_seid={pdu[3] >> 2}")
            else:
                print(head + f"  acp_seid={pdu[2] >> 2}")
            j = off
            while j + 1 < len(pdu):
                cat, losc = pdu[j], pdu[j+1]
                body = pdu[j+2:j+2+losc]
                if cat == 0x07:
                    nm, extra = codec_name(body)
                    print(f"          MediaCodec: {nm}{extra}")
                    print(f"          raw: {bytes([losc]).hex()} {body.hex(' ')}")
                else:
                    print(f"          {CAT.get(cat, hex(cat))}")
                j += 2 + losc
        elif mt == 3 and len(pdu) >= 3:
            if sig in (0x03, 0x05) and len(pdu) >= 4:
                print(head + f"  failing_category={CAT.get(pdu[2], hex(pdu[2]))} "
                             f"error=0x{pdu[3]:02x} {ERR.get(pdu[3], 'unknown')}")
            else:
                print(head + f"  error=0x{pdu[2]:02x} {ERR.get(pdu[2], 'unknown')}")
        elif sig == 0x01 and mt == 2:
            seps = []
            for j in range(2, len(pdu) - 1, 2):
                seps.append(f"SEID {pdu[j] >> 2}{'/in-use' if (pdu[j] >> 1) & 1 else ''}"
                            f"{'(SNK)' if (pdu[j+1] >> 3) & 1 else '(SRC)'}")
            print(head + "  " + ", ".join(seps))
        elif sig in (0x02, 0x0c) and mt == 2:
            j = 2; caps = []
            while j + 1 < len(pdu):
                cat, losc = pdu[j], pdu[j+1]
                body = pdu[j+2:j+2+losc]
                if cat == 0x07:
                    nm, extra = codec_name(body)
                    caps.append(nm + extra)
                else:
                    caps.append(CAT.get(cat, hex(cat)))
                j += 2 + losc
            print(head + "  " + "; ".join(caps))
        elif sig in (0x06, 0x07, 0x08, 0x09, 0x0a) and mt in (0, 2):
            extra = f"  seid={pdu[2] >> 2}" if len(pdu) > 2 and mt == 0 else ""
            print(head + extra)
        else:
            print(head)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit('usage: tools/avdtp.py btsnoop_hci.log')
    main(sys.argv[1])
