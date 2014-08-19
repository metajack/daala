#!/usr/bin/env python

import simplejson as json

acct_data = json.loads(open("acct.json").read())

frames = 0
mvf_frac_bits = [0, 0, 0, 0, 0] # first field unused
mv_frac_bits = [0, 0, 0, 0, 0]
for frame in acct_data:
    frames += 1
    mv_frac_bits[0] += frame["technique"]["motion-vectors0"]
    for i in range(1, 5):
        mvf_frac_bits[i] += frame["technique"]["motion-flags%d" % i]
        mv_frac_bits[i] += frame["technique"]["motion-vectors%d" % i]

mvf_bytes = [b / 16 for b in mvf_frac_bits]
mv_bytes = [b / 16 for b in mv_frac_bits]

print "mv data per frame:"
print "  level 0> flags:      --- Bpf  vectors: %8.f Bpf" % (mv_bytes[0] / frames)
for i in range(1, 5):
    print "  level %d> flags: %8.f Bpf  vectors: %8.f Bpf" % (i,
                                                              mvf_bytes[i] / frames,
                                                              mv_bytes[i] / frames)


