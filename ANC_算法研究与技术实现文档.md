# 主动降噪 (ANC) 算法研究与技术实现文档

> 项目：Android 主动降噪应用 (ANCApp)
> 版本：v1.0
> 日期：2026-05-24

---

## 1. 主动降噪原理概述

### 1.1 物理原理

主动降噪 (Active Noise Cancellation, ANC) 的核心原理是**声波叠加干涉**——生成一个与环境噪声**等幅反相**(相位差180°)的声波，两列波叠加后实现降噪。

$$d(n) + y'(n) \rightarrow e(n) \approx 0$$

其中：
- $d(n)$: 原始噪声（Primary Noise）
- $y'(n)$: 反噪声经次级路径后的信号（Anti-noise through Secondary Path）
- $e(n)$: 残差噪声（Error/Residual Noise）

### 1.2 关键约束

| 约束条件 | 说明 | 手机场景影响 |
|---------|------|------------|
| **因果性** | 电子延迟 < 声学延迟 | 手机麦克风到扬声器距离短（~5cm），声学延迟仅~0.15ms，留给算法的时间窗口极小 |
| **实时性** | 处理延迟 < 1个采样周期 | 48kHz采样下每帧仅20.8μs，需分块处理 |
| **稳定性** | 步长 μ 满足收敛条件 | $0 < \mu < \frac{2}{L \cdot E[\hat{x}^2]}$ |
| **次级路径** | 必须准确估计 Ŝ(z) | 手机扬声器→麦克风路径非线性严重 |

---

## 2. 核心算法：FxLMS

### 2.1 算法推导

**FxLMS (Filtered-x Least Mean Square)** 是ANC领域最经典、应用最广泛的算法，由Morgan (1980) 和Burgess (1981) 独立提出。

#### 2.1.1 标准LMS的问题

在ANC系统中，自适应滤波器的输出 $y(n)$ 需要经过**次级路径** $S_2(z)$（功放→扬声器→空间传播→麦克风→ADC）才能到达误差麦克风。如果直接用标准LMS：

$$w(n+1) = w(n) + \mu \cdot x(n) \cdot e(n)$$

由于 $S_2(z)$ 的存在，$x(n)$ 与 $e(n)$ 之间的相关性被扭曲，导致**算法不收敛**。

#### 2.1.2 FxLMS修正

FxLMS的关键修正：**将参考信号通过次级路径估计 $\hat{S}_2(z)$ 滤波**后再用于权重更新：

$$\hat{x}(n) = \hat{S}_2(z) * x(n)$$

$$w(n+1) = w(n) + \mu \cdot \hat{x}(n) \cdot e(n)$$

这保证了权重更新方向与误差曲面梯度一致。

### 2.2 完整算法流程

```
初始化: w(0) = 0, μ = 0.01

For n = 0, 1, 2, ...:
  1. 采集参考信号 x(n)
  2. 滤波器输出: y(n) = wᵀ(n) · x(n)
  3. 输出反噪声: -y(n) → 扬声器
  4. 采集误差信号: e(n) = d(n) + S₂*y(n)
  5. 滤波参考: x̂(n) = Ŝ₂ * x(n)
  6. Leaky FxLMS更新:
     w(n+1) = (1 - μδ) · w(n) + μ · x̂(n) · e(n)
```

### 2.3 Leaky FxLMS

Leaky变体引入泄漏因子 $\delta$，防止权重无限增长：

$$w(n+1) = (1 - \mu\delta) \cdot w(n) + \mu \cdot \hat{x}(n) \cdot e(n)$$

- $\delta = 0$: 标准FxLMS
- $\delta > 0$: 权重向零收缩，增强鲁棒性
- 本实现采用 $\delta = 0.0001$, 即 $leakyFactor = 1 - \mu\delta = 0.9999$

### 2.4 归一化 FxLMS (Normalized FxLMS)

步长自适应：

