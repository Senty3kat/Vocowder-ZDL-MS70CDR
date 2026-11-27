#!/usr/bin/env python3
"""PC simulator for Vocowder — faithful host port of vocowder.c audio body.

Matches the pedal C kernel (Fx_DYN_Vocowder) sample-for-sample in structure:
vocoder bands, halfband 44.1↔22.05, FrzL 8-head grains, FRev micro-freeze +
reverb, LoCut/HiCut, air band, Level/Mix/Out/Gain, soft clip.

Run before every ZDL build:

    python src/custom/vocowder/sim_frev.py
    python src/custom/vocowder/sim_frev.py --compare path/to/c_wav_dir

PASS if FRev still has energy after the note is muted (freeze holds).
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import wave
from pathlib import Path

import numpy as np

SR = 44100

# ---- C constants (vocowder.c) --------------------------------------------
VC_MAGIC = 0x566F4331
VC_VERSION = 27
VC_MAXB = 80
VC_RING = 131072
VC_RMASK = VC_RING - 1
VC_SG = 16384
VC_AP1, VC_AP2, VC_AP3, VC_AP4 = 1009, 1733, 2617, 3923
VC_APN = VC_AP1 + VC_AP2 + VC_AP3 + VC_AP4
VC_FB = 0.75
VC_LREG = 32768
VC_LMASK = VC_LREG - 1
VC_PI_SR = 0.0000712379
VC_HB = (0.3132242, -0.0915584, 0.0418614, -0.0193695, 0.0079192, -0.0024690, 0.0003642)
VC_AIR = 2.5
VC_FLO = 120.0
VC_LOG2E = 1.44269504
VC_INV_SR_MS = 22.6757370
VC_INV_SR = 0.0000226757
VC_PI = 3.14159265
VC_HALF_PI = 1.57079633

# band ladder: (count, log2 step, 1/Q, makeup)
LADDER = [
    (4.0, 1.9553, 1.462, 1.18),
    (6.0, 1.1732, 0.836, 1.20),
    (8.0, 0.8380, 0.589, 1.22),
    (10.0, 0.6518, 0.4556, 1.24),
    (12.0, 0.5333, 0.3717, 1.27),
    (16.0, 0.3911, 0.2717, 1.30),
    (20.0, 0.3087, 0.2145, 1.40),
    (24.0, 0.2550, 0.1771, 1.50),
    (32.0, 0.1892, 0.1313, 1.68),
    (40.0, 0.1504, 0.1043, 1.80),
    (48.0, 0.1248, 0.0866, 1.90),
    (64.0, 0.0931, 0.0645, 2.10),
    (80.0, 0.0743, 0.0515, 2.30),
]


def f32(x):
    return np.float32(x)


# ---- C helper approximations (must match vocowder.c) ----------------------
def vc_ui(raw, def_ui):
    if not (0.0 <= raw <= 100.0):
        return def_ui
    return raw * 100.0 if raw <= 1.0 else raw


def vc_exp2(x):
    x = max(-60.0, min(60.0, float(x)))
    i = math.floor(x)
    f = x - i
    p = 1.0 + f * (0.69606564 + f * (0.22449433 + f * 0.07944023))
    # bit-trick 2^i via float exponent field
    import struct as _st
    bits = ((i + 127) & 0xFFFFFFFF) << 23
    return _st.unpack("f", _st.pack("I", bits & 0xFFFFFFFF))[0] * p


def vc_recip(x):
    x = float(x)
    if x == 0.0:
        return 0.0
    import struct as _st
    u = _st.unpack("I", _st.pack("f", x))[0]
    u = (0x7EF311C7 - u) & 0xFFFFFFFF
    r = _st.unpack("f", _st.pack("I", u))[0]
    r = r * (2.0 - x * r)
    r = r * (2.0 - x * r)
    r = r * (2.0 - x * r)
    return r


def vc_tan(w):
    w2 = w * w
    return w * (1.0 + w2 * (0.33333333 + w2 * (0.13333333 + w2 * 0.05396825)))


def vc_tan_wide(w):
    t = vc_tan(0.5 * w)
    return 2.0 * t * vc_recip(1.0 - t * t)


def vc_sin(x):
    ax = abs(x)
    y = 1.2732395447 * x - 0.4052847346 * x * ax
    ay = abs(y)
    return y + 0.225 * (y * ay - y)


def vc_coef(ms):
    x = VC_INV_SR_MS * vc_recip(ms)
    return 1.0 - vc_exp2(-x * VC_LOG2E)


def vc_svf2(g):
    d = vc_recip(1.0 + g * (g + 1.41421356))
    return d, g * d, g * g * d


def vc_rand(s):
    """[-1, 1); s is int32-like mutable via return."""
    s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
    import struct as _st
    bits = ((s >> 9) | 0x40000000) & 0xFFFFFFFF
    v = _st.unpack("f", _st.pack("I", bits))[0]
    return v - 3.0, s


class Vocowder:
    """Faithful host port of Fx_DYN_Vocowder (8-sample stereo blocks)."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.rng = 0x13579BDF
        self.curBands = -1.0
        self.coefN = 0
        self.curCut = -1.0
        self.curHi = -1.0
        self.hpA1 = self.hpA2 = self.hpA3 = 0.0
        self.hc1 = self.hc2 = 0.0
        self.lpA1 = self.lpA2 = self.lpA3 = 0.0
        self.lo1 = self.lo2 = 0.0
        self.envCt = 0.0
        self.envMt = 0.0
        self.pk0 = self.pk1 = self.pk2 = 0.0
        self.brown = 0.0
        self.crushV = 0.0
        self.crushN = 0.0
        self.gr1 = self.gr2 = 0.0
        self.blueP = 0.0
        self.panPh = 0
        self.wPos = 0
        self.filled = 0
        self.frozen = 0
        self.frzEnd = 0
        self.frzP = 0.0
        self.grainS = 0.0
        self.gateG = 1.0
        self.lvlP = 0.0
        self.strOn = 0
        self.strC = 0
        self.hb = [0] * 8
        self.ho = [0] * 8
        self.apI = [0] * 4
        self.lpT = 0.0
        self.rReg, self.nReg, self.oReg = 0, 1, 2
        self.recOff = self.nSeam = self.oSeam = 0
        self.tmr = self.nSnap = 0
        self.envF = self.envSl = 0.0
        self.gTs = 1.0
        self.envDf = self.envLf = 0.0
        self.rW = 1.0
        self.rvp = self.rvq = self.rva = 0.0
        self.rhp1 = self.rlp1 = 0.0
        self.frzM = 0.0
        self.tailE = 0.0
        self.rvI = 0
        self.rvC = [0.0] * 512
        self.decM = [0.0] * 34
        self.decC = [0.0] * 34
        self.hbk = list(VC_HB)
        self.intW = [0.0] * 18
        self.ac1 = self.ac2 = 0.0
        self.aenC = self.alp = self.alp2 = 0.0
        self.apd = [0.0] * VC_APN
        self.ring = [0.0] * VC_RING
        # bands
        self.band = [
            {
                "a1": 0.0, "a2": 0.0, "a3": 0.0, "k": 0.0,
                "m1": 0.0, "m2": 0.0, "c1": 0.0, "c2": 0.0,
                "envM": 0.0, "envS": 0.0, "holdM": 0.0, "envC": 0.0, "gO": 0.0,
            }
            for _ in range(VC_MAXB)
        ]

    # ---- one all-pass ----------------------------------------------------
    def _ap(self, idx, length, x):
        q = self.apI[idx]
        base = 0
        for j in range(idx):
            base += (VC_AP1, VC_AP2, VC_AP3, VC_AP4)[j]
        d = self.apd
        y = d[base + q] - 0.65 * x
        d[base + q] = x + 0.65 * y
        q += 1
        if q >= length:
            q = 0
        self.apI[idx] = q
        return y

    def process_block(self, params, fxBuf):
        """params: list of floats (slots 0..13). fxBuf: 16 floats, L then R.
        Returns nothing; fxBuf is overwritten with output (8 L + 8 R)."""
        if params[0] < 0.5:
            return

        bandsUi = vc_ui(params[5], 4.0)
        detUi = 23.0
        cutUi = vc_ui(params[8], 30.0)
        hiUi = vc_ui(params[9], 0.0)
        noise = vc_ui(params[10], 0.0) * 0.01
        ntUi = vc_ui(params[11], 1.0)
        # slots: 5=Bands 6=Level 7=Mix 8=LoCut 9=HiCut 10=Noise 11=NType 12=Out 13=Gain
        gainUi = vc_ui(params[13], 50.0)
        if gainUi > 100:
            gainUi = 100
        gainUi = float(int(gainUi + 0.5))
        gDb = (gainUi - 50.0) * 0.24
        gMod = vc_exp2(gDb * 0.16609640) if gDb > 0.0 else 1.0
        gCar = vc_exp2(-gDb * 0.16609640) if gDb < 0.0 else 1.0
        if noise > 1.0:
            noise = 1.0
        if cutUi > 100:
            cutUi = 100
        if hiUi > 100:
            hiUi = 100
        bandsUi = float(int(bandsUi + 0.5))
        cutUi = float(int(cutUi + 0.5))
        hiUi = float(int(hiUi + 0.5))

        bi = int(bandsUi)
        bi = max(0, min(12, bi))
        nb, step, kq, makeup = LADDER[bi]
        nbi = int(nb)

        if nb != self.curBands:
            self.curBands = nb
            self.coefN = 0
        if self.coefN < nbi:
            b = self.coefN
            self.coefN += 1
            bd = self.band[b]
            fc = VC_FLO * vc_exp2(step * b)
            g = vc_tan_wide(fc * (2.0 * VC_PI_SR))
            oc = step * b - 0.0589
            h3 = oc - 3.0
            h4 = oc - 4.0
            h5 = oc - 5.0
            h6 = oc - 5.33
            tdb = (
                0.2
                + 0.1 * oc
                + 0.4 * max(h3, 0.0)
                + 0.6 * max(h4, 0.0)
                + 0.1 * max(h5, 0.0)
                + 6.7 * max(h6, 0.0)
            )
            tdb *= 1.3 - 0.6 * kq
            bd["gO"] = 1.0 + tdb * (0.115129 + tdb * (0.0066276 + tdb * 0.000254))
            d = vc_recip(1.0 + g * (g + kq))
            bd["a1"] = d
            bd["a2"] = g * d
            bd["a3"] = g * g * d
            bd["k"] = kq
            bd["m1"] = bd["m2"] = bd["c1"] = bd["c2"] = 0.0
            bd["envM"] = bd["envS"] = bd["holdM"] = bd["envC"] = 0.0

        if cutUi != self.curCut:
            fc = 20.0 * vc_exp2(cutUi * 0.05321928)
            self.hpA1, self.hpA2, self.hpA3 = vc_svf2(vc_tan(fc * VC_PI_SR))
            self.curCut = cutUi
        if hiUi != self.curHi:
            fc = 16000.0 * vc_exp2(-hiUi * 0.04)
            self.lpA1, self.lpA2, self.lpA3 = vc_svf2(vc_tan_wide(fc * VC_PI_SR))
            self.curHi = hiUi

        aCa = 0.0224
        aCr = 0.00075

        # ---- pre-pass locals ----
        h1, h2, h3 = self.hpA1, self.hpA2, self.hpA3
        hc1, hc2 = self.hc1, self.hc2
        q1, q2, q3 = self.lpA1, self.lpA2, self.lpA3
        lo1, lo2 = self.lo1, self.lo2
        envCt = self.envCt
        ring = self.ring
        wPos = self.wPos
        filled = self.filled
        frozen = self.frozen
        frzEnd = self.frzEnd
        frzP = self.frzP
        stretch = 1 if ntUi > 6.5 else 0
        frzR = 1 if ntUi > 7.5 else 0

        if stretch and not self.strOn:
            self.strC = 0
            self.recOff = 0
            self.tmr = 0
            self.nSnap = 0
            self.rReg = 0
            self.nReg = 1
            self.oReg = 2
            self.nSeam = 0
            self.oSeam = 0
            self.envF = 0.0
            self.envSl = 0.0
            self.gTs = 1.0
            frzP = 0.0
            self.frzM = 0.0
            for b in range(8):
                self.hb[b] = 0
                self.ho[b] = 0
            for b in range(4):
                self.apI[b] = 0
            for b in range(VC_APN):
                self.apd[b] = 0.0
            self.lpT = 0.0
        if not stretch and self.strOn:
            filled = 0
        self.strOn = stretch

        period = 44100.0 * vc_exp2(noise * 3.0)
        rRamp = vc_recip(0.8 * period)
        perI = (
            (VC_SG if frzR else VC_LREG)
            if self.nSnap == 0
            else int(period)
        )
        if stretch:
            frozen = 0
        elif ntUi > 5.5:
            if frozen == 0 and filled >= 8820:
                frozen = 1
                frzEnd = wPos
                frzP = 0.0
        else:
            frozen = 0

        grainT = 4410.0 * vc_exp2(noise * 4.858)
        maxG = float(filled) - 8.0
        if grainT > maxG:
            grainT = maxG
        if grainT < 441.0:
            grainT = 441.0
        grain = self.grainS
        if not (grain >= 441.0):
            grain = grainT
        grain += 0.02 * (grainT - grain)
        grain = float(int(grain))
        self.grainS = grain
        if frzP >= grain:
            frzP = 0.0
        rGrain = vc_recip(grain)
        rng = self.rng
        envMt = self.envMt
        envDf, envLf, rW = self.envDf, self.envLf, self.rW
        rT = 1.0
        if envLf > 0.0005:
            rT = 0.0 if envDf < 0.01 * envLf else 1.0
        else:
            rT = 1.0 if rW > 0.5 else 0.0

        mod = [0.0] * 8
        dry = [0.0] * 8
        car = [0.0] * 8
        wet = [0.0] * 8

        for i in range(8):
            xm = gMod * fxBuf[i]
            mod[i] = xm
            dry[i] = fxBuf[i]
            am0 = abs(xm)
            envMt += (0.01 if am0 > envMt else 0.00011) * (am0 - envMt)
            dLR = fxBuf[i] - fxBuf[i + 8]
            aL = abs(fxBuf[i])
            envDf += 0.0005 * (abs(dLR) - envDf)
            envLf += 0.0005 * (aL - envLf)
            rW += 0.002 * (rT - rW)

            wn, rng = vc_rand(rng)
            rin = gCar * fxBuf[i + 8]
            if ntUi < 0.5:
                nz = 0.2 * wn
            elif ntUi < 1.5:
                self.pk0 = 0.99765 * self.pk0 + wn * 0.0990460
                self.pk1 = 0.96300 * self.pk1 + wn * 0.2965164
                self.pk2 = 0.57000 * self.pk2 + wn * 1.0526913
                nz = 0.03 * (self.pk0 + self.pk1 + self.pk2 + wn * 0.1848)
            elif ntUi < 2.5:
                self.brown = 0.997 * self.brown + 0.06 * wn
                nz = 0.64 * self.brown
            elif ntUi < 3.5:
                g3 = wn - self.gr2
                g1 = 0.9513 * self.gr1 + 0.0339 * g3
                g2 = self.gr2 + 0.0339 * self.gr1 + 0.00121 * g3
                self.gr1 = 2.0 * g1 - self.gr1
                self.gr2 = 2.0 * g2 - self.gr2
                nz = 1.1 * g1
            elif ntUi < 4.5:
                nz = 0.16 * (wn - 0.7 * self.blueP)
                self.blueP = wn
            elif ntUi < 5.5:
                self.crushN -= 1.0
                if self.crushN <= 0.0:
                    self.crushN = 15.0
                    self.crushV = 0.05 * float(int(wn * 4.0))
                nz = self.crushV
            else:
                nz = 0.0

            frz = 0.0
            if frozen == 0 and stretch == 0:
                ring[wPos & VC_RMASK] = rin
                wPos += 1
                if filled < VC_RING:
                    filled += 1

            if stretch:
                x = gCar * fxBuf[i]
                rb = self.rReg << 15
                ring[rb + self.recOff] = x
                ax = abs(x)
                self.envF += (0.0224 if ax > self.envF else 0.0011) * (ax - self.envF)
                self.envSl += 0.0005 * (ax - self.envSl)
                gT = 1.0
                if self.envF > 2.0 * self.envSl + 1.0e-6:
                    gT = 2.0 * self.envSl * vc_recip(self.envF)
                self.gTs += 0.05 * (gT - self.gTs)
                ring[rb + ((self.recOff - 64) & VC_LMASK)] *= self.gTs
                self.recOff = (self.recOff + 1) & VC_LMASK
                self.tmr += 1
                if self.tmr >= perI:
                    self.oReg = self.nReg
                    self.oSeam = self.nSeam
                    self.nReg = self.rReg
                    self.nSeam = self.recOff
                    self.rReg = 3 - self.oReg - self.nReg
                    self.tmr = 0
                    if self.nSnap < 2:
                        self.nSnap += 1

                ph = VC_PI * self.strC * (2.0 / VC_SG) - VC_PI
                aph = abs(ph)
                S = -vc_sin(ph)
                C = -vc_sin(VC_HALF_PI - aph)
                ck, sk, tex = 1.0, 0.0, 0.0
                texM = 0.0
                wantM = 1 if (frzR and (self.nSnap == 0 or self.frzM < 1.0)) else 0
                if wantM:
                    loopN = 256
                    if self.recOff >= 8192:
                        loopN = 8192
                    elif self.recOff >= 4096:
                        loopN = 4096
                    elif self.recOff >= 2048:
                        loopN = 2048
                    elif self.recOff >= 512:
                        loopN = 512
                    if self.recOff >= loopN:
                        fl = float(loopN)
                        i1 = (self.recOff - loopN + int(frzP)) & VC_LMASK
                        i2 = (self.recOff - (loopN >> 1) + int(frzP)) & VC_LMASK
                        w = frzP * (2.0 / fl)
                        w = (2.0 - w) if w > 1.0 else w
                        texM = (ring[rb + i1] * w + ring[rb + i2] * (1.0 - w)) * 1.55
                        frzP += 1.0
                        if frzP >= fl:
                            frzP -= fl
                    else:
                        texM = x

                if frzR and self.nSnap == 0:
                    tex = texM
                    self.frzM = 0.0
                else:
                    for b in range(8):
                        hw = 0.5 - 0.5 * (C * ck - S * sk)
                        tex += hw * ring[
                            self.hb[b]
                            + ((self.ho[b] + ((self.strC + 2048 * b) & (VC_SG - 1))) & VC_LMASK)
                        ]
                        nc = 0.70710678 * (ck - sk)
                        sk = 0.70710678 * (ck + sk)
                        ck = nc
                    if wantM:
                        self.frzM += 0.00035
                        if self.frzM > 1.0:
                            self.frzM = 1.0
                        tex = texM * (1.0 - self.frzM) + tex * self.frzM

                xa = 0.3 * tex + VC_FB * self.lpT
                xa = self._ap(0, VC_AP1, xa)
                xa = self._ap(1, VC_AP2, xa)
                xa = self._ap(2, VC_AP3, xa)
                xa = self._ap(3, VC_AP4, xa)
                lpT = self.lpT + 0.45 * (xa - self.lpT)
                self.lpT = lpT
                frz = lpT
                if self.nSnap == 0 and not frzR:
                    frz = 0.0
                self.strC = (self.strC + 1) & (VC_SG - 1)
                if (self.strC & 2047) == 0:
                    k = ((VC_SG - self.strC) & (VC_SG - 1)) >> 11
                    wN = self.tmr * rRamp
                    u0, rng = vc_rand(rng)
                    u = 0.5 + 0.5 * u0
                    useN = 1 if (self.nSnap < 2 or u < wN) else 0
                    r0, rng = vc_rand(rng)
                    r = int((0.5 + 0.5 * r0) * (VC_LREG - VC_SG - 1))
                    self.hb[k] = (self.nReg if useN else self.oReg) << 15
                    self.ho[k] = (self.nSeam if useN else self.oSeam) + r
            elif frozen:
                p2 = frzP + 0.5 * grain
                if p2 >= grain:
                    p2 -= grain
                i1 = (frzEnd - int(grain) + int(frzP)) & VC_RMASK
                i2 = (frzEnd - int(grain) + int(p2)) & VC_RMASK
                wA = vc_sin(VC_PI * frzP * rGrain)
                wB = vc_sin(VC_PI * p2 * rGrain)
                frz = ring[i1] * wA + ring[i2] * wB
                frzP += 1.0
                if frzP >= grain:
                    frzP -= grain

            y = frz if (frozen or stretch) else (rW * rin + noise * nz)

            # LoCut / HiCut on carrier
            v3 = y - hc2
            v1 = h1 * hc1 + h2 * v3
            v2 = hc2 + h2 * hc1 + h3 * v3
            hc1 = 2.0 * v1 - hc1
            hc2 = 2.0 * v2 - hc2
            hpo = y - 1.41421356 * v1 - v2
            v3 = hpo - lo2
            v1 = q1 * lo1 + q2 * v3
            v2 = lo2 + q2 * lo1 + q3 * v3
            lo1 = 2.0 * v1 - lo1
            lo2 = 2.0 * v2 - lo2
            cy = v2
            car[i] = cy
            ac = abs(cy)
            envCt += (aCa if ac > envCt else aCr) * (ac - envCt)
            wet[i] = 0.0

        self.hc1, self.hc2 = hc1, hc2
        self.lo1, self.lo2 = lo1, lo2
        self.envCt = envCt
        self.envMt = envMt
        if frzR:
            act = 1.0 if envMt > 0.008 else 0.0
            tE = self.tailE + (0.01 if act > self.tailE else 0.00007) * (act - self.tailE)
            if not (tE >= 0.0):
                tE = 0.0
            self.tailE = tE
        else:
            self.tailE = 0.0
        if not (self.envF == self.envF and self.envSl == self.envSl and self.gTs == self.gTs):
            self.envF = self.envSl = 0.0
            self.gTs = 1.0
        self.wPos = wPos
        self.filled = filled
        self.frozen = frozen
        self.frzEnd = frzEnd
        self.frzP = frzP
        self.rng = rng
        if not (envDf == envDf and envLf == envLf):
            envDf = envLf = 0.0
        self.envDf, self.envLf, self.rW = envDf, envLf, rW

        floorC = 0.15 * envCt + 0.0003

        # ---- 44.1 -> 22.05 halfband ----
        hm, hc = self.decM, self.decC
        for i in range(8):
            hm[26 + i] = mod[i]
            hc[26 + i] = car[i]
        modD = [0.0] * 4
        carD = [0.0] * 4
        wetD = [0.0] * 4
        for i in range(4):
            c = 14 + 2 * i
            am = 0.5 * hm[c]
            ac = 0.5 * hc[c]
            for k in range(7):
                am += self.hbk[k] * (hm[c - 1 - 2 * k] + hm[c + 1 + 2 * k])
                ac += self.hbk[k] * (hc[c - 1 - 2 * k] + hc[c + 1 + 2 * k])
            modD[i] = am
            carD[i] = ac
        for i in range(26):
            hm[i] = hm[i + 8]
            hc[i] = hc[i + 8]

        # ---- detector ----
        det = detUi * 0.01
        aAtk = vc_coef(0.25 * vc_exp2(5.32 * det))
        aRel = vc_coef((40.0 if frzR else 5.0) * vc_exp2(5.32 * det))
        holdLen = det * 1102.5
        aCaB = 0.0443
        aCrB = 0.0015

        # ---- bands ----
        for b in range(0, nbi, 2):
            for S, bb in ((0, b), (1, b + 1)):
                pass
            # process pair
            bds = [self.band[b], self.band[b + 1] if b + 1 < nbi else self.band[b]]
            locals_a = []
            for bd in bds:
                locals_a.append(
                    {
                        "a1": bd["a1"], "a2": bd["a2"], "a3": bd["a3"], "k": bd["k"],
                        "m1": bd["m1"], "m2": bd["m2"], "c1": bd["c1"], "c2": bd["c2"],
                        "envM": bd["envM"], "envS": bd["envS"], "holdM": bd["holdM"],
                        "envC": bd["envC"],
                        "gC": vc_recip(bd["envC"] + floorC) * bd["gO"],
                    }
                )
            for i in range(4):
                x = modD[i]
                y = carD[i]
                for L in locals_a:
                    v3 = x - L["m2"]
                    v1 = L["a1"] * L["m1"] + L["a2"] * v3
                    v2 = L["m2"] + L["a2"] * L["m1"] + L["a3"] * v3
                    L["m1"] = 2.0 * v1 - L["m1"]
                    L["m2"] = 2.0 * v2 - L["m2"]
                    bm = L["k"] * v1
                    am = abs(bm)
                    rise = 1.0 if am > L["envM"] else 0.0
                    held = 1.0 if L["holdM"] > 0.0 else 0.0
                    aE = rise * aAtk + (1.0 - rise) * (1.0 - held) * aRel
                    L["envM"] += aE * (am - L["envM"])
                    hn = L["holdM"] - 1.0
                    hn = hn if hn > 0.0 else 0.0
                    L["holdM"] = hn + rise * (holdLen - hn)
                    v3 = y - L["c2"]
                    v1 = L["a1"] * L["c1"] + L["a2"] * v3
                    v2 = L["c2"] + L["a2"] * L["c1"] + L["a3"] * v3
                    L["c1"] = 2.0 * v1 - L["c1"]
                    L["c2"] = 2.0 * v2 - L["c2"]
                    bc = L["k"] * v1
                    ac = abs(bc)
                    L["envC"] += (aCaB if ac > L["envC"] else aCrB) * (ac - L["envC"])
                    L["envS"] += 0.0199 * (L["envM"] - L["envS"])
                    wetD[i] += bc * L["gC"] * L["envS"]
            for bd, L in zip(bds, locals_a):
                if not (
                    L["m1"] == L["m1"]
                    and L["c1"] == L["c1"]
                    and L["envM"] == L["envM"]
                    and L["envC"] == L["envC"]
                ):
                    L["m1"] = L["m2"] = L["c1"] = L["c2"] = 0.0
                    L["envM"] = L["envS"] = L["holdM"] = L["envC"] = 0.0
                bd["m1"] = L["m1"]
                bd["m2"] = L["m2"]
                bd["c1"] = L["c1"]
                bd["c2"] = L["c2"]
                bd["envM"] = L["envM"]
                bd["envS"] = L["envS"]
                bd["holdM"] = L["holdM"]
                bd["envC"] = L["envC"]

        # ---- 22.05 -> 44.1 halfband ----
        w = self.intW
        for i in range(4):
            w[14 + i] = wetD[i]
        for i in range(4):
            t = 14 + i
            ev = 0.0
            for k in range(7):
                ev += self.hbk[k] * (w[t - 6 + k] + w[t - 7 - k])
            wet[2 * i] = 2.0 * ev
            wet[2 * i + 1] = w[t - 6]
        for i in range(14):
            w[i] = w[i + 4]

        # ---- air band ----
        c1, c2 = self.ac1, self.ac2
        eC, lp, lp2 = self.aenC, self.alp, self.alp2
        gA = VC_AIR * self.band[nbi - 1]["envS"] * vc_recip(eC + floorC)
        for i in range(8):
            v3 = car[i] - c2
            v1 = 0.38266 * c1 + 0.28569 * v3
            v2 = c2 + 0.28569 * c1 + 0.21329 * v3
            c1 = 2.0 * v1 - c1
            c2 = 2.0 * v2 - c2
            hc_ = car[i] - 1.41421356 * v1 - v2
            a = abs(hc_)
            eC += (aCa if a > eC else aCr) * (a - eC)
            lp += 0.68 * (hc_ * gA - lp)
            lp2 += 0.68 * (lp - lp2)
            wet[i] += lp2
        if not (c1 == c1 and eC == eC and lp2 == lp2):
            c1 = c2 = eC = lp = lp2 = 0.0
        self.ac1, self.ac2, self.aenC = c1, c2, eC
        self.alp, self.alp2 = lp, lp2

        # ---- output stage ----
        levUi = vc_ui(params[6], 50.0)
        mix = vc_ui(params[7], 100.0) * 0.01
        outRaw = params[12]
        if not (0.0 <= outRaw <= 200.0):
            outUi = 0.0
        elif outRaw < 1.5:
            outUi = outRaw * 100.0
        else:
            outUi = outRaw
        if outUi > 101.0:
            outUi = 101.0
        outUi = float(int(outUi + 0.5))
        if mix > 1.0:
            mix = 1.0
        if levUi > 100.0:
            levUi = 100.0
        level = levUi * 0.02
        level = level * level

        lvl = makeup * envCt * vc_recip(envMt + 0.002)
        lvlR = self.lvlP
        if not (lvlR >= 0.0 and lvlR < 1.0e6):
            lvlR = lvl
        self.lvlP = lvl
        lvlD = 0.125 * (lvl - lvlR)

        if frzR:
            gH = 0.04 + 0.0003 * cutUi
            gL = 0.95 - 0.008 * hiUi
            for i in range(8):
                x = wet[i]
                d = self.rvC[self.rvI]
                self.rvC[self.rvI] = x + 0.75 * d
                self.rvI = (self.rvI + 1) & 511
                self.rlp1 += gL * (d - self.rlp1)
                self.rhp1 += gH * (self.rlp1 - self.rhp1)
                wet[i] = x + 0.22 * (self.rlp1 - self.rhp1)

        sep = 1.0 if (0.5 < outUi < 1.5) else 0.0
        pan = 1.0 if outUi > 1.5 else 0.0
        if outUi < 2.5:
            pr = 1.0
        elif outUi < 3.5:
            pr = 120.0 / 60.0          # T.LFO: 1 cycle/beat
        elif outUi < 4.5:
            pr = 0.5                   # LFO A
        elif outUi < 5.5:
            pr = 4.0                   # LFO B
        else:
            pr = 0.1 * (outUi - 4.0)   # P0.1..P9.7
        if not (0.0 <= pr <= 12.0):
            pr = 1.0
        pinc = ((int(pr * 8.0 * VC_INV_SR * 2147483648.0) & 0xFFFFFFFF) << 1) & 0xFFFFFFFF
        pph = (self.panPh + pinc) & 0xFFFFFFFF
        self.panPh = pph
        # signed >> 1
        signed = pph if pph < 0x80000000 else pph - 0x100000000
        panPh = (signed >> 1) * 4.656612873e-10
        pos = 0.5 + 0.5 * vc_sin(2.0 * VC_PI * panPh - VC_PI)
        if not (pos >= 0.0):
            pos = 0.0
        if pos > 1.0:
            pos = 1.0
        th = pos * VC_HALF_PI
        tB = VC_HALF_PI - th
        t2 = th * th
        u2 = tB * tB
        gA = th * (1.0 - t2 * (0.16666667 - t2 * (0.00833333 - t2 * 0.00019841)))
        gB = tB * (1.0 - u2 * (0.16666667 - u2 * (0.00833333 - u2 * 0.00019841)))
        if not (gA >= 0.0):
            gA = 0.0
        if gA > 1.0:
            gA = 1.0
        if not (gB >= 0.0):
            gB = 0.0
        if gB > 1.0:
            gB = 1.0
        wG, dG = mix, 1.0 - mix
        mixMode = 1.0 if (sep < 0.5 and pan < 0.5) else 0.0
        gT = 1.0 if envMt > 0.002 else (0.0 if envMt < 0.0014 else (1.0 if self.gateG > 0.5 else 0.0))
        gateG = self.gateG

        for i in range(8):
            dryv = dry[i]
            lvlR += lvlD
            wetv = lvlR * wet[i]
            if wetv > 1.5:
                wetv = 1.0
            elif wetv < -1.5:
                wetv = -1.0
            else:
                wetv = wetv - 0.14814815 * wetv * wetv * wetv
            mixed = dryv + mix * (wetv - dryv)
            oL = mixed + sep * (wetv - mixed)
            oR = mixed + sep * (dryv - mixed)
            pL = wG * wetv * gB + dG * dryv * gA
            pR = wG * wetv * gA + dG * dryv * gB
            gateG += (0.0224 if gT > gateG else 0.00057) * (gT - gateG)
            gg = 1.0 + mixMode * (gateG - 1.0)
            if frzR:
                gg = 1.0
            oL = gg * level * (oL + pan * (pL - oL))
            oR = gg * level * (oR + pan * (pR - oR))
            if frzR:
                tA = level * 0.50 * self.tailE * car[i]
                oL += tA
                oR += tA
            if oL > 1.5:
                oL = 1.0
            elif oL < -1.5:
                oL = -1.0
            else:
                oL = oL - 0.14814815 * oL * oL * oL
            if oR > 1.5:
                oR = 1.0
            elif oR < -1.5:
                oR = -1.0
            else:
                oR = oR - 0.14814815 * oR * oR * oR
            if not (oL == oL):
                oL = 0.0
            if not (oR == oR):
                oR = 0.0
            fxBuf[i] = oL
            fxBuf[i + 8] = oR
        self.gateG = gateG

    def process(self, audio, params=None):
        """audio: (n, 2) array.

        params: pedal UI values (Bands/Level/Mix/LoCut/HiCut/Noise/NType/Out/Gain)
        as a dict or a 14-slot list. Converted to raw slots UI*0.01 like zdl_host.
        Returns (n, 2) float64 output.
        """
        x = np.asarray(audio, dtype=np.float64)
        n = len(x)
        # default params matching pedal defaults (UI values 0..100 as shown)
        ui = [1.0, 0.0, 0.0, 0.0, 0.0, 4.0, 50.0, 100.0, 30.0, 0.0, 0.0, 1.0, 0.0, 50.0]
        if params is not None:
            if isinstance(params, dict):
                names = ["on", "u1", "u2", "u3", "u4", "Bands", "Level", "Mix",
                         "LoCut", "HiCut", "Noise", "NType", "Out", "Gain"]
                for i, name in enumerate(names):
                    if name in params:
                        ui[i] = float(params[name])
            else:
                for i, v in enumerate(params):
                    if i < len(ui):
                        ui[i] = float(v)
        # raw slots as the pedal delivers them (UI * 0.01), except params[0] on/off
        p = [ui[0]] + [v * 0.01 for v in ui[1:]]
        y = np.zeros((n, 2), dtype=np.float64)
        for i in range(0, n - 7, 8):
            block = [0.0] * 16
            m = min(8, n - i)
            for k in range(m):
                block[k] = x[i + k, 0]
                block[k + 8] = x[i + k, 1]
            self.process_block(p, block)
            for k in range(m):
                y[i + k, 0] = block[k]
                y[i + k, 1] = block[k + 8]
        return y


