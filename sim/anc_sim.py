"""
QuietZone ANC - 单麦克风可行性仿真 (v2)

在移植到 C++ 之前，用贴近真实手机声学的模型验证控制结构。

与旧 fxlms_simulation.py 的关键区别:
  * 旧仿真的"次级路径"是单位增益零延迟标量 s2 —— 这是把算法吹成 44dB 的根源。
    真实手机扬声器->麦克风回环包含 DAC/ADC 缓冲延迟 + 声传播 + 扬声器低频滚降，
    典型回环延迟 3~20 ms，且 200 Hz 以下输出能力很弱。本模型显式建模这两点。

被验证的结构 (单麦克风唯一物理可行的结构):
  A. 窄带谐波抵消器 - 参考信号由内部振荡器合成，不依赖麦克风。
     对纯音做抵消时"未来相位"已知，因此不受回环延迟的因果性限制。
     这是手机上真正能出效果的部分(风扇嗡鸣/发动机低频轰鸣/交流声)。
  B. 低频宽带反馈 FxLMS (internal model control) - d̂ = e - Ŝ*y，
     再用 Ŝ 滤波后的 d̂ 做自适应。带宽受回环延迟限制。

性能: 延迟线用双缓冲保证"最近 L 样本"是连续正序切片，FIR 退化为 numpy dot。
"""

import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np

FS = 48000


# ============================================================
# 基础工具
# ============================================================
def lfilter(b, a, x):
    b = np.asarray(b, np.float64) / a[0]
    a = np.asarray(a, np.float64) / a[0]
    y = np.zeros_like(x)
    for i in range(len(x)):
        acc = 0.0
        for j in range(len(b)):
            if i - j >= 0:
                acc += b[j] * x[i - j]
        for j in range(1, len(a)):
            if i - j >= 0:
                acc -= a[j] * y[i - j]
        y[i] = acc
    return y


def biquad_coeffs(kind, fc, fs=FS, q=0.7071):
    K = np.tan(np.pi * fc / fs)
    nrm = 1.0 / (1.0 + K / q + K * K)
    b = np.array([nrm, -2 * nrm, nrm]) if kind == 'hp' else \
        np.array([K * K * nrm, 2 * K * K * nrm, K * K * nrm])
    a = np.array([1.0, 2.0 * (K * K - 1.0) * nrm, (1.0 - K / q + K * K) * nrm])
    return b, a


class Biquad:
    """二阶节 (状态用 Python 标量，避免 numpy 切片拷贝的问题)"""

    def __init__(self, kind, fc, fs=FS, q=0.7071):
        b, a = biquad_coeffs(kind, fc, fs, q)
        self.b0, self.b1, self.b2 = b
        self.a1, self.a2 = a[1], a[2]
        self.z1 = self.z2 = 0.0

    def process(self, x):
        y = self.b0 * x + self.z1
        self.z1 = self.b1 * x - self.a1 * y + self.z2
        self.z2 = self.b2 * x - self.a2 * y
        return y

    def gain(self, f, fs=FS):
        z = np.exp(-1j * 2 * np.pi * f / fs)
        num = self.b0 + self.b1 * z + self.b2 * z * z
        den = 1.0 + self.a1 * z + self.a2 * z * z
        return num / den


class DelayLine:
    """
    双缓冲延迟线: buf 长度 2L，每个样本写入 p 和 p-L 两处，
    因此 buf[p-L+1 : p+1] 恒为最近 L 个样本的正序切片。
    """

    def __init__(self, L):
        self.L = L
        self.buf = np.zeros(2 * L, dtype=np.float32)
        self.p = L - 1

    def push(self, x):
        self.p += 1
        if self.p >= 2 * self.L:
            self.p = self.L
        self.buf[self.p] = x
        self.buf[self.p - self.L] = x
        return self.buf[self.p - self.L + 1: self.p + 1]

    def reset(self):
        self.buf[:] = 0.0
        self.p = self.L - 1