$$\mu(n) = \frac{\tilde{\mu}}{\|\hat{x}(n)\|^2 + \epsilon}$$

其中 $\tilde{\mu}$ 为归一化步长 (0~2)，$\epsilon$ 为防除零小量。

---

## 3. ANC系统架构

### 3.1 Feedforward 前馈系统

```
参考麦克风 x(n) ──→ [W(z)] ──→ y(n) ──→ 扬声器
                       │                  │
                       │  Ŝ(z)            │ S₂(z)
                       ↓                  ↓
                    x̂(n)              y'(n)
                       │                  │
                       ↓                  ↓
误差麦克风 e(n) ← d(n) + y'(n) ──→ FxLMS更新
```

**优点**：
- 宽带降噪（可处理随机噪声）
- 结构简单、稳定性好

**缺点**：
- 需要参考麦克风（手机仅有1-2个）
- 受因果性约束（电子延迟必须 < 声学延迟）

**适用场景**：环境噪声方向已知，如窗边、空调旁

### 3.2 Feedback 反馈系统

```
误差麦克风 e(n) ──→ 估计 d̂(n) ──→ [W(z)] ──→ y(n) ──→ 扬声器
     ↑                  │                         │
     │                  │ Ŝ(z)                    │ S(z)
     │                  ↓                         ↓
     └──── d(n) + S*y(n) ←────────── y'(n) ←────┘
```

核心：**从误差信号估计原始噪声**

$$\hat{d}(n) = e(n) - \hat{S}(z) * y(n)$$

**优点**：
- 不需要参考麦克风
- 结构紧凑

**缺点**：
- 仅对窄带/周期噪声有效
- 非因果限制

**适用场景**：耳机降噪、无法放置参考麦克风

### 3.3 Hybrid 混合系统（本应用推荐模式）

```
x(n) ──→ [W_ff(z)] ──→ y_ff(n) ──┐
                                   ├──→ y(n) = y_ff + y_fb ──→ 扬声器
e(n) ──→ [W_fb(z)] ──→ y_fb(n) ──┘
```

**混合 = 前馈 + 反馈**，前馈处理宽带噪声，反馈处理窄带残余。

**优点**：
- 宽窄带兼顾
- 降噪量最大（可达 20-30dB 低频）

**缺点**：
- 计算量最大
- 需要两个自适应滤波器

---

## 4. 次级路径估计

### 4.1 问题

FxLMS需要知道次级路径 $S_2(z)$ 才能正确滤波参考信号。次级路径包括：
1. DAC → 功放 → 扬声器（电声转换）
2. 扬声器 → 空间传播 → 误差麦克风（声学传播）
3. 误差麦克风 → ADC（声电转换）

这个路径**随温度、距离、环境变化而改变**，必须动态估计。

### 4.2 离线估计方法

**白噪声激励法** (Eriksson, 1991)：
1. 播放白噪声 $v(n)$ 到扬声器
2. 同时在误差麦克风录音 $e(n)$
3. 用LMS辨识 $\hat{S}_2(z)$：最小化 $|e(n) - \hat{S}_2 * v(n)|^2$

### 4.3 在线估计方法

**辅助白噪声注入法**：
1. 在输出信号中叠加低电平白噪声 $v(n)$（-40dB）
2. 用LMS持续估计 $\hat{S}_2(z)$
3. 用户几乎听不到辅助噪声

**参考文献**：
- Shi et al., "A Computation-efficient Online Secondary Path Modeling Technique for Modified FXLMS Algorithm", arXiv 2023

---

## 5. 性能优化策略

### 5.1 ARM NEON SIMD加速

FxLMS中最耗时的操作是**FIR卷积**和**权重更新**，均为向量点积/乘加运算，天然适合NEON SIMD加速。