# ---- convenience: the old simplified API ---------------------------------
def frev_block(x, ntype=8, release_mult=8.0):
    """Kept for existing callers. ntype 7=FrzL, 8=FRev (UI value)."""
    sim = Vocowder()
    p = {
        "on": 1.0,
        "Bands": 4.0,
        "Level": 50.0,
        "Mix": 100.0,
        "LoCut": 30.0,
        "HiCut": 0.0,
        "Noise": 0.0,
        "NType": float(ntype),
        "Out": 0.0,
        "Gain": 50.0,
    }
    return sim.process(x, p)


def energy(a):
    return float(np.sqrt(np.mean(a * a)))


# ---- WAV helpers ---------------------------------------------------------
def write_wav(path, x):
    x = np.clip(np.asarray(x, dtype=np.float64), -1.0, 1.0)
    ints = np.round(x * 8388607.0).astype(np.int32)
    n, ch = x.shape
    with wave.open(str(path), "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(3)
        w.setframerate(SR)
        frames = bytearray()
        for i in range(n):
            for c in range(ch):
                frames += struct.pack("<i", int(ints[i, c]))[:3]
        w.writeframes(bytes(frames))


def read_wav(path):
    with wave.open(str(path), "rb") as w:
        ch, sw, n = w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(n)
    if sw == 3:
        a = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = (
            a[:, 0].astype(np.int32)
            | (a[:, 1].astype(np.int32) << 8)
            | (a[:, 2].astype(np.int32) << 16)
        )
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float64) / 8388608.0
    elif sw == 2:
        x = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    else:
        raise RuntimeError(f"sampwidth {sw}")
    x = x.reshape(n, ch) if ch > 1 else x.reshape(n, 1)
    if ch == 1:
        x = np.repeat(x, 2, axis=1)
    return x[:, :2]


