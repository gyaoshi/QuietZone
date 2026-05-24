#!/usr/bin/env python3
"""
QuietZone FxLMS 主动降噪算法仿真验证 v3

核心修正:
  - 滤波参考向量 x_hat_vec = s2_hat * x_buf (完整卷积)
  - 固定步长 + leaky, 不过度归一化
  - e(n) = d(n) + s2*y(n) 标准定义

运行: python3 fxlms_simulation.py
"""

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import os

plt.rcParams['axes.unicode_minus'] = False


def fxlms_feedforward(x, d, s2, s2_hat, L=128, mu=0.01, leaky=0.9999):
    """Feedforward FxLMS — 标准实现"""
    N = len(x)
    M = len(s2)

    w = np.zeros(L)
    x_buf = np.zeros(L)
    s2_buf = np.zeros(M)
    s2h_buf = np.zeros(len(s2_hat))

    e = np.zeros(N)
    y = np.zeros(N)

    for n in range(N):
        x_buf = np.roll(x_buf, 1)
        x_buf[0] = x[n]

        # 滤波器输出
        y_n = np.dot(w, x_buf)
        y_n = np.clip(y_n, -5.0, 5.0)
        y[n] = y_n

        # 经次级路径
        s2_buf = np.roll(s2_buf, 1)
        s2_buf[0] = y_n
        y_prime = np.dot(s2, s2_buf)

        # 误差
        e_n = d[n] + y_prime
        e[n] = e_n

        # 滤波参考 (标量): x 经 s2_hat → x_hat_n
        s2h_buf = np.roll(s2h_buf, 1)
        s2h_buf[0] = x[n]
        x_hat_n = np.dot(s2_hat, s2h_buf)  # 标量

        # FxLMS 更新: w(n+1) = w(n) + mu * x_hat(n) * e(n) * x_buf
        w = leaky * w + mu * x_hat_n * e_n * x_buf

    return e, d, y


def fxlms_feedback(d, s2, s2_hat, L=128, mu=0.003, leaky=0.9999):
    """Feedback FxLMS — 从误差信号估计原始噪声 (v2 修正版)

    关键修正:
      - 正确模拟麦克风信号: mic(n) = d(n) + S₂·anti(n)
        其中 anti(n) = -y(n) 是反噪声, 经真实次级路径到达麦克风
      - 使用 prev_anti 计算 Ŝ·anti(n-1), 避免 y(n) 的代数环路
      - s2_buf 仅包含 previous anti-noise, 保证因果性
      - 匹配 C++ processFeedback 的逻辑流

    算法流程 (与 C++ 实现一致):
      1. mic(n) = d(n) + S₂·anti(n)        [模拟麦克风信号]
      2. e(n) = mic(n)                      [误差=麦克风残差]
      3. s_hat_y = Ŝ·anti(n-1)             [前一帧反噪声经次级路径估计]
      4. d̂(n) = e(n) - s_hat_y             [原始噪声估计]
      5. y(n) = W·d̂(n)                     [自适应滤波器输出]
      6. anti(n) = -y(n)                    [反噪声]
      7. x̂(n) = Ŝ·d̂(n)                    [标量滤波参考]
      8. W ← leaky·W + μ·x̂(n)·e(n)·d̂_buf  [权重更新]

    适合: 周期性/窄带噪声 (引擎、风扇)
    """
    N = len(d)
    M = len(s2)
    M_hat = len(s2_hat)

    w = np.zeros(L)
    d_hat_buf = np.zeros(L)

    # 真实次级路径缓冲 (模拟扬声器→麦克风)
    # 只存 previous anti-noise, 保证因果性
    s2_buf = np.zeros(M)

    # 估计次级路径缓冲
    s2h_buf_anti = np.zeros(M_hat)   # Ŝ·anti(n-1) 缓冲
    s2h_buf_dhat = np.zeros(M_hat)   # Ŝ·d̂(n) 缓冲

    e = np.zeros(N)
    anti_arr = np.zeros(N)
    max_w = 10.0
    prev_anti = 0.0

    for n in range(N):
        # 1. 模拟麦克风信号: mic(n) = d(n) + S₂·anti(n)
        #    s2_buf = [anti(n-1), anti(n-2), ...] (上一帧已更新)
        #    anti_prime = s2[0]*anti(n-1) + s2[1]*anti(n-2) + ...
        #    注意: 这是 (S₂*anti)(n) 的近似 (略去 s2[0]*anti(n) 项)
        #    在真实系统中 s2[0]≈0 (物理延迟), 所以近似成立
        anti_prime = np.dot(s2, s2_buf)
        mic_n = d[n] + anti_prime

        # 2. 误差信号 = 麦克风信号
        e_n = mic_n
        e[n] = e_n

        # 3. 估计前一帧反噪声的次级路径贡献: Ŝ·anti(n-1)
        s2h_buf_anti = np.roll(s2h_buf_anti, 1)
        s2h_buf_anti[0] = prev_anti
        s_hat_anti = np.dot(s2_hat, s2h_buf_anti)

        # 4. 估计原始噪声: d̂(n) = e(n) - Ŝ·anti(n-1)
        d_hat = e_n - s_hat_anti
        d_hat = np.clip(d_hat, -1.0, 1.0)

        # 5. 自适应滤波器输出: y(n) = W·d̂_buf
        d_hat_buf = np.roll(d_hat_buf, 1)
        d_hat_buf[0] = d_hat
        y_n = np.clip(np.dot(w, d_hat_buf), -5.0, 5.0)

        # 6. 反噪声: anti(n) = -y(n)
        anti_n = -y_n
        anti_arr[n] = anti_n
        prev_anti = anti_n

        # 7. 更新真实次级路径缓冲 (供下一帧模拟 mic 用)
        s2_buf = np.roll(s2_buf, 1)
        s2_buf[0] = anti_n

        # 8. 标量滤波参考: x̂(n) = Ŝ·d̂(n)
        s2h_buf_dhat = np.roll(s2h_buf_dhat, 1)
        s2h_buf_dhat[0] = d_hat
        x_hat_n = np.dot(s2_hat, s2h_buf_dhat)

        # 9. FxLMS 权重更新
        w = leaky * w + mu * x_hat_n * e_n * d_hat_buf
        w = np.clip(w, -max_w, max_w)

    return e, d


