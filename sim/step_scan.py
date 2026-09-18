"""细扫谐波抵消器步长/泄漏，并测收敛速度"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np
import anc_sim as A

FS = A.FS


def trial(step, delay_ms=6.0, dur=6.0, leak=0.0, rise=1.5):
    h = A.build_secondary_path(delay_ms=delay_ms, spk_hp_fc=200.0)
    shat = h.copy()
    D = int(round(delay_ms * 1e-3 * FS))
    shat[:max(1, D - 4)] = 0.0
    B = max(32, min(D, 512))
    n = int(dur * FS)
    d = A.make_noise('tonal', n, amp=0.05, f0=120.0)
    ctrl = A.Controller(shat.copy(), wb_on=False, n_harm=5,
                        tonal_step=step, tonal_leak=leak)
    ctrl.set_f0(120.0)
    plant = A.Plant(h, B)
    e = np.zeros(n); y = np.zeros(n)
    for n0 in range(0, n, B):
        n1 = min(n0 + B, n)
        eb = d[n0:n1] + plant.pending()[:n1 - n0]
        for i in range(n0, n1):
            e[i] = eb[i - n0]
            y[i] = ctrl.process(e[i])
        plant.feed(y[n0:n1])
    t = int(1.5 * FS)
    nr_end = 10 * np.log10(np.mean(d[-t:] ** 2) / max(np.mean(e[-t:] ** 2), 1e-20))
    yr = np.sqrt(np.mean(y[-t:] ** 2))
    wmax = max(max(abs(v) for v in ctrl.wc), max(abs(v) for v in ctrl.ws))
    # 首次达 20dB 的时间
    t20 = None
    seg = int(0.1 * FS)
    for s in range(0, n - seg, seg):
        r = 10 * np.log10(np.mean(d[s:s + seg] ** 2) /
                          max(np.mean(e[s:s + seg] ** 2), 1e-20))
        if r > 20.0:
            t20 = s / FS
            break
    return nr_end, yr, wmax, t20


print("=== A. 步长细扫 (延迟 6ms, 无泄漏) ===")
print(f"{'step':>8}{'NR(1.5s尾)':>12}{'drive':>9}{'|w|max':>9}{'t20(s)':>9}")
for step in [0.001, 0.002, 0.003, 0.004, 0.005, 0.006, 0.008, 0.01]:
    nr, yr, wm, t20 = trial(step)
    flag = " 发散" if yr > 0.5 else ""
    print(f"{step:>8.4f}{nr:>12.2f}{yr:>9.4f}{wm:>9.4f}"
          f"{(t20 if t20 else -1):>9.2f}{flag}")

print()
print("=== B. 各延迟下的安全步长 (取 0.5 * 临界) ===")
print(f"{'延迟(ms)':>10}{'D':>6}{'step':>9}{'NR':>9}{'t20(s)':>9}{'drive':>9}")
for dl in [1.0, 2.0, 4.0, 6.0, 10.0, 20.0]:
    best = None
    for step in [0.0005, 0.001, 0.002, 0.003, 0.004, 0.005, 0.007, 0.01, 0.015]:
        nr, yr, wm, t20 = trial(step, delay_ms=dl)
        if yr > 0.5:
            break
        best = (step, nr, t20)
    if best:
        D = int(round(dl * 1e-3 * FS))
        print(f"{dl:>10.1f}{D:>6}{best[0]:>9.4f}{best[1]:>9.2f}"
              f"{(best[2] if best[2] else -1):>9.2f}")

print()
print("=== C. 加泄漏是否允许更大步长 (延迟 6ms) ===")
print(f"{'step':>8}{'leak':>10}{'NR':>9}{'drive':>9}{'t20(s)':>9}")
for step in [0.005, 0.01, 0.02, 0.04]:
    for leak in [0.0, 1e-5, 1e-4]:
        nr, yr, wm, t20 = trial(step, leak=leak)
        flag = " 发散" if yr > 0.5 else ""
        print(f"{step:>8.4f}{leak:>10.0e}{nr:>9.2f}{yr:>9.4f}"
              f"{(t20 if t20 else -1):>9.2f}{flag}")