# ============================================================
# 次级路径
# ============================================================
def build_secondary_path(delay_ms=6.0, spk_hp_fc=200.0,
                         spk_res=(1150.0, 6.0, 0.30), gain=1.0,
                         n=1024, fs=FS):
    d = int(round(delay_ms * 1e-3 * fs))
    freqs = np.fft.rfftfreq(n, 1.0 / fs)
    z = np.exp(-1j * 2 * np.pi * freqs / fs)
    b, a = biquad_coeffs('hp', spk_hp_fc, fs)
    H = (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)
    fr, qr, gr = spk_res
    br, ar = biquad_coeffs('hp', fr, fs, qr)
    Hr = (br[0] + br[1] * z + br[2] * z * z) / (ar[0] + ar[1] * z + ar[2] * z * z)
    H = (H + gr * Hr) * np.exp(-1j * 2 * np.pi * freqs * d / fs) * gain
    return np.fft.irfft(H, n=n)


def freq_response(h, f, fs=FS, n=None):
    n = len(h) if n is None else n
    return np.sum(h[:n] * np.exp(-1j * 2 * np.pi * f * np.arange(n) / fs))


class Plant:
    """
    重叠相加 (overlap-add) 实时 FIR: out = h * y

    因果性约束: 块长 B <= 纯延迟 D，使 h[0..B-1]==0，
    因此当前块的 y 不会影响当前块的 e —— 与真实声学的因果性一致。

    用法 (顺序很重要):
        plant_out = plant.pending()   # 取本块的对象输出(只依赖更早的 y)
        e = d + plant_out             # 得到本块误差
        ... 控制器算出 y ...
        plant.feed(y)                 # 把本块 y 卷积进延迟余项
    """

    def __init__(self, h, block):
        self.h = h
        self.M = len(h)
        self.B = block
        self.NFFT = 1
        while self.NFFT < block + self.M - 1:
            self.NFFT *= 2
        self.H = np.fft.rfft(h, self.NFFT)
        self.acc = np.zeros(self.M - 1)

    def pending(self):
        """取出当前块的对象输出，并把延迟余项向前推进一个块"""
        out = self.acc[:self.B].copy()
        self.acc = np.concatenate([self.acc[self.B:], np.zeros(self.B)])
        return out

    def feed(self, y_block):
        """把一块驱动信号卷积进余项"""
        B = len(y_block)
        c = np.fft.irfft(np.fft.rfft(y_block, self.NFFT) * self.H,
                         self.NFFT)[:B + self.M - 1]
        self.acc[:self.M - 1] += c[B:B + self.M - 1]


