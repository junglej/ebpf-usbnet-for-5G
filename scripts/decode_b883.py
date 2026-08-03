#!/usr/bin/env python3
"""Decode 0xB883 (5G_NR_MAC_UL_Physical_Channel_Schedule_Report, v2.11) Raw Hex
payloads into per-PUSCH-TB records.

Layout reverse-engineered from MobileInsight's
dm_collector_c/nr_mac_ul_physical_channel_schedule_report.h (v2_11 Fmt chain)
and validated on RM500Q-GL captures (firmware RM500QGLABR13A02M4G):

  per record:  systime(4): slot(1), numerology(1, low 4b), frame(2, low 10b)
               records-hdr(4): num_carrier(1), skip(3)
  per carrier: carrier(4): id+flags(1), phychan_mask(1), skip(2)
               ch = ((phy & 0x0f) << 2) | ((id >> 6) & 3)
               ch & 0b010 -> PUSCH(40B)   ch & 0b100 -> PUCCH hdr(4)+N*16B
               ch & 0b010000 -> PRACH(8B)
  PUSCH(40) bit packing (see decode_pusch): HARQ ID, MCS(5b), RB start,
  num RBs(9b), TB size(21b), RV index, TX type (0=new, 1=retx), RNTI.

Input : CSV from mi_b883_raw.py  (epoch,num_records,raw_len,raw_hex)
Output: CSV, one PUSCH TB per line.
"""
import sys


def decode_pusch(p):
    b0, b1, b2, b3 = p[0], p[1], p[2], p[3]
    nr = int.from_bytes(p[4:6], "little")
    tb = int.from_bytes(p[6:8], "little")
    return dict(
        num_symbols=((b1 & 1) << 3) | ((b0 >> 5) & 7),
        harq_id=(b1 >> 1) & 0xF,
        mcs=((b2 & 3) << 3) | ((b1 >> 5) & 7),
        rb_start=(b3 << 1) | (b2 >> 7),
        num_rbs=nr & 0x1FF,
        tb_size=(tb << 5) | (nr >> 11),
        rv=(p[8] >> 2) & 7,
        tx_type=p[9] & 3,
        rnti=int.from_bytes(p[30:32], "little"),
    )


def main():
    src, dst = sys.argv[1], sys.argv[2]
    n = 0
    with open(dst, "w") as out:
        out.write("epoch,frame,slot,harq_id,mcs,rb_start,num_rbs,tb_size,rv,tx_type\n")
        for line in open(src):
            epoch, nrec, blen, raw = line.strip().split(",", 3)
            raw = bytes.fromhex(raw)
            off = 0
            try:
                for _ in range(int(nrec)):
                    slot = raw[off]
                    frame = int.from_bytes(raw[off + 2:off + 4], "little") & 0x3FF
                    off += 4
                    ncar = raw[off]
                    off += 4
                    for _ in range(ncar):
                        cid, phy = raw[off], raw[off + 1]
                        off += 4
                        ch = ((phy & 0x0F) << 2) | ((cid >> 6) & 3)
                        if ch & 0b010:
                            d = decode_pusch(raw[off:off + 40])
                            off += 40
                            out.write(f"{float(epoch):.6f},{frame},{slot},{d['harq_id']},"
                                      f"{d['mcs']},{d['rb_start']},{d['num_rbs']},{d['tb_size']},"
                                      f"{d['rv']},{d['tx_type']}\n")
                            n += 1
                        elif ch & 0b100:
                            npu = raw[off]
                            off += 4 + npu * 16
                        elif ch & 0b010000:
                            off += 8
            except IndexError:
                pass
    print(f"decoded {n} PUSCH records -> {dst}", file=sys.stderr)


if __name__ == "__main__":
    main()
