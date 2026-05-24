# QuietZone 实施计划

> **项目**: QuietZone 主动降噪 App
> **版本**: v1.0 MVP
> **制定日期**: 2026-05-24
> **计划周期**: 8 周 (Phase 0 ~ Phase 5)

---

## 0. 项目现状评估

### 0.1 已完成资产

| 资产 | 路径 | 状态 | 完成度 |
|------|------|------|--------|
| ANC 算法研究文档 | `ANC_算法研究与技术实现文档.md` | ✅ 已完成 | 100% |
| 产品设计文档 (PRD) | `QuietZone_产品设计文档_PRD.md` | ✅ 已完成 | 100% |
| C++ ANC 引擎头文件 | `cpp/anc_engine.h` | ✅ 骨架完成 | 70% |
| FxLMS 滤波器实现 | `cpp/fxlms.cpp` | ✅ 骨架完成 | 65% |
| 次级路径估计器 | `cpp/secondary_path.cpp` | ✅ 骨架完成 | 50% |
| 频谱分析器 | `cpp/spectrum_analyzer.cpp` | ✅ 骨架完成 | 60% |
| 音频处理器 | `cpp/audio_processor.cpp` | ✅ 骨架完成 | 55% |
| JNI 接口 | `cpp/anc_engine.cpp` | ✅ 骨架完成 | 70% |
| Kotlin ANCEngine | `engine/ANCEngine.kt` | ✅ 骨架完成 | 80% |
| ANC 前台服务 | `engine/ANCService.kt` | ✅ 骨架完成 | 70% |
| 音频流管理器 | `audio/AudioStreamManager.kt` | ✅ 骨架完成 | 50% |
| UI 界面 (Compose) | `ui/ANCScreen.kt` | ✅ 骨架完成 | 65% |
| MainActivity | `ui/MainActivity.kt` | ✅ 骨架完成 | 80% |
| CMake 构建配置 | `cpp/CMakeLists.txt` | ✅ 已完成 | 90% |
| Gradle 构建配置 | `app/build.gradle` | ✅ 已完成 | 85% |
| AndroidManifest | `AndroidManifest.xml` | ✅ 已完成 | 85% |

### 0.2 待完成关键缺口

| 缺口 | 严重度 | 说明 |
|------|--------|------|
| **Oboe C++ 集成** | 🔴 高 | 当前使用 Java AudioRecord/Track，延迟 20-50ms，不满足 ANC 要求。需替换为 Oboe C++ 回调 |
| **次级路径校准流程** | 🔴 高 | 校准逻辑仅有骨架，未实现完整的白噪声脉冲→LMS辨识→路径更新流程 |
| **单麦克风近似误差** | 🟠 中 | 手机只有1个麦克风，当前用 x(n) ≈ e(n) 近似，需加入更合理的参考信号估计 |
| **频谱实时刷新** | 🟠 中 | UI 频谱数据为静态模拟，未与引擎真实输出连接 |
| **状态机管理** | 🟠 中 | 校准中/运行中/待机等状态转换逻辑不完整 |
| **异常处理** | 🟡 低 | 音频设备异常、buffer underrun 等边界场景未覆盖 |

---

## 1. 总体里程碑

```
Week 1-2  ── Phase 0: 基础设施 ──→ 可编译可运行的白板 App
Week 2-3  ── Phase 1: 核心算法 ──→ FxLMS 算法验证通过
Week 3-5  ── Phase 2: 音频集成 ──→ Oboe 低延迟音频通路打通
Week 5-6  ── Phase 3: UI 完善 ──→ 完整交互界面
Week 6-7  ── Phase 4: 集成联调 ──→ 端到端 ANC 功能可用
Week 7-8  ── Phase 5: 测试优化 ──→ 性能达标，发布准备
```

```
      W1    W2    W3    W4    W5    W6    W7    W8
      ├─────┼─────┼─────┼─────┼─────┼─────┼─────┤
P0    ███████                                               🏗️ 可编译运行
P1          ██████                                          🧮 算法验证
P2                ██████████                                🔊 音频通路
P3                          ████████                        🎨 UI 完善
P4                                  ████████                🔗 集成联调
P5                                          ██████████      ✅ 测试发布
      ├─────┼─────┼─────┼─────┼─────┼─────┼─────┤
     M1     M2     M3     M4     M5     M6     M7     M8
```