def fxlms_hybrid(x, d, s2, s2_hat, L=128, mu_ff=0.005, mu_fb=0.003, leaky=0.9999):
    """Hybrid ANC = Feedforward + Feedback (解耦版)

    解耦设计 (与 C++ 实现一致):
      - 反馈分支只使用前馈贡献 Ŝ·y_ff(n) 来估计 d_hat
      - 避免循环依赖: y(n) = y_ff + y_fb, 但 d_hat 不依赖 y_fb
      - d_hat = e(n) - Ŝ·y_ff(n)  (只减前馈贡献)

    逐样本处理流程:
      1. y_ff = W_ff · x_buf           [前馈输出]
      2. e_approx = x_n + anti_ff      [前馈残差]
      3. s_hat_y_ff = Ŝ · anti_ff      [前馈输出经次级路径]
      4. d_hat = e_approx - s_hat_y_ff [窄带残余估计]
      5. y_fb = W_fb · d_hat_buf       [反馈输出]
      6. y_n = y_ff + y_fb             [混合输出]
      7. e_n = d_n + S₂ · y_n         [真实误差]
      8. x̂_ff = Ŝ · x_n              [前馈标量滤波参考]
      9. x̂_fb = Ŝ · d_hat            [反馈标量滤波参考]
      10. W_ff, W_fb 更新
    """
    N = len(x)
    M = len(s2)

    w_ff = np.zeros(L)
    w_fb = np.zeros(L)
    x_buf = np.zeros(L)
    d_hat_buf = np.zeros(L)
    s2_buf = np.zeros(M)
    s2h_buf_x = np.zeros(len(s2_hat))    # Ŝ·x(n) 缓冲
    s2h_buf_dhat = np.zeros(len(s2_hat))  # Ŝ·d_hat(n) 缓冲
    s2h_buf_out = np.zeros(len(s2_hat))   # Ŝ·output 缓冲 (用于反馈参考)
    y_out = np.zeros(N)

    e = np.zeros(N)
    max_w = 10.0  # 权重限幅

    for n in range(N):
        x_n = x[n]

        # ── 前馈分支 ──
        x_buf = np.roll(x_buf, 1)
        x_buf[0] = x_n
        y_ff = np.clip(np.dot(w_ff, x_buf), -5.0, 5.0)

        # ── 反馈分支: 估计窄带残余 ──
        # 使用前馈输出通过次级路径估计
        anti_ff = -y_ff  # 前馈反噪声
        s2h_buf_out = np.roll(s2h_buf_out, 1)
        s2h_buf_out[0] = anti_ff
        s_hat_y_ff = np.dot(s2_hat, s2h_buf_out)

        # 前馈残差 (近似的 e_ff)
        e_ff_approx = x_n + anti_ff
        e_ff_approx = np.clip(e_ff_approx, -1.0, 1.0)

        # 原始噪声估计 (只减前馈贡献)
        d_hat = e_ff_approx - s_hat_y_ff
        d_hat = np.clip(d_hat, -1.0, 1.0)

        d_hat_buf = np.roll(d_hat_buf, 1)
        d_hat_buf[0] = d_hat
        y_fb = np.clip(np.dot(w_fb, d_hat_buf), -5.0, 5.0)

        # ── 混合输出 ──
        y_n = y_ff + y_fb
        y_out[n] = y_n

        # 经真实次级路径
        s2_buf = np.roll(s2_buf, 1)
        s2_buf[0] = y_n
        y_prime = np.dot(s2, s2_buf)

        # 真实误差
        e_n = d[n] + y_prime
        e[n] = e_n

        # ── 前馈 FxLMS 更新 ──
        s2h_buf_x = np.roll(s2h_buf_x, 1)
        s2h_buf_x[0] = x_n
        x_hat_ff = np.dot(s2_hat, s2h_buf_x)  # 标量

        w_ff = leaky * w_ff + mu_ff * x_hat_ff * e_n * x_buf
        w_ff = np.clip(w_ff, -max_w, max_w)  # 权重限幅

        # ── 反馈 FxLMS 更新 ──
        s2h_buf_dhat = np.roll(s2h_buf_dhat, 1)
        s2h_buf_dhat[0] = d_hat
        x_hat_fb = np.dot(s2_hat, s2h_buf_dhat)  # 标量

        w_fb = leaky * w_fb + mu_fb * x_hat_fb * e_n * d_hat_buf
        w_fb = np.clip(w_fb, -max_w, max_w)  # 权重限幅

    return e, d