# ============================================================
# 控制器
# ============================================================
class Controller:
    def __init__(self, shat, fs=FS, band=(30.0, 600.0), n_harm=5,
                 tonal_step=0.005, tonal_leak=0.0, wb_len=256, wb_step=0.05,
                 wb_band=(30.0, 250.0), wb_on=True, out_limit=0.95):
        self.fs = fs
        self.shat = shat
        self.band = band
        self.n_harm = n_harm
        self.tonal_step = tonal_step
        self.tonal_leak = tonal_leak
        self.wb_len = wb_len
        self.wb_step = wb_step
        self.wb_on = wb_on
        self.out_limit = out_limit
        self.n_samp = 0
        self.prev_y = 0.0

        # 延迟线: 需要覆盖 D+K 个样本，只对前 K 个做 dot
        M = len(shat)
        self.line_ref = DelayLine(M)      # Ŝ * d̂
        self.line_out = DelayLine(M)      # Ŝ * y

        # 次级路径系数 (按 dot 顺序存放)
        self.shat_f = np.asarray(shat, dtype=np.float32)

        # 宽带分支
        self.w = np.zeros(wb_len, dtype=np.float32)   # 反向存放
        self.line_xh = DelayLine(wb_len)              # 滤波参考 x̂
        self.line_d = DelayLine(wb_len)               # d̂
        self.d_pow = 1e-6
        self.hp_r = Biquad('hp', wb_band[0], fs)
        self.lp_r = Biquad('lp', wb_band[1], fs)
        self.hp_e = Biquad('hp', wb_band[0], fs)
        self.lp_e = Biquad('lp', wb_band[1], fs)

        # 谐波
        self.hf = []
        self.wc = []
        self.ws = []
        self.sA = []
        self.cosphi = []
        self.sinphi = []
        self.mu = []

    def set_f0(self, f0):
        self.hf, self.wc, self.ws, self.sA, self.cosphi, self.sinphi, self.mu = \
            [], [], [], [], [], [], []
        for k in range(1, self.n_harm + 1):
            f = k * f0
            if f > self.band[1]:
                break
            H = freq_response(self.shat, f, self.fs)
            A = max(abs(H), 1e-3)
            phi = np.angle(H)
            self.hf.append(f)
            self.wc.append(0.0)
            self.ws.append(0.0)
            self.sA.append(A)
            self.cosphi.append(np.cos(phi))
            self.sinphi.append(np.sin(phi))
            self.mu.append(self.tonal_step / (A * A))
        return len(self.hf)

    def process(self, e):
        # ===== A. 窄带谐波抵消 (原始误差直接做相关，无需带通) =====
        y_t = 0.0
        if self.hf:
            n = self.n_samp
            for k in range(len(self.hf)):
                ph = 2 * np.pi * self.hf[k] * n / self.fs
                c = np.cos(ph)
                s = np.sin(ph)
                y_t += self.wc[k] * c + self.ws[k] * s
                cf = self.sA[k] * (c * self.cosphi[k] - s * self.sinphi[k])
                sf = self.sA[k] * (c * self.sinphi[k] + s * self.cosphi[k])
                g = self.mu[k] * e
                if self.tonal_leak > 0.0:
                    lk = 1.0 - self.tonal_leak
                    self.wc[k] = lk * self.wc[k] - g * cf
                    self.ws[k] = lk * self.ws[k] - g * sf
                else:
                    self.wc[k] -= g * cf
                    self.ws[k] -= g * sf
                if self.wc[k] > 40.0:
                    self.wc[k] = 40.0
                elif self.wc[k] < -40.0:
                    self.wc[k] = -40.0
                if self.ws[k] > 40.0:
                    self.ws[k] = 40.0
                elif self.ws[k] < -40.0:
                    self.ws[k] = -40.0

        # ===== B. 低频宽带反馈 (IMC) =====
        y_w = 0.0
        if self.wb_on:
            win_o = self.line_out.push(self.prev_y)
            sy = float(np.dot(self.shat_f, win_o))
            d = e - sy
            if d > 1.0:
                d = 1.0
            elif d < -1.0:
                d = -1.0
            d_bp = self.lp_r.process(self.hp_r.process(d))
            e_bp = self.lp_e.process(self.hp_e.process(e))

            win_d = self.line_d.push(d_bp)
            y_w = float(np.dot(self.w, win_d))

            win_x = self.line_ref.push(d_bp)
            xh = float(np.dot(self.shat_f, win_x[:len(self.shat_f)]))
            win_xh = self.line_xh.push(xh)

            self.d_pow = 0.999 * self.d_pow + 0.001 * (d_bp * d_bp)
            mu = self.wb_step / (self.d_pow + 1e-9)
            if mu > 1.0:
                mu = 1.0
            self.w *= np.float32(0.9999)
            self.w -= np.float32(mu * e_bp) * win_xh

        self.n_samp += 1
        y = y_t + y_w
        if y > self.out_limit:
            y = self.out_limit
        elif y < -self.out_limit:
            y = -self.out_limit
        self.prev_y = y
        return y