---

## 2. Phase 0: 基础设施 (Week 1-2)

> **目标**: 项目可编译、可安装、可运行，UI 骨架可见

### 2.1 任务分解

| # | 任务 | 优先级 | 预估 | 交付物 | 依赖 |
|---|------|--------|------|--------|------|
| 0-1 | 完善 Gradle 构建配置，确保 Android Studio 可编译 | P0 | 4h | 可编译项目 | — |
| 0-2 | 集成 Oboe 库到 CMake 构建 | P0 | 3h | CMakeLists.txt 更新 | 0-1 |
| 0-3 | 修复 JNI 函数签名，确保 nativeInit 可调用 | P0 | 2h | JNI 调用验证通过 | 0-1 |
| 0-4 | 实现 ANCService 完整生命周期 | P1 | 4h | Service 可启停 | 0-3 |
| 0-5 | 完善 MainActivity 权限请求流程 | P1 | 2h | 麦克风权限正常弹出 | 0-4 |
| 0-6 | UI 骨架可渲染（静态界面，无实时数据） | P1 | 3h | App 可安装看到界面 | 0-5 |
| 0-7 | 配置 ProGuard / signing / 版本号 | P2 | 1h | Release 构建配置 | 0-1 |

### 2.2 里程碑 M1 验收标准

- [ ] `./gradlew assembleDebug` 构建成功
- [ ] App 安装到 Android 10+ 设备可正常启动
- [ ] 可看到完整 UI 骨架（暗色主题、所有卡片）
- [ ] 点击启动按钮可弹出麦克风权限请求
- [ ] 授权后 ANCService 可启动（即使引擎无实际处理）

---

## 3. Phase 1: 核心算法验证 (Week 2-3)

> **目标**: FxLMS 算法在 PC 仿真和 Android 设备上验证通过

### 3.1 任务分解

| # | 任务 | 优先级 | 预估 | 交付物 | 依赖 |
|---|------|--------|------|--------|------|
| 1-1 | PC 端 Python FxLMS 仿真验证 | P0 | 6h | Python 仿真脚本 + 结果图 | — |
| 1-2 | C++ FxLMS 单元测试 (Google Test) | P0 | 6h | test_fxlms.cpp | 1-1 |
| 1-3 | 完善 FxLMSFilter::process NEON 优化路径 | P0 | 4h | NEON 优化代码 + benchmark | 1-2 |
| 1-4 | 完善 FxLMSFilter::update NEON 优化路径 | P0 | 3h | NEON 优化代码 + benchmark | 1-2 |
| 1-5 | 实现 SecondaryPathEstimator 完整离线校准 | P0 | 8h | 离线校准流程可用 | 1-2 |
| 1-6 | 实现 SecondaryPathEstimator 在线更新 | P1 | 6h | 在线估计收敛测试 | 1-5 |
| 1-7 | 实现 AudioProcessor 三种模式完整逻辑 | P0 | 8h | FF/FB/Hybrid 三模式 | 1-3,1-4,1-5 |
| 1-8 | SpectrumAnalyzer 单元测试 + 优化 | P1 | 3h | FFT 正确性验证 | 1-2 |
| 1-9 | 算法性能 benchmark (处理时间 < 500μs) | P1 | 3h | benchmark 报告 | 1-7 |

### 3.2 Python 仿真验证计划

```python
# 仿真场景
场景1: 纯音 200Hz, Feedforward FxLMS → 预期 > 20dB 降噪
场景2: 纯音 500Hz, Feedback FxLMS → 预期 > 15dB 降噪
场景3: 混合噪声 (200Hz+500Hz+白噪声), Hybrid → 预期 > 10dB 降噪
场景4: 非平稳噪声 (频率突变), 验证收敛速度 < 3s
场景5: 次级路径估计误差 ±20%, 验证算法鲁棒性
```

### 3.3 里程碑 M2 验收标准