def compute_nr(d, e, fs, tail_sec=1.0):
    tail = int(fs * tail_sec)
    p_d = np.mean(d[-tail:]**2)
    p_e = np.mean(e[-tail:]**2)
    if p_d < 1e-20 or p_e < 1e-20:
        return 0.0
    return 10 * np.log10(p_d / p_e)


def nr_envelope(d, e, fs, win_ms=100):
    N = len(d)
    win = int(fs * win_ms / 1000)
    nr = np.zeros(N)
    for i in range(win, N, win):
        pd = np.mean(d[max(0,i-win):i]**2)
        pe = np.mean(e[i:min(i+win,N)]**2)
        if pd > 1e-20 and pe > 1e-20:
            nr[i:i+win] = 10 * np.log10(pd / pe)
    return nr


def run():
    fs = 8000
    T = 10.0
    N = int(fs * T)
    t = np.arange(N) / fs

    P = np.array([0.5, 0.3, 0.2, 0.1, 0.05])
    S = np.array([0.1, 0.3, 0.4, 0.2])
    S_hat = S.copy()

    res = {}

    # 场景1
    print("场景1: 200Hz 纯音 — Feedforward")
    x = 0.8 * np.sin(2*np.pi*200*t)
    d = np.convolve(x, P)[:N]
    e, _, y = fxlms_feedforward(x, d, S, S_hat, L=128, mu=0.005)
    nr = compute_nr(d, e, fs)
    print(f"  降噪量: {nr:.1f} dB")
    res['s1'] = (d, e, y, nr, t)

    # 场景2
    print("场景2: 500Hz 纯音 — Feedforward")
    x = 0.8 * np.sin(2*np.pi*500*t)
    d = np.convolve(x, P)[:N]
    e, _, y = fxlms_feedforward(x, d, S, S_hat, L=128, mu=0.005)
    nr = compute_nr(d, e, fs)
    print(f"  降噪量: {nr:.1f} dB")
    res['s2'] = (d, e, y, nr, t)

    # 场景3
    print("场景3: 200Hz 纯音 — Feedback (窄带)")
    d3 = np.convolve(0.8*np.sin(2*np.pi*200*t), P)[:N]
    e3, _ = fxlms_feedback(d3, S, S_hat, L=128, mu=0.003)
    nr3 = compute_nr(d3, e3, fs)
    print(f"  降噪量: {nr3:.1f} dB")
    res['s3'] = (d3, e3, None, nr3, t)

    # 场景4
    print("场景4: 混合噪声 — Hybrid")
    np.random.seed(42)
    x = 0.5*np.sin(2*np.pi*200*t) + 0.3*np.sin(2*np.pi*500*t) + 0.1*np.random.randn(N)
    d = np.convolve(x, P)[:N]
    e, _ = fxlms_hybrid(x, d, S, S_hat, L=128, mu_ff=0.005, mu_fb=0.003)
    nr = compute_nr(d, e, fs)
    print(f"  降噪量: {nr:.1f} dB")
    res['s4'] = (d, e, None, nr, t)

    # 场景5
    print("场景5: 非平稳噪声 — Hybrid")
    x = np.zeros(N)
    half = N // 2
    x[:half] = 0.8*np.sin(2*np.pi*200*t[:half])
    x[half:] = 0.8*np.sin(2*np.pi*500*t[half:])
    d = np.convolve(x, P)[:N]
    e, _ = fxlms_hybrid(x, d, S, S_hat, L=128, mu_ff=0.005, mu_fb=0.003)
    nr = compute_nr(d, e, fs)
    nr_env = nr_envelope(d, e, fs)
    conv_ms = 0
    for i in range(half, N, fs//100):
        if i < len(nr_env) and nr_env[i] > 3.0:
            conv_ms = (i - half) / fs * 1000
            break
    print(f"  收敛时间: {conv_ms:.0f} ms, 降噪量: {nr:.1f} dB")
    res['s5'] = (d, e, None, nr, t, conv_ms, nr_env)

    # 场景6
    print("场景6: 次级路径估计误差 ±20% — Feedforward")
    x = 0.8*np.sin(2*np.pi*200*t)
    d = np.convolve(x, P)[:N]
    S_noisy = S * (1.0 + 0.2*np.array([1, -1, 0.5, -0.5]))
    e_err, _, _ = fxlms_feedforward(x, d, S, S_noisy, L=128, mu=0.005)
    e_perf, _, _ = fxlms_feedforward(x, d, S, S_hat, L=128, mu=0.005)
    nr_err = compute_nr(d, e_err, fs)
    nr_perf = compute_nr(d, e_perf, fs)
    print(f"  完美: {nr_perf:.1f} dB, 20%误差: {nr_err:.1f} dB, 损失: {nr_perf-nr_err:.1f} dB")
    res['s6'] = (d, e_err, e_perf, nr_err, t, nr_perf)

    return res, fs


def plot(res, fs):
    fig, axes = plt.subplots(4, 2, figsize=(16, 16))
    fig.suptitle('QuietZone FxLMS ANC Simulation v3 (Scalar x̂(n))', fontsize=16, fontweight='bold')

    # S1: 200Hz Feedforward
    d, e, y, nr, t = res['s1']
    ax = axes[0,0]
    ax.plot(t[:fs], d[:fs], 'b', alpha=0.5, label='d(n)')
    ax.plot(t[:fs], e[:fs], 'r', alpha=0.7, label=f'e(n) NR={nr:.1f}dB')
    ax.set_title('S1: 200Hz — Feedforward'); ax.legend(fontsize=8); ax.grid(True, alpha=0.3)

    # S2: 500Hz Feedforward
    d, e, y, nr, t = res['s2']
    ax = axes[0,1]
    ax.plot(t[:fs], d[:fs], 'b', alpha=0.5, label='d(n)')
    ax.plot(t[:fs], e[:fs], 'r', alpha=0.7, label=f'e(n) NR={nr:.1f}dB')
    ax.set_title('S2: 500Hz — Feedforward'); ax.legend(fontsize=8); ax.grid(True, alpha=0.3)

    # S3: 200Hz Feedback
    d, e, _, nr, t = res['s3']
    ax = axes[1,0]
    env = nr_envelope(d, e, fs)
    ax.plot(t, env, 'purple', lw=1.5)
    ax.axhline(nr, color='r', ls='--', label=f'Steady {nr:.1f}dB')
    ax.axhline(3, color='orange', ls=':', label='3dB')
    ax.set_title('S3: 200Hz — Feedback'); ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
    ax.set_ylim([-5, 35])

    # S4: Mixed Hybrid
    d, e, _, nr, t = res['s4']
    ax = axes[1,1]
    env = nr_envelope(d, e, fs)
    ax.plot(t, env, 'g', lw=1.5)
    ax.axhline(nr, color='r', ls='--', label=f'Steady {nr:.1f}dB')
    ax.axhline(3, color='orange', ls=':', label='3dB')
    ax.set_title('S4: Mixed — Hybrid'); ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
    ax.set_ylim([-5, 35])

    # S5: Non-stationary Hybrid
    d, e, _, nr, t, conv, env = res['s5']
    ax = axes[2,0]
    ax.plot(t, env, 'purple', lw=1.5)
    ax.axvline(5.0, color='r', ls='--', label='Switch')
    ax.axhline(3, color='orange', ls=':', label='3dB')
    ax.set_title(f'S5: Non-stationary Conv={conv:.0f}ms'); ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
    ax.set_ylim([-5, 35])

    # S6: SP Estimation Error
    d, e_err, e_perf, nr_err, t, nr_perf = res['s6']
    ax = axes[2,1]
    env_p = nr_envelope(d, e_perf, fs)
    env_e = nr_envelope(d, e_err, fs)
    ax.plot(t, env_p, 'g', lw=1.5, label=f'Perfect {nr_perf:.1f}dB')
    ax.plot(t, env_e, 'r', lw=1.5, alpha=0.7, label=f'20%err {nr_err:.1f}dB')
    ax.set_title('S6: SP Estimation Error'); ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
    ax.set_ylim([-5, 35])

    # Summary Table
    ax = axes[3,0]; ax.axis('off')
    rows = [
        ['Scene','Mode','Noise','NR(dB)','Conv(ms)'],
        ['1','FF','200Hz',f'{res["s1"][3]:.1f}','-'],
        ['2','FF','500Hz',f'{res["s2"][3]:.1f}','-'],
        ['3','FB','200Hz',f'{res["s3"][3]:.1f}','-'],
        ['4','Hybrid','Mixed',f'{res["s4"][3]:.1f}','-'],
        ['5','Hybrid','Non-stat',f'{res["s5"][3]:.1f}',f'{res["s5"][5]:.0f}'],
        ['6a','FF perf','200Hz',f'{res["s6"][5]:.1f}','-'],
        ['6b','FF 20%err','200Hz',f'{res["s6"][3]:.1f}','-'],
    ]
    tab = ax.table(cellText=rows[1:], colLabels=rows[0], loc='center', cellLoc='center')
    tab.auto_set_font_size(False); tab.set_fontsize(9); tab.scale(1.2, 1.5)
    for k, c in tab.get_celld().items():
        if k[0]==0: c.set_facecolor('#00E5FF'); c.set_text_props(fontweight='bold', color='black')
    ax.set_title('Summary', fontweight='bold')

    # Algorithm description
    ax = axes[3,1]; ax.axis('off')
    desc = (
        "FxLMS Scalar Update (v3)\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "y(n) = w^T · x_buf\n"
        "e(n) = d(n) + S₂ · y(n)\n"
        "x̂(n) = Ŝ · x(n)   [SCALAR]\n"
        "w ← leaky·w + μ·x̂(n)·e(n)·x_buf\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "Key Fix: x̂(n) is a scalar\n"
        "multiplying entire x_buf vector\n"
        "NOT element-wise x̂_vec[i]·e(n)"
    )
    ax.text(0.1, 0.5, desc, transform=ax.transAxes, fontsize=10,
            verticalalignment='center', fontfamily='monospace',
            bbox=dict(boxstyle='round', facecolor='#1a1a2e', edgecolor='#00E5FF', alpha=0.9),
            color='#00E5FF')

    plt.tight_layout()
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fxlms_simulation_results.png')
    plt.savefig(path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"图表: {path}")


if __name__ == '__main__':
    print("QuietZone FxLMS 仿真验证 v3\n" + "="*60)
    res, fs = run()
    plot(res, fs)
    print("\n✅ 仿真完成!")