# ============================================================
# 噪声源
# ============================================================
def make_noise(kind, n, fs=FS, amp=0.05, f0=120.0, seed=1):
    rng = np.random.default_rng(seed)
    t = np.arange(n) / fs
    if kind == 'tonal':
        s = (np.sin(2 * np.pi * f0 * t) + 0.5 * np.sin(2 * np.pi * 2 * f0 * t)
             + 0.25 * np.sin(2 * np.pi * 3 * f0 * t)) / 1.4
    elif kind == 'rumble':
        b, a = biquad_coeffs('lp', 150.0, fs)
        s = lfilter(b, a, rng.normal(0, 1, n))
        s /= (np.std(s) + 1e-12)
    elif kind == 'mix':
        tone = (np.sin(2 * np.pi * f0 * t) + 0.5 * np.sin(2 * np.pi * 2 * f0 * t)) / 1.12
        b, a = biquad_coeffs('lp', 150.0, fs)
        rum = lfilter(b, a, rng.normal(0, 1, n))
        rum /= (np.std(rum) + 1e-12)
        s = 0.7 * tone + 0.3 * rum
    elif kind == 'white':
        s = rng.normal(0, 1, n)
        s /= (np.std(s) + 1e-12)
    else:
        raise ValueError(kind)
    return (amp * s).astype(np.float64)


# ============================================================
# 闭环
# ============================================================
def run(kind='tonal', f0=120.0, delay_ms=6.0, spk_hp=200.0, dur=12.0,
        amp=0.05, seed=1, wb_on=True, n_harm=5, tonal_step=0.06,
        calib_err=0.02, trace=False, shat_len=None):
    n = int(dur * FS)
    d = make_noise(kind, n, amp=amp, f0=f0, seed=seed)
    h = build_secondary_path(delay_ms=delay_ms, spk_hp_fc=spk_hp)

    M = shat_len if shat_len else len(h)
    rng = np.random.default_rng(seed + 77)
    # 校准误差按冲激响应能量缩放 (按 max|h| 缩放会在频域引入比对象本身还大的误差)
    shat = h.copy()
    shat += calib_err * rng.normal(0, 1, len(h)) * np.sqrt(np.mean(h * h))
    shat[:max(1, int(delay_ms * FS / 1000) - 4)] = 0.0

    D = int(round(delay_ms * 1e-3 * FS))
    B = max(32, min(D, 512))
    ctrl = Controller(shat[:M], wb_on=wb_on, n_harm=n_harm, tonal_step=tonal_step)
    ctrl.set_f0(f0)

    plant = Plant(h, B)
    e = np.zeros(n)
    y = np.zeros(n)
    for n0 in range(0, n, B):
        n1 = min(n0 + B, n)
        eb = d[n0:n1] + plant.pending()[:n1 - n0]
        for i in range(n0, n1):
            e[i] = eb[i - n0]
            y[i] = ctrl.process(e[i])
        plant.feed(y[n0:n1])

    tail = int(3.0 * FS)
    pd = float(np.mean(d[-tail:] ** 2))
    pe = float(np.mean(e[-tail:] ** 2))
    nr = 10 * np.log10(pd / pe) if pe > 0 else 0.0

    win = np.hanning(tail)
    Dsp = np.abs(np.fft.rfft(d[-tail:] * win)) / tail
    Esp = np.abs(np.fft.rfft(e[-tail:] * win)) / tail
    fr = np.fft.rfftfreq(tail, 1 / FS)
    lo = fr < 700
    nr_band = 10 * np.log10(np.sum(Dsp[lo] ** 2) / max(np.sum(Esp[lo] ** 2), 1e-20))

    harm = {}
    for k in range(1, n_harm + 1):
        fk = k * f0
        if fk > 700:
            break
        j = int(np.argmin(np.abs(fr - fk)))
        sl = slice(max(0, j - 8), j + 9)
        gd = np.sqrt(np.sum(Dsp[sl] ** 2))
        ge = np.sqrt(np.sum(Esp[sl] ** 2))
        harm[fk] = 20 * np.log10(max(gd, 1e-12) / max(ge, 1e-12))

    res = dict(nr=nr, nr_band=nr_band,
               y_rms=float(np.sqrt(np.mean(y[-tail:] ** 2))),
               d_rms=float(np.sqrt(pd)), e_rms=float(np.sqrt(pe)),
               harm=harm)
    if trace:
        seg = int(FS)
        res['trace'] = [10 * np.log10(np.mean(d[s:s + seg] ** 2) /
                                      max(np.mean(e[s:s + seg] ** 2), 1e-20))
                        for s in range(0, n - seg + 1, seg)]
    return res