```c
// 4路并行乘加
float32x4_t sum = vdupq_n_f32(0.0f);
for (int i = 0; i <= L-4; i += 4) {
    float32x4_t w = vld1q_f32(&weights[i]);
    float32x4_t x = vld1q_f32(&x_buf[i]);
    sum = vmlaq_f32(sum, w, x);  // 4次乘加 → 1条NEON指令
}
```

**预期加速**: 卷积/更新部分 2-4x

### 5.2 分块频域FxLMS

对于长滤波器（L > 128），时域卷积复杂度 O(N·L) 过高，可使用**Overlap-Save频域卷积**：

$$Y(k) = W(k) \cdot X(k)$$

复杂度降至 O(N·logN)，但引入1帧额外延迟。

**参考文献**：
- "Frequency-Domain Filtered-x LMS Algorithms for Active Noise Control", Applied Sciences 2018

### 5.3 低延迟音频

| 方案 | 延迟 | 说明 |
|-----|------|------|
| AudioRecord + AudioTrack | 20-50ms | Java层, 延迟大, 不适合ANC |
| OpenSL ES | 10-20ms | NDK层, 较好但仍偏大 |
| **AAudio (Oboe)** | **2-5ms** | Android 8+ 原生低延迟API |
| **MMAP** | **< 2ms** | Android 9+ 直接内存映射 |

本应用使用 **Oboe库**，自动选择AAudio (Android 8+) / OpenSL ES (旧版)：

```kotlin
// Oboe关键配置
AudioStreamBuilder()
    .setDirection(Direction.Input)
    .setPerformanceMode(PerformanceMode.LowLatency)  // 低延迟模式
    .setSharingMode(SharingMode.Exclusive)            // 独占模式
    .setSampleRate(48000)
    .setFramesPerCallback(128)                        // 2.67ms/帧
```

---

## 6. 深度学习增强ANC

### 6.1 Deep ANC (NeurIPS 2021)

将ANC建模为**监督学习问题**，训练CNN/LSTM直接映射参考信号到反噪声信号。

- **优势**：可处理非线性失真（扬声器非线性、高声压级失真）
- **劣势**：推理延迟、模型泛化性

**参考**：Zhang & Wang, "Deep ANC: A Deep Learning Approach to Active Noise Control", NeurIPS 2021

### 6.2 Latent FxLMS (arXiv 2025)

在FxLMS框架内嵌入**神经自适应滤波器**：

$$w(n+1) = w(n) + \mu \cdot \Phi_{\theta}(\hat{x}(n)) \cdot e(n)$$

其中 $\Phi_{\theta}$ 是学习到的投影函数，将滤波参考映射到低维流形上，**加速收敛**。

**参考**：arXiv:2507.03854, "Latent FxLMS: Accelerating Active Noise Control with Neural Adaptive Filters", 2025

### 6.3 混合深度在线学习 (IEEE 2024)

结合传统FxLMS和深度学习的**混合架构**：

- FxLMS处理线性可预测部分
- DNN处理非线性残余

**参考**：IEEE, "A Hybrid Deep-Online Learning Based Method for Active Noise Control", 2024

---

## 7. 外接音箱方案

### 7.1 必要性

手机扬声器在低频段（< 200Hz）输出能力极弱：
- 典型手机扬声器频响：200Hz - 20kHz
- 主动降噪主要目标频段：50Hz - 1kHz
- **矛盾**：降噪最需要的低频段恰恰是手机扬声器最弱的

### 7.2 外接方案

| 方案 | 连接方式 | 延迟 | 输出能力 | 适用场景 |
|-----|---------|------|---------|---------|
| 3.5mm有线音箱 | 耳机口 | ~1ms | 中 | 固定场景 |
| USB-C音频 | USB Host | ~2ms | 强 | 固定场景 |
| 蓝牙音箱 | A2DP | 100-300ms | 中 | ❌延迟太大 |
| 蓝牙LE Audio | LC3 | 20-50ms | 中 | 需LE Audio支持 |

**推荐**：3.5mm有线音箱或USB-C有源音箱，延迟最低、输出功率最大。

