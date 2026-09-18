"""验证即将写入 C++ 的步长规则: tonalStep = clamp(1.0/D, 2e-4, 2e-3)

对每个回环延迟 D, 扫描步长, 找出实际临界值, 并检查规则取值是否安全且有效。
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np
import anc_sim as A

FS = A.FS


def trial(step, delay_ms, dur=5.0, leak=0.0):
    h = A.build_secondary_path(delay_ms=delay_ms, spk_hp_fc=200.0)
    D = int(round(delay_ms * 1e-3 * FS))
    shat = h.copy()
    shat[:max(1, D - 4)] = 0.0
    B = max(32, min(D, 512))
    n = int(dur * FS)
    d = A.make_noise('tonal', n, amp=0.05, f0=120.0)
    ctrl = A.Controller(shat.copy(), wb_on=False, n_harm=5,
                        tonal_step=step, tonal_leak=leak)
    ctrl.set_f0(120.0)
    plant = A.Plant(h, B)
    e = np.zeros(n)
    y = np.zeros(n)
    for n0 in range(0, n, B):
        n1 = min(n0 + B, n)
        eb = d[n0:n1] + plant.pending()[:n1 - n0]
        for i in range(n0, n1):
            e[i] = eb[i - n0]
            y[i] = ctrl.process(e[i])
        plant.feed(y[n0:n1])
    t = int(1.0 * FS)
    nr_end = 10 * np.log10(np.mean(d[-t:] ** 2) / max(np.mean(e[-t:] ** 2), 1e-20))
    yr = np.sqrt(np.mean(y[-t:] ** 2))
    t20 = None
    seg = int(0.1 * FS)
    for s in range(0, n - seg, seg):
        r = 10 * np.log10(np.mean(d[s:s + seg] ** 2) /
                          max(np.mean(e[s:s + seg] ** 2), 1e-20))
        if r > 20.0:
            t20 = s / FS
            break
    return nr_end, yr, t20


def rule(D):
    return min(max(1.0 / D, 2e-4), 2e-3)


print("=== 每个延迟下扫描实际临界步长; 并标出规则取值 ===")
steps = [0.0005, 0.001, 0.0015, 0.002, 0.003, 0.004, 0.005, 0.007, 0.010, 0.015]
print(f"{'延迟ms':>7}{'D':>6}{'step':>9}{'NR':>9}{'drive':>9}{'t20':>7}  标记")
for dl in [2.0, 3.0, 6.0, 10.0, 15.0]:
    D = int(round(dl * 1e-3 * FS))
    rs = rule(D)
    last_ok = None
    for st in steps:
        nr, yr, t20 = trial(st, dl)
        flag = "发散" if yr > 0.5 else ""
        mark = "  <== 规则" if abs(st - rs) < 1e-9 else ""
        print(f"{dl:>7.1f}{D:>6}{st:>9.4f}{nr:>9.2f}{yr:>9.4f}"
              f"{(t20 if t20 else -1):>7.2f}  {flag}{mark}")
        if yr <= 0.5:
            last_ok = st
    # 规则值本身单独跑一次 (若不在扫描点上)
    if all(abs(st - rs) > 1e-9 for st in steps):
        nr, yr, t20 = trial(rs, dl)
        print(f"{dl:>7.1f}{D:>6}{rs:>9.4f}{nr:>9.2f}{yr:>9.4f}"
              f"{(t20 if t20 else -1):>7.2f}  {'发散' if yr > 0.5 else ''}  <== 规则")
    print(f"        最大安全扫描步长 = {last_ok}  规则={rs:.5f}  "
          f"裕度={last_ok / rs if last_ok else 0:.2f}x")
    print()