- [ ] Python 仿真: 三种模式均可收敛，Hybrid 降噪量 > 10dB
- [ ] C++ 单元测试: 所有测试通过
- [ ] NEON benchmark: 256 阶 FIR 卷积 < 100μs (arm64-v8a)
- [ ] 次级路径离线校准: 估计误差 < 5%
- [ ] Hybrid 模式完整流程可跑通 (模拟输入)

---

## 4. Phase 2: 低延迟音频集成 (Week 3-5)

> **目标**: Oboe C++ 回调打通，端到端延迟 < 5ms

### 4.1 任务分解

| # | 任务 | 优先级 | 预估 | 交付物 | 依赖 |
|---|------|--------|------|--------|------|
| 2-1 | 实现 Oboe InputStream (麦克风, 48kHz, LowLatency) | P0 | 6h | 麦克风采集可用 | M1 |
| 2-2 | 实现 Oboe OutputStream (扬声器, 48kHz, LowLatency) | P0 | 6h | 扬声器输出可用 | M1 |
| 2-3 | 实现 Oboe AudioCallback (直接 C++ 回调，不经 JNI) | P0 | 8h | 回调内完成 FxLMS 处理 | 2-1,2-2,1-7 |
| 2-4 | 替换 Java AudioRecord/Track 为 Oboe C++ 管道 | P0 | 4h | AudioStreamManager 简化 | 2-3 |
| 2-5 | 实现环形缓冲区 (lock-free SPSC) | P0 | 4h | 参考信号缓冲 | 2-3 |
| 2-6 | 延迟测量工具 (环路测试: 扬声器→麦克风) | P1 | 6h | 延迟测量功能 | 2-3 |
| 2-7 | 音频设备热插拔处理 (有线/USB-C 插入) | P1 | 4h | 外接音箱自动检测 | 2-4 |
| 2-8 | Buffer underrun / overrun 恢复机制 | P1 | 3h | 健壮性 | 2-3 |
| 2-9 | JNI 统计/频谱数据传递优化 | P2 | 3h | Stats/Spectrum 刷新 < 100ms | 2-3 |

### 4.2 Oboe 集成架构

```
┌─ Oboe C++ 层 ──────────────────────────────┐
│                                              │
│  InputStream::onAudioReady()                │
│       │  float* inputData                    │
│       ↓                                      │
│  ┌──────────────────────────┐               │
│  │   ANC Engine (C++)       │               │
│  │   processFrame()         │ ← 直接C++调用 │
│  │   · FxLMS filter         │   无JNI开销   │
│  │   · Secondary path       │               │
│  │   · Spectrum (降频更新)   │               │
│  └──────────────────────────┘               │
│       │  float* outputData                   │
│       ↓                                      │
│  OutputStream::onAudioReady()               │
│                                              │
│  Stats/Spectrum → JNI (100ms 刷新)          │
└──────────────────────────────────────────────┘
```

### 4.3 延迟优化策略

| 优化项 | 方法 | 预期收益 |
|--------|------|---------|
| AAudio MMAP | Android 9+ 直接内存映射 | -1~2ms |
| Exclusive 模式 | 不与其他 App 共享音频设备 | -2~5ms |
| 小 Buffer | framesPerCallback=96 (2ms) | -0.67ms |
| C++ 直接回调 | 绕过 Java 层 | -1~3ms |
| THREAD_PRIORITY_URGENT_AUDIO | 音频线程最高优先级 | 减少调度抖动 |
| -O3 + NEON | 编译器 + SIMD | 减少处理时间 |

### 4.4 里程碑 M3 验收标准

- [ ] Oboe 回调稳定运行，无 crash
- [ ] 端到端延迟 < 5ms (Pixel 6+ / Galaxy S22+ 级设备)
- [ ] 环路测试: 扬声器播放脉冲 → 麦克风采集，测量 round-trip
- [ ] 外接音箱插拔可自动切换输出路由
- [ ] 连续运行 10 分钟无 buffer underrun

---

## 5. Phase 3: UI 完善与交互 (Week 5-6)

> **目标**: 完整交互界面，所有 PRD 功能可视化

### 5.1 任务分解