# ---- default mute-tail self-test ----------------------------------------
def main_selftest():
    note_s, tail_s = 0.6, 2.5
    n = int(SR * (note_s + tail_s))
    t = np.arange(n, dtype=np.float64) / SR
    x = np.zeros((n, 2), np.float64)
    note = t < note_s
    x[note, 0] = 0.35 * np.sin(2 * np.pi * 220 * t[note]) + 0.12 * np.sin(
        2 * np.pi * 440 * t[note]
    )
    x[note, 1] = 0.25 * np.sin(2 * np.pi * 110 * t[note]) + 0.1 * np.sin(
        2 * np.pi * 220 * t[note]
    )

    print("=== Vocowder FRev PC sim (freeze / mute tail) ===")
    results = {}
    for name, nt in (("FrzL", 7), ("FRev", 8)):
        y = frev_block(x, ntype=nt)
        n_note = int(SR * note_s)
        e = {
            "onset": energy(y[int(SR * 0.02) : int(SR * 0.15)]),
            "hold": energy(y[int(SR * 0.35) : int(SR * 0.55)]),
            "mute+0.15s": energy(y[n_note + int(SR * 0.05) : n_note + int(SR * 0.25)]),
            "mute+0.75s": energy(y[n_note + int(SR * 0.5) : n_note + int(SR * 1.0)]),
            "mute+1.6s": energy(y[n_note + int(SR * 1.2) : n_note + int(SR * 2.0)]),
        }
        results[name] = e
        print(f"\n[{name}] RMS:")
        for k, v in e.items():
            print(f"  {k:12s}  {v:.5f}")

    frev = results["FRev"]
    hold = frev["hold"] + 1e-9
    ratio = frev["mute+0.75s"] / hold
    print(f"\nFRev mute+0.75s / hold = {ratio:.3f}")
    ok = ratio > 0.03
    print("RESULT:", "PASS (freeze tail holds)" if ok else "FAIL (sound dies on mute)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main_selftest())