# ============================================================
# 主程序
# ============================================================
if __name__ == '__main__':
    def hr(t):
        print("\n" + "=" * 82)
        print(t)
        print("=" * 82)

    hr("0) 次级路径物理特性 (回环延迟 6ms / 扬声器截止 200Hz)")
    h = build_secondary_path()
    print(f"{'频率(Hz)':>10}{'|S|(dB)':>12}{'相位(度)':>12}")
    for f in [50, 80, 100, 120, 150, 200, 260, 300, 400, 600, 1000]:
        H = freq_response(h, f)
        print(f"{f:>10}{20 * np.log10(max(abs(H), 1e-9)):>12.1f}{np.degrees(np.angle(H)):>12.1f}")

    hr("1) 噪声类型对比 (延迟 6ms, 混合模式)")
    print(f"{'场景':<18}{'总NR':>9}{'<700Hz NR':>11}{'驱动rms':>10}{'噪声rms':>10}{'残余rms':>10}")
    for kind, f0 in [('tonal', 120.0), ('tonal', 200.0), ('rumble', 0.0),
                     ('mix', 120.0), ('white', 0.0)]:
        r = run(kind, f0=f0, wb_on=True)
        print(f"{kind + '/' + str(int(f0)):<18}{r['nr']:>9.2f}{r['nr_band']:>11.2f}"
              f"{r['y_rms']:>10.4f}{r['d_rms']:>10.4f}{r['e_rms']:>10.5f}")

    hr("2) 只开谐波抵消器 (关闭宽带分支)")
    print(f"{'场景':<18}{'总NR':>9}{'<700Hz NR':>11}")
    for kind, f0 in [('tonal', 120.0), ('tonal', 200.0), ('mix', 120.0), ('white', 0.0)]:
        r = run(kind, f0=f0, wb_on=False)
        print(f"{kind + '/' + str(int(f0)):<18}{r['nr']:>9.2f}{r['nr_band']:>11.2f}")

    hr("3) 回环延迟敏感性 (120Hz 三谐波噪声, 混合模式)")
    print(f"{'延迟(ms)':>10}{'总NR':>10}{'<700Hz NR':>11}{'驱动rms':>10}")
    for dl in [1.0, 2.0, 4.0, 6.0, 10.0, 20.0, 40.0]:
        r = run('tonal', f0=120.0, delay_ms=dl, wb_on=True)
        print(f"{dl:>10.1f}{r['nr']:>10.2f}{r['nr_band']:>11.2f}{r['y_rms']:>10.4f}")

    hr("3b) 回环延迟敏感性 (只开谐波抵消器)")
    print(f"{'延迟(ms)':>10}{'总NR':>10}{'<700Hz NR':>11}{'驱动rms':>10}")
    for dl in [1.0, 2.0, 4.0, 6.0, 10.0, 20.0, 40.0]:
        r = run('tonal', f0=120.0, delay_ms=dl, wb_on=False)
        print(f"{dl:>10.1f}{r['nr']:>10.2f}{r['nr_band']:>11.2f}{r['y_rms']:>10.4f}")

    hr("4) 扬声器低频截止敏感性 (120Hz 噪声, 延迟 6ms)")
    print(f"{'截止(Hz)':>10}{'总NR':>10}{'<700Hz NR':>11}{'驱动rms':>10}")
    for fc in [80.0, 120.0, 150.0, 200.0, 300.0, 500.0]:
        r = run('tonal', f0=120.0, spk_hp=fc, wb_on=True)
        print(f"{fc:>10.1f}{r['nr']:>10.2f}{r['nr_band']:>11.2f}{r['y_rms']:>10.4f}")

    hr("5) 收敛轨迹 + 谐波分解 (120Hz 三谐波, 延迟 6ms)")
    r = run('tonal', f0=120.0, dur=10.0, trace=True)
    print("  每秒 NR(dB): " + "  ".join(f"{v:5.1f}" for v in r['trace']))
    print("  谐波衰减(dB): " + ", ".join(f"{f:.0f}Hz:{v:5.1f}" for f, v in r['harm'].items()))