### 7.3 外接音箱模式特殊处理

1. **输出增益增大**：2x-4x
2. **次级路径重新校准**：外接音箱的声学路径完全不同
3. **滤波器长度可增加**：声学路径更长，需要更长FIR建模
4. **空间区域降噪**：外接音箱功率大，可在一定空间范围内形成静区

---

## 8. 手机ANC实现的挑战与对策

| 挑战 | 原因 | 对策 |
|------|------|------|
| 延迟太大 | Java层AudioRecord/Track延迟 > 20ms | 使用Oboe/AAudio, C++回调 |
| 麦克风数量不足 | 手机通常1-2个麦克风 | 利用多麦克风做波束成形参考 |
| 扬声器低频弱 | 手机扬声器尺寸限制 | 外接有源低音炮/音箱 |
| 声学反馈 | 扬声器→麦克风回声 | 反馈消除 + 次级路径估计 |
| 非线性失真 | 扬声器高声压失真 | Deep ANC + 非线性补偿 |
| 计算资源受限 | 手机CPU/GPU共享 | NEON优化 + 分块处理 |

---

## 9. 关键参考文献

| # | 论文 | 年份 | 核心贡献 |
|---|------|------|---------|
| [1] | Kuo & Morgan, "Active Noise Control Systems: Algorithms and DSP Implementations", Wiley | 1996 | ANC领域经典教材，完整推导FxLMS |
| [2] | Eriksson, "Development of the filtered-U algorithm for ANC", JASA | 1991 | 次级路径在线估计方法 |
| [3] | "Frequency-Domain Filtered-x LMS Algorithms for ANC", Applied Sciences | 2018 | 频域FxLMS加速 |
| [4] | Zhang & Wang, "Deep ANC: A Deep Learning Approach to ANC", NeurIPS | 2021 | 深度学习做ANC |
| [5] | "Latent FxLMS: Accelerating ANC with Neural Adaptive Filters", arXiv:2507.03854 | 2025 | 神经自适应滤波加速收敛 |
| [6] | Shi et al., "Computation-efficient Online Secondary Path Modeling", arXiv:2306.11408 | 2023 | 高效在线次级路径估计 |
| [7] | "Hybrid Deep-Online Learning for ANC", IEEE | 2024 | 传统+深度学习混合 |
| [8] | "Low-Frequency Active Noise Control System", Electronics | 2025 | 低频ANC系统设计 |
| [9] | "Active Noise Control System Based on Hybrid Fixed Filter", IEEE | 2023 | 混合固定+自适应滤波 |
| [10] | Google Oboe Library Documentation | 2024+ | Android低延迟音频开发 |

---

## 10. 算法数学公式汇总

### FxLMS 核心公式

$$\text{滤波器输出:} \quad y(n) = \mathbf{w}^T(n) \cdot \mathbf{x}(n)$$

$$\text{误差信号:} \quad e(n) = d(n) + s_2(n) * y(n)$$

$$\text{滤波参考:} \quad \hat{x}(n) = \hat{s}_2(n) * x(n)$$

$$\text{权重更新:} \quad \mathbf{w}(n+1) = (1-\mu\delta)\mathbf{w}(n) + \mu \cdot \hat{\mathbf{x}}(n) \cdot e(n)$$

### 收敛条件

$$0 < \mu < \frac{2}{L \cdot E[\hat{x}^2(n)]}$$

### 降噪量

$$NR(dB) = 10 \log_{10}\frac{E[d^2(n)]}{E[e^2(n)]}$$

### 次级路径估计

$$\min_{\hat{S}} E\left[|e(n) - \hat{S}(z)*v(n)|^2\right]$$

### Feedback ANC噪声估计

$$\hat{d}(n) = e(n) - \hat{S}(z) * y(n)$$

### 完美消噪条件

$$W_{opt}(z) = -\frac{P(z)}{S_2(z)}$$

---

*文档结束*
