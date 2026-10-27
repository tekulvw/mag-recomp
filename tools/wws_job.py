"""Decoder for WWS job records as found in guest memory (464-byte jobs written by the material flush stage).

Layout (big-endian): 32 eight-byte command slots, a 24-entry DMA list of (size, effective address)
pairs, and a 16-byte marker. The command list occupies the leading slots up to the first end
command; the remaining slots carry the job's parameter data. Each command's first word holds the
flags: length in quadwords (14 bits), a pad bit, continue-after-discard-write, input-read,
input-gather, output-shareable, shareable-write-if-discarded, a 3-bit command number, a 4-bit
logical buffer set and a 5-bit logical buffer number (most significant bits first). The second word
depends on the command: a buffer-set description (first page, pages per buffer, buffer count, first
buffer), a main-memory address in quadwords, a buffer-set mask, or a dependency address."""
import argparse, json, struct, sys

FIELDS = (('length_qwords', 0, 14), ('continue_after_discard_write', 15, 1), ('input_read', 16, 1),
          ('input_gather', 17, 1), ('output_shareable', 18, 1), ('shareable_write_if_discarded', 19, 1),
          ('command', 20, 3), ('buffer_set', 23, 4), ('buffer', 27, 5))

def flags(word):
    return {name: (word >> (32 - start - width)) & ((1 << width) - 1) for name, start, width in FIELDS}

def buffer_set(word):
    return dict(first_page=word >> 24, pages_per_buffer=(word >> 16) & 0xFF,
                buffers=(word >> 8) & 0x7F, first_buffer=word & 0xFF)

def decode(record):
    if len(record) != 464:
        raise ValueError('a WWS job record is 464 bytes')
    w = struct.unpack('>116I', record)
    slots = [(w[2 * i], w[2 * i + 1]) for i in range(32)]
    dma = [dict(size=w[64 + 2 * i], ea=w[65 + 2 * i]) for i in range(24)]
    return dict(slots=slots, commands=[dict(flags(a), word1=b) for a, b in slots], dma=dma, marker=list(w[112:116]))

def main(argv=None):
    ap = argparse.ArgumentParser(); ap.add_argument('record', help='raw 464-byte record file'); a = ap.parse_args(argv)
    print(json.dumps(decode(open(a.record, 'rb').read()), indent=1))

if __name__ == '__main__':
    main()