| # | 任务 | 优先级 | 预估 | 交付物 | 依赖 |
|---|------|--------|------|--------|------|
| 3-1 | 实现状态机: Idle→Calibrating→Converging→Running→Error | P0 | 6h | 状态机 ViewModel | M2 |
| 3-2 | 主控按钮完整交互 (权限→校准→运行→停止) | P0 | 4h | 完整启停流程 | 3-1 |
| 3-3 | 实时统计数据刷新 (降噪量/延迟/收敛) | P0 | 4h | Stats 100ms 刷新 | 3-1 |
| 3-4 | 实时频谱图渲染 (Compose Canvas) | P0 | 6h | 双曲线频谱 4Hz 刷新 | 3-1 |
| 3-5 | 模式切换交互 + 运行时切换 | P1 | 3h | 三模式 Chip | 3-2 |
| 3-6 | 参数面板完整功能 (步长/增益/外接音箱) | P1 | 4h | 滑块+开关联动引擎 | 3-2 |
| 3-7 | 校准中状态 UI (进度条+提示) | P1 | 3h | 校准动画 | 3-2 |
| 3-8 | 通知栏控制 (播放/暂停/停止) | P1 | 4h | Notification Actions | 3-2 |
| 3-9 | 错误/异常状态提示 (无权限/设备不支持) | P2 | 2h | Snackbar/Dialog | 3-2 |
| 3-10 | 暗色主题微调 + 动画效果 | P2 | 3h | 视觉打磨 | 3-4 |

### 5.2 ViewModel 状态机设计

```kotlin
sealed class ANCState {
    object Idle : ANCState()                          // 待机
    object RequestingPermission : ANCState()           // 请求权限
    object Calibrating : ANCState()                    // 校准中
    data class Converging(val progress: Float) : ANCState()  // 收敛中
    data class Running(                               // 运行中
        val noiseReductionDb: Float,
        val processingTimeUs: Float,
        val isConverged: Boolean
    ) : ANCState()
    data class Error(val message: String) : ANCState() // 错误
}
```

### 5.3 里程碑 M4 验收标准

- [ ] 完整启停流程: 点击→校准(2-3s)→运行→点击停止
- [ ] 实时统计面板数据刷新 (降噪量、延迟、收敛状态)
- [ ] 频谱图实时更新，参考/残差双曲线可见
- [ ] 运行中切换模式，滤波器重新收敛可见
- [ ] 外接音箱开关可切换，增益自动调整
- [ ] 通知栏可控制启停

---

## 6. Phase 4: 集成联调 (Week 6-7)

> **目标**: 端到端 ANC 功能完整可用，真实环境降噪有效

### 6.1 任务分解

| # | 任务 | 优先级 | 预估 | 交付物 | 依赖 |
|---|------|--------|------|--------|------|
| 4-1 | 全链路集成: UI → Service → Oboe → FxLMS → 输出 | P0 | 8h | 端到端功能 | M3,M4 |
| 4-2 | 单麦克风参考信号估计优化 | P0 | 6h | 改进参考信号质量 | 4-1 |
| 4-3 | 次级路径校准完整实现 (白噪声→辨识→更新) | P0 | 6h | 校准后降噪明显提升 | 4-1 |
| 4-4 | 外接音箱模式完整流程 (检测→校准→输出) | P1 | 6h | 外接音箱降噪可用 | 4-3 |
| 4-5 | 步长 μ 自适应调整 (防止发散) | P1 | 4h | 稳定性提升 | 4-1 |
| 4-6 | 输出限幅 + 防削波保护 | P0 | 2h | 防扬声器损伤 | 4-1 |
| 4-7 | 首次使用引导 (Onboarding) | P2 | 4h | 引导页 | 4-1 |
| 4-8 | 日志系统完善 (可调试模式) | P2 | 3h | 调试工具 | 4-1 |

### 6.2 关键联调场景

| 场景 | 预期行为 | 验证方法 |
|------|---------|---------|
| 手机扬声器模式 | 室内空调噪声降低 8-15dB | 声级计 App 测量 |
| 外接 3.5mm 音箱 | 窗边交通噪声降低 15-20dB | 声级计 App 测量 |
| 切换模式 | 收敛时间 < 5s | 观察 UI 指标 |
| 来电/闹钟中断 | ANC 暂停，通话结束自动恢复 | 实际测试 |
| 插入/拔出音箱 | 自动检测，重新校准 | 设备插拔 |

### 6.3 里程碑 M5 验收标准

- [ ] 手机扬声器模式: 200Hz 纯音降噪 > 10dB
- [ ] 外接音箱模式: 200Hz 纯音降噪 > 15dB
- [ ] 校准流程: 首次启动自动校准，2-3秒完成
- [ ] 运行 30 分钟无 crash、无发散
- [ ] 来电中断可正常恢复

---

## 7. Phase 5: 测试与发布准备 (Week 7-8)

> **目标**: 性能达标，全机型兼容，可发布

### 7.1 任务分解

| # | 任务 | 优先级 | 预估 | 交付物 | 依赖 |
|---|------|--------|------|--------|------|
| 5-1 | 算法降噪量系统测试 (纯音/窄带/宽带) | P0 | 6h | 测试报告 | M5 |
| 5-2 | 延迟/性能系统测试 | P0 | 4h | 性能报告 | M5 |
| 5-3 | 多机型兼容性测试 (5+ 机型) | P0 | 8h | 兼容性报告 | M5 |
| 5-4 | 电池消耗测试 | P1 | 4h | 电池报告 | M5 |
| 5-5 | 长时间稳定性测试 (4h 持续运行) | P1 | 4h | 稳定性报告 | M5 |
| 5-6 | 性能优化 (根据测试结果) | P0 | 6h | 优化后代码 | 5-1,5-2 |
| 5-7 | 安全审计 (权限最小化/数据不外泄) | P1 | 3h | 审计报告 | M5 |
| 5-8 | 应用商店素材 (图标/截图/描述) | P2 | 4h | 素材文件 | — |
| 5-9 | 签名 + Release 构建 | P0 | 2h | APK/AAB | 5-6 |
| 5-10 | 内测发布 (5-10 人) | P1 | 2h | 内测反馈 | 5-9 |

### 7.2 兼容性测试矩阵

| 品牌 | 机型 | 芯片 | Android | AAudio | 备注 |
|------|------|------|---------|--------|------|
| Google | Pixel 7 | Tensor G2 | 13 | ✅ | 基准设备 |
| Samsung | Galaxy S23 | Snapdragon 8 Gen2 | 13 | ✅ | 主流旗舰 |
| Xiaomi | 13 Pro | Snapdragon 8 Gen2 | 13 | ✅ | 国内主流 |
| OPPO | Find X6 | Dimensity 9200 | 13 | ✅ | MTK 平台 |
| Huawei | P60 | Snapdragon 8+ Gen1 | 13(HMS) | ⚠️ | 需验证 HMS 兼容 |
| Samsung | Galaxy A54 | Exynos 1380 | 13 | ✅ | 中端设备 |
| Redmi | Note 12 Pro | Dimensity 1080 | 12 | ✅ | 低端设备 |

### 7.3 里程碑 M6 验收标准

- [ ] 降噪量: Hybrid 模式 200Hz 纯音 > 15dB (旗舰机)
- [ ] 延迟: 端到端 < 5ms (旗舰机), < 10ms (中端机)
- [ ] CPU: < 15% 单核占用
- [ ] 电池: < 5%/小时
- [ ] 7 款测试机型中 ≥ 5 款通过全部测试
- [ ] 4 小时持续运行无 crash
- [ ] Release APK 可正常安装运行

---

## 8. 资源与分工建议

### 8.1 角色配置

| 角色 | 人数 | 职责 |
|------|------|------|
| **算法工程师** | 1 | FxLMS/C++ 引擎开发、NEON 优化、次级路径 |
| **Android 开发** | 1 | Oboe 集成、Service、JNI、UI |
| **测试工程师** | 1 (可兼职) | 兼容性/性能/稳定性测试 |

### 8.2 关键技术风险与预案

| 风险 | 概率 | 影响 | 预案 |
|------|------|------|------|
| **手机扬声器低频太弱，降噪无效** | 高 | 高 | Phase 4 早期验证；若无效则主推外接音箱模式，UI 明确引导 |
| **延迟 > 5ms 导致前馈模式不收敛** | 中 | 高 | 优先确保 Hybrid/Feedback 模式可用；反馈模式无因果性约束 |
| **不同手机 Oboe 延迟差异大** | 高 | 中 | Phase 5 兼容性测试；启动时自动测延迟，延迟过大弹窗警告 |
| **声学反馈啸叫** | 中 | 中 | Leaky 因子防发散；输出限幅；检测到啸叫时自动降低增益 |
| **NEON 浮点精度不足** | 低 | 低 | 关键路径使用 double 验证对比；如果差异大则切换 |

---

## 9. 交付清单

### 9.1 代码交付

```
ANCApp/
├── app/src/main/cpp/              # C++ 算法引擎 (含 Oboe 回调)
│   ├── anc_engine.h
│   ├── fxlms.cpp                  # FxLMS + NEON
│   ├── secondary_path.cpp         # 次级路径估计
│   ├── spectrum_analyzer.cpp      # FFT 频谱
│   ├── audio_processor.cpp        # 三模式处理
│   ├── oboe_callback.cpp          # 🆕 Oboe 回调实现
│   ├── ring_buffer.h              # 🆕 Lock-free 环形缓冲
│   └── anc_engine.cpp             # JNI
├── app/src/main/java/com/anc/app/
│   ├── engine/
│   │   ├── ANCEngine.kt
│   │   ├── ANCService.kt
│   │   └── ANCViewModel.kt        # 🆕 状态机 ViewModel
│   ├── audio/
│   │   └── AudioStreamManager.kt
│   ├── ui/
│   │   ├── MainActivity.kt
│   │   ├── ANCScreen.kt
│   │   ├── components/            # 🆕 拆分子组件
│   │   │   ├── MainControlButton.kt
│   │   │   ├── StatsPanel.kt
│   │   │   ├── SpectrumView.kt
│   │   │   ├── ModeSelector.kt
│   │   │   └── ParameterPanel.kt
│   │   └── onboarding/            # 🆕 首次引导
│   │       └── OnboardingScreen.kt
│   └── utils/
│       ├── PermissionHelper.kt    # 🆕
│       └── AudioDeviceHelper.kt   # 🆕
├── app/src/test/                  # 🆕 单元测试
│   └── java/com/anc/app/
│       └── FxLMSTest.kt
├── app/src/androidTest/           # 🆕 集成测试
│   └── java/com/anc/app/
│       ├── AudioLatencyTest.kt
│       └── ANCEndToEndTest.kt
├── app/src/main/res/
│   ├── drawable/                  # 🆕 图标/矢量图
│   ├── mipmap-*/                  # 🆕 应用图标
│   └── values/
├── docs/                          # 🆕 文档目录
│   ├── ANC_算法研究与技术实现文档.md
│   ├── QuietZone_产品设计文档_PRD.md
│   ├── QuietZone_实施计划.md
│   └── test_reports/              # 🆕 测试报告
└── build 配置文件
```

### 9.2 文档交付

| 文档 | 状态 |
|------|------|
| ANC 算法研究文档 | ✅ 已完成 |
| 产品设计文档 (PRD) | ✅ 已完成 |
| 实施计划 | ✅ 本文档 |
| 测试报告 | 📋 Phase 5 产出 |
| 性能基准报告 | 📋 Phase 5 产出 |
| 兼容性报告 | 📋 Phase 5 产出 |

---

## 10. 周计划速查

| 周次 | Phase | 核心任务 | 交付里程碑 |
|------|-------|---------|-----------|
| **W1** | P0 | Gradle/Oboe构建、JNI调通、UI骨架 | 可编译运行 |
| **W2** | P0+P1 | Service生命周期、Python仿真、FxLMS测试 | M1: 白板App |
| **W3** | P1+P2 | NEON优化、Oboe InputStream/Output | M2: 算法验证 |
| **W4** | P2 | Oboe回调集成、环形缓冲、延迟测量 | |
| **W5** | P2+P3 | 设备热插拔、UI状态机、频谱渲染 | M3: 音频通路 |
| **W6** | P3+P4 | 参数面板、模式切换、全链路集成 | M4: UI完善 |
| **W7** | P4+P5 | 校准优化、外接音箱、性能/降噪测试 | M5: 功能可用 |
| **W8** | P5 | 兼容性测试、优化、签名发布 | M6: 可发布 |

---

*文档结束 — QuietZone v1.0 实施计划*
