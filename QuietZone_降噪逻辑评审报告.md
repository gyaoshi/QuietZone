# QuietZone 主动降噪 App —— 项目审查与降噪逻辑评审报告

> 审查日期：2026-09-18
> 审查对象：`D:\project\anc2`（GitHub: `gyaoshi/anc2`，私有仓库）
> 验证机型：2210132C（Redmi），Android 16 / SDK 36 / arm64-v8a
> 审查方式：源码通读 + GitHub Actions 产物核对 + 真机安装运行取证 + 算法逻辑逐行复刻仿真

---

## 0. 结论先行

| 问题 | 结论 |
|------|------|
| 项目能不能编译、能不能装到手机 | ✅ 能。CI 最后一次（`ec26a2b`）成功，产物齐全 |
| App 能不能启动、界面正不正常 | ✅ 能。界面完整，状态机、参数面板、通知栏前台服务都工作 |
| **降噪量为什么永远是 0** | ❌ **因为自适应滤波器权重从头到尾几乎不增长（\|w\| ≈ 1.2e-3），算法实际上是"空转"的** |
| **为什么没有声音输出** | ❌ **同一个原因。滤波器输出 y≈0 → 反噪声 ≈0 → 扬声器实际输出 rms 只有麦克风的约 1/300，等同静音** |
| 降噪实现逻辑是否可靠可行 | ❌ **不可行。数学内核（FxLMS 更新公式、环形缓冲）写法是对的，但信号通路是虚构的，整套结构在物理上不成立** |

一句话概括：**这不是"参数没调好"或者"差一点效果"的问题，而是"误差信号是编造出来的、次级路径校准管道是断的"，导致算法在数学上无梯度可学、输出恒等于零。**

我已在真机上复现，并用 Python 逐行复刻 C++ 逻辑复现出**完全一致**的数字（下面第 4 节）。

---

## 1. 项目与仓库现状

### 1.1 仓库 / CI

| 项 | 值 |
|----|----|
| 仓库 | `https://github.com/gyaoshi/anc2`（PRIVATE，默认分支 `main`） |
| 本地 HEAD | `ec26a2bf513595e475d47f66db8573c9c5809567`，与 `origin/main` 一致 |
| 最后一次 CI | Run `26360275607` ✅ success（9m45s，2026-05-24） |
| 之前 9 次 | 全部 failure（清一色是 `-Werror` 编译告警：prefab、未用参数、int→float、64 位位移） |

CI 配置（`.github/workflows/android.yml`）本身写得干净：temurin 17 + `gradle/actions/setup-gradle@v3`，没有踩 `android-actions/setup-android` 的坑。**唯一的短板是 CI 只做 `assembleDebug/Release`，不跑 `tests/` 里的 C++ 单元测试**，所以那 9 个测试实际上从来没有在 CI 里被执行过。

### 1.2 本地 APK

| 文件 | 大小 | 时间 | 说明 |
|------|------|------|------|
| `apk-output/debug/app-debug.apk` | 20.5 MB | 05-24 20:15 | 可直接安装（含 debug 签名） |
| `apk-output/release/app-release-unsigned.apk` | 9.9 MB | 05-24 20:02 | **unsigned，装不上**，只能用于对齐校验 |
| `apk-output/release-apk/app-release-unsigned.apk` | 9.9 MB | 05-24 19:55 | 同上的重复副本 |

两个包都正确包含了 `lib/arm64-v8a` 与 `lib/armeabi-v7a` 的 `libanc_engine.so` / `liboboe.so` / `libc++_shared.so`。**用 debug 包安装成功**（`versionCode=1, versionName=1.0, primaryCpuAbi=arm64-v8a`）。

### 1.3 仓库卫生（顺手提一下）

- `app/src/main/cpp/` 下残留 3 个编辑器临时文件：`audio_processor.cpp.tmp.2064.*`（2 个）、`ring_buffer.h.tmp.2064.*`，建议删掉并加进 `.gitignore`。
- `fxlms_simulation_results.png`（仿真图）与源码同目录，建议归到 `docs/`。

---

## 2. 真机运行取证

安装、授权、启动、点"开始降噪"后，从 `logcat` + `dumpsys` 拿到的硬证据：

**① 引擎确实启动了**（说明启动链路没坏）：
```
I/ANC_JNI : JNI: Init ANC Engine sr=48000 L=256 M=128 μ=0.010000 leaky=0.999900 block=128 mode=2
I/OboeEngine: Input stream opened
I/OboeEngine: Output stream opened
I/OboeEngine: Audio streams started
I/ANC_JNI : JNI: Start SUCCESS
```
前台服务也正常：`isForeground=true foregroundId=1001 types=0x2`。

**② 通知栏暴露了真实运行数据**（`dumpsys notification --noredact`）：
```
android.text=String (降噪 0.0 dB | 延迟 838 μs)
```
→ **降噪量 0.0 dB 与用户观察一致**；而 **延迟 838 μs 说明 `processFrame()` 确实在被调用**（否则这个值会是 0）。所以不是"回调没跑"，是"跑了但什么也没算出来"。

**③ 麦克风环形缓冲持续溢出**（8 秒内 776 次）：
```
W/OboeEngine: Mic buffer overflow: 0/128
```
`write()` 返回 0 表示缓冲已满、**新采到的麦克风数据被整批丢弃**。原因是输入、输出是两条独立的 Oboe 流，速率不匹配，而代码里**没有任何漂移补偿/重采样**。后果是算法拿到的参考信号永远是约 10.7 ms 之前的陈旧数据（容量 512 帧）。

**④ 输出流被系统挂起**（`dumpsys media.audio_flinger`）：`audio-playback-voip` 轨道创建后约 1.7 s 进入 standby，`FrmRdy=0`——客户端几乎没有向输出流写数据。

**⑤ 一条潜在干扰**（MIUI 特有）：
```
AudioHardening background playback would be muted for com.anc.app (10348), level: partial
```
小米的 AudioHardening 会对后台播放降级。当前 App 在前台所以没被真正静音，但一旦熄屏/退到后台，输出可能被系统削弱——这一点需要在测试清单里单独验证。

---

## 3. 降噪逻辑评审（核心）

### 3.1 值得肯定的部分

这几处**写法是对的**，不是"全盘皆错"：

- `FxLMSFilter::process()` / `update()` 的**环形缓冲索引推导是正确的**。我用手推 + 数值双向核对过：`x_ordered[i]` 恰好等于 `x(n-i)`，`seg2` 那段"从 `length_-1` 倒着取"的写法虽然绕，但因为映射关系恰好对消，结果无误。
- **权重更新公式本身正确**：`w ← leaky·w + μ·x̂(n)·e(n)·x_buf`（标量 x̂ 乘整个参考向量的做法是对的，v3 那次的修正确实修掉了"只更新 weights_[0]"的老 bug）。
- NEON 8 路展开、权重限幅 ±10、输出限幅 ±0.95、Leaky 防漂移、锁无关 SPSC 环形缓冲（`RingBuffer` 的 acquire/release 配对正确）、Radix-2 FFT —— 这些都实现得规范。
- Oboe 用了 `LowLatency` + `Exclusive` + `Float` + 128 帧，方向正确。

**问题不在这些地方，而在"喂给这个滤波器的是什么信号"。**

### 3.2 致命问题 A：误差信号是编造的，不是测量出来的

`audio_processor.cpp` 三种模式的误差都这么算：

```cpp
float anti_noise = -y_n * config_.outputGain;
float e_n = x_n + anti_noise;      // ← 虚构的"误差"
```

这等价于假设：**反噪声从扬声器发出后，以单位增益、零延迟、零相移、零失真，瞬间出现在同一个麦克风上**。真实情况是它要经过 `DAC→功放→扬声器→空气传播→麦克风→ADC`，这条路（次级路径 `S₂`）有 5~30 ms 的延迟和强烈的频率选择性。

**后果**：LMS 的梯度方向由这个 `e(n)` 决定，而它和真实误差没有任何对应关系。算法优化的目标函数是"让 y(n) 复现 x(n)"——即把麦克风信号原样复制一遍再反相。这**不是主动降噪**，而是"把环境噪声反相后重新播一遍"。

同时这也是**正反馈闭环**：麦克风信号里本来就包含自己的扬声器输出（`mic = d + S₂·out`），而代码直接把 `mic` 当成参考信号 `x`，既没有做反馈中和（feedback neutralisation），也没有把自身输出从参考里减掉。一旦环路增益上去就会啸叫。目前的限幅只是掩盖了症状。

### 3.3 致命问题 B：次级路径估计器形同虚设 + 校准管道是断的

`filterSample()` 用的是 `path_coeffs_`，而它的初值是构造函数里的：

```cpp
int delay_samples = 8;
if (delay_samples < filter_length_) path_coeffs_[delay_samples] = 0.5f;   // Ŝ = 0.5·δ(n-8)
```

也就是 **Ŝ 恒等于"8 个采样的纯延迟 × 0.5 增益"**（≈0.17 ms），这跟真实的手机扬声器→麦克风路径毫无关系。

更关键的是，**本来应该覆盖它的那条校准链路是断的**：

1. `OboeEngine::onAudioReady()` 里调用的是 `processCalibrationFrame(nullptr, outputData, numFrames)` —— **第一个参数硬编码 nullptr**；
2. 而函数体内 `if (inputData) { calibration_input_.push_back(...); }` —— 进来的永远是空指针，**录音数据一条都没被采集**；
3. 于是 `calibration_input_.empty()` 恒为真，`offlineCalibrate()` **永远不会被调用**；
4. 再加上 `ANCViewModel.startANC()` 里**根本没有调用 `engine.calibrate()`**（UI 会先把状态切到"校准中"，但 `isCalibrating()` 一直是 false，监控协程立刻跳到"收敛中"）——**用户看到的"校准"是假的**。

**数学后果（这才是降噪量恒为 0 的直接原因）**：更新量是 `μ·x̂(n)·e(n)·x(n-i)`，而 `x̂(n) = 0.5·x(n-8)`（延迟 8 个采样），`e(n) = x(n) - y(n)`（零延迟）。两者在时间上错位，导致逐样本乘积的**统计期望为 0**：

- `E[x(n-8)·x(n)·x(n-i)] = 0`（三阶矩，零均值对称信号下恒为 0）
- `E[x(n-8)·y(n)·x(n-i)]` 在 `w≈0` 时也为 0

→ **梯度恒为 0 → 权重不增长 → y≈0 → 反噪声≈0 → 输出静音 → NR=0**。这是结构性死锁，调 μ 没用，调滤波器长度也没用。

### 3.4 致命问题 C：单麦克风拓扑在物理上不成立

前馈 ANC 要成立，必须满足**因果性**：参考麦克风必须**提前**拿到噪声，即在噪声到达"误差点"之前就采到。这需要参考麦和误差麦**是两个物理位置不同的麦克风**。

本 App 只有一部手机、一个麦克风——**参考麦和误差麦是同一点**。这意味着：

- 噪声到达参考点和误差点是**同一时刻**，"提前量"为负 → 对宽带随机噪声**根本不可能降**，这是原理性的天花板，不是工程优化问题；
- 无论叫它"前馈"还是"混合"，实际都退化成**反馈式 ANC**，只能对付**窄带/周期性**噪声，而且受环路延迟严格限制；
- 标称的 `中低频 50–500 Hz、15–25 dB` 在手机上不成立：手机扬声器在 200 Hz 以下输出能力极弱（PRD 自己也写了），而**要降的频段恰好就是扬声器最弱的频段**；
- "开放空间静区"还需要多通道/大尺寸声源阵列做波前匹配，单点小扬声器只能做出极小范围的局部抵消，且对头部位置极度敏感。

**这不是"以后优化能解决"的问题，是产品定位需要重新划定。**

### 3.5 问题 D：Python 仿真与 C++ 实现不是同一个算法（"44 dB"不能当证据）

`fxlms_simulation.py` 里，误差是按**真实次级路径卷积**算的：

```python
y_prime = np.dot(s2, s2_buf)   # S₂ = [0.1, 0.3, 0.4, 0.2]
e_n = d[n] + y_prime           # 真正的误差麦克风读数
```

这是一个**标准、正确**的 FxLMS 仿真，所以它能跑出 44 dB。

但 C++ 里**没有这一步**——它用的是 `e_n = mic + anti_noise`。**两者不是同一个算法。** 所以仿真报告上的降噪量**不能作为"实现有效"的证据**，它只证明了"如果按标准 FxLMS 做，是能降的"。

### 3.6 问题 E：单元测试是空心的（"9/9 通过"没有意义）

- `test_feedforward_anc()` 是唯一的主收敛测试，注释里写着"只要 NR > 0 就说明算法在工作"，**但代码里只 `ASSERT_GT(stats.frameCount, 0)`——什么都没校验收敛**。
- 更巧的是这个测试配了 `config.secondaryPathLength = 4`，而默认延迟是 8，`if (delay_samples < filter_length_)` 不成立 → **Ŝ 全零** → `x̂ ≡ 0` → **权重从头到尾一次都没更新过**。测试通过，但算法一次都没跑。
- 其余 8 个测试校验的是"权重范数 > 0""无 NaN""输出不超 ±1""reset 后归零"这类**结构性质**，没有一个触及真实降噪能力。
- 而这些测试**在 CI 里从来没被执行过**。

### 3.7 问题 F：实时音频路径的工程缺陷

| 位置 | 问题 |
|------|------|
| `onAudioReady` 输出分支 | 每次回调 `std::vector<float> micData(numFrames)` —— **实时线程里堆分配**，可能触发锁/GC 抖动 |
| `processCalibrationFrame` | 实时回调里 `push_back` + `std::mutex` —— RT 线程里做动态分配和加锁 |
| `OboeEngine::start()` | `running_` 在**两次 `requestStart()` 之后**才置 true，而回调首行就是 `if (!running_) return Stop` —— 首帧回调若先到就会把流**永久停掉**（时序竞态） |
| 双流协同 | 输入/输出是两条独立流，**无漂移补偿**，实测持续溢出并丢弃全部新数据 |
| `nativeReset()` | 注释自己承认"需要重新 init 才能完全重置"，函数体是空的 |
| `generateAuxiliaryNoise` / `onlineUpdate` | 在线次级路径估计**从未被调用**，属于死代码 |

---

## 4. 量化证据：逐行复刻 C++ 逻辑的仿真

我把 `processFeedforward` + `FxLMSFilter::process/update` + `SecondaryPathEstimator::filterSample`（含默认 Ŝ=0.5·δ(n-8)、限幅、Leaky）**逐行搬到 Python**，脚本见 `_diag_fxlms.py` / `_diag_fxlms2.py`。

| 场景 | 权重范数 \|w\| | 输出 rms | 输入 rms | **算法自报 NR** |
|------|--------------|---------|---------|----------------|
| 环境噪声（低通宽带） | 1.28e-03 | 0.000156 | 0.0500 | **-0.00 dB** |
| 200 Hz 纯音 | 1.84e-04 | 0.000040 | 0.0354 | **+0.00 dB** |
| 200/300/450 Hz 三音 | 4.97e-04 | 0.000099 | 0.0409 | **-0.00 dB** |
| 白噪声 | 7.26e-04 | 0.000035 | 0.0500 | **+0.00 dB** |

对照真机通知栏：**"降噪 0.0 dB | 延迟 838 μs"** —— 仿真与真机**完全一致**。

两个结论同时被复现：

1. **NR ≡ 0.00 dB**，且和噪声类型无关（纯音、复合音、噪声都一样）→ 证明是结构性问题，不是"噪声不合适"；
2. **输出 rms 只有输入的 1/300 ~ 1/1500（约 -50 ~ -65 dB）** → 等同于静音，正是用户说的"似乎没有声音输出"。

另外我做了个"物理真相"对照实验（把误差换成真实声学 `e = d + S₂·y`，`S₂` 含 3 个采样延迟）：**真实降噪量同样是 -0.00 dB**，输出峰值 0.0007。也就是说，**即便把数字再算一遍，真实的声学残差也没有被削弱分毫**。

---

## 5. 可行性判断

| 目标 | 可行性 | 说明 |
|------|--------|------|
| 手机单机、开放空间、宽带降噪 10~25 dB | ❌ **不可行** | 违反因果性（参考麦=误差麦），且手机扬声器低频输出不足。这是物理约束，不是算法问题 |
| 手机单机、窄带/周期性噪声（风扇、空调嗡鸣）小范围降噪 | ⚠️ **有限可行** | 只能做反馈式 ANC，需重写结构 + 严格控制在环路延迟内，预期 5~10 dB 且只在喇叭正前方很近处 |
| 手机 + 有线音箱，构建局部静区 | ⚠️ **有希望** | 声源功率改善，但同样受单点声源与因果性限制，需要把参考麦与被控区分开 |
| 换耳机式 ANC（前馈+反馈双麦） | ✅ **可行** | 这才是标准 ANC 拓扑；但已超出当前产品定位 |
| 纯软件"降噪播放"（把噪声反相后叠加播放，靠经验调） | ⚠️ | 能做出"变化"，但无法保证是"降低"；容易变成加噪 |

**建议的产品定位调整**：要么承认它是"低延迟音频实验平台/教学演示"，要么把目标收窄为"配合外接音箱做窄带噪声抑制"，要么转向耳机式双麦方案。当前 PRD 里"无需专用硬件、10-25 dB、开放空间静区"的承诺，在手机上无法兑现。

---

## 6. 修复清单（按优先级）

### P0 —— 不修就没有任何降噪可言

1. **补上真实的次级路径辨识**：修 `processCalibrationFrame` 的 `nullptr`（改成把 input 流的录音数据传进来），并把 `calibrate()` 接进 `startANC()`。注意：输入/输出两条流的固有延迟（可达 10~30 ms）**远大于 128 抽头（2.67 ms）的建模窗口**，所以 `secondaryPathLength` 至少要扩到能覆盖实测回环延迟，或者干脆改用频域/延迟补偿方案。
2. **把误差信号换成真实测量**：在单机拓扑下必须是"反馈式"——`d̂(n) = e(n) − Ŝ·y(n)`，且 `Ŝ` 必须来自上面的真实校准。
3. **给 `x̂(n)` 补上与 `e(n)` 匹配的时延对齐**，否则梯度恒为 0（见 3.3）。

### P1 —— 不改就会持续输出垃圾

4. 修 `start()` 里 `running_` 的竞态（先置标志再 `requestStart`，或在首帧回调里宽容处理）。
5. 双流漂移：要么用**单流全双工**（Oboe 的 `FullDuplex` / AAudio 双向流），要么做重采样 + 自适应水位控制。
6. 实时路径去分配：`micData` 改成预分配成员缓冲；校准数据采集挪出 RT 线程（用无锁队列）。

### P2 —— 工程质量

7. 把 `tests/` 接进 CI（`ctest`），并**给主收敛测试加上真正的断言**（例如 `NR >= 10 dB`、`|w| > 阈值`），让它有失败的能力。
8. 让 `fxlms_simulation.py` **与 C++ 实现共用同一套模型**（把 `e = d + S₂·y` 那步在 C++ 里也实现），否则仿真永远是"另一个算法"。
9. 清掉 `.tmp` 残留文件；`nativeReset()` 补实现；删掉或接通死代码（`generateAuxiliaryNoise` / `onlineUpdate`）。
10. PRD 的指标承诺需要按第 5 节重新标定。

---

## 7. 附：本次使用的复现命令

```powershell
# 真机
adb install -r -t apk-output\debug\app-debug.apk
adb shell am start -n com.anc.app/.ui.MainActivity
adb logcat -d -v time | Select-String "OboeEngine|ANC_JNI"
adb shell dumpsys notification --noredact | Select-String "android.text"
adb shell dumpsys media.audio_flinger | Select-String "FrmCnt|Active|Standby"

# 算法复刻仿真（需用系统 Python，它带 numpy）
& "C:\Users\User\AppData\Local\Programs\Python\Python313\python.exe" _diag_fxlms.py
& "C:\Users\User\AppData\Local\Programs\Python\Python313\python.exe" _diag_fxlms2.py
```

---

## 8. v4 修复记录（评审后实施）

> 本节记录针对第 6 节清单的修复结果。用户确认的目标：**单麦克风输入**，尽量做出可用的降噪效果。

### 8.1 算法结构：从"宽带反馈空转"改为物理可行的单麦方案

| 支路 | 原理 | 有效频段 | 对回环延迟 |
|------|------|----------|-----------|
| 宽带 FxLMS 反馈（IMC） | 参考取估计扰动 `d̂(n) = e(n) − Ŝ·y(n)`，误差用**真实测量** `e = mic` | 约 30–400 Hz | 受延迟限制 |
| **窄带谐波抵消器（主力）** | 内部合成参考 `A·cos(ωn+φ)` / `A·sin(ωn+φ)`，A∠φ = Ŝ 复数增益；每谐波复数权重 | 30–600 Hz 的稳态线谱 | **免疫**（环路增益归一化到 1） |

窄带支路是这次能"真正降下去"的关键：稳态噪声（风扇/电机/空调嗡鸣）能量集中在基频及谐波，用内部合成参考就不受"参考麦=误差麦"的因果性约束。

### 8.2 逐条对照第 6 节清单

**P0**
1. ✅ 次级路径辨识接通：`processCalibrationFrame` 不再传 `nullptr`，真实回环延迟经对数扫频 + 匹配滤波估出 IR；稀疏化（峰值定位 + 0.2% 尾部截断 + 抽头上限 512）；`secondaryPathLength` 1024→2048 覆盖共享模式缓冲延迟。新增**校准有效性检查**（峰太弱/位置越界则拒绝并保留旧模型）。
2. ✅ 误差信号改为真实测量 `e = mic`，参考用 `d̂ = e − Ŝ·y`（纯反馈式，符合单麦物理约束）。
3. ✅ `x̂` 与 `e` 时延对齐：改用**归一化** FxLMS（`μ = stepSize/(L·‖x̂‖²)`），并把次级路径建模为"纯延迟 + 有源支路"的稀疏结构，规避零延迟假设导致的梯度恒零。

**P1**
4. ✅ `running_` 竞态：`requestStart()` **之前**置位。
5. ✅ 双流漂移：弹性缓冲水位管理 + 时间规整（±1 采样滑移）；修掉了 `fillMicBlock` **滑移方向反了**的 bug（高水位应 +1 压缩 / 低水位 −1 拉伸）。
6. ✅ 实时路径去分配：扫描缓冲预分配（`kMaxScanPoints=256`）；校准在调用线程执行，`processCalibrationFrame` 不再在 RT 路径分配。

**P2**
7. ✅ `tests/` 已接入 CI（`dsp-test` job），断言真实化：显式稀疏卷积精确性、FxLMS 系统辨识收敛、长延迟（6 s / D=576）稳定性、频响一致性（相对容差 3%）。**CI 结果：9 passed, 0 failed。**
8. ✅ 仿真与实现对齐：仿真脚本落到 `sim/`（`anc_sim.py` / `step_scan.py` / `step_rule_check.py`），步长规则由仿真标定后写入实现。
9. ⚠️ 部分：`.tmp` 残留已清；PRD 指标需按第 5 节重新标定（待办）。

### 8.3 稳定性边界（仿真标定，`sim/results_step_rule_by_delay.txt`）

| 回环延迟 | D(采样) | 实测最大安全步长 | 规则取值 | 裕度 |
|---------|--------|----------------|---------|------|
| 2 ms | 96 | 0.005 | 0.002 | 2.50× |
| 3 ms | 144 | 0.005 | 0.002 | 2.50× |
| 6 ms | 288 | 0.007 | 0.002 | 3.50× |
| 10 ms | 480 | 0.004 | 0.002 | 2.00× |

最终规则：`tonalStep = clamp(1.0/D, 2e-4, 2e-3)`（与上表"规则"列一致）。
另有运行时守卫：检测到过度驱动/发散（`outRms>0.4` 或 `NR<−3dB`）则减半步长并重置，累计 4 次后**禁用窄带支路**防啸叫。

### 8.4 平台侧修正

- `Usage::VoiceCommunication` 会触发系统 AEC/NS/AGC，把误差信号抹掉并抵消反噪声 → 改用 `Media` + `InputPreset::VoiceRecognition`，并保留通信模式降级 plan。
- Manifest 增加 `FOREGROUND_SERVICE_MICROPHONE`，服务类型 `mediaPlayback|microphone`。

### 8.6 「降噪量」指标口径修正（v4.4）—— 从"模型推算"改为"实测 A/B"

**问题**：真机读到 `降噪量 5.4 dB`，但同时提示"次级路径未校准"。这暴露了指标本身的缺陷。

原口径（`audio_processor.cpp::updateStats`）：

```
NR = 10·log10( P(d̂) / P(e) ) ,  其中 d̂ = e − Ŝ·y
```

- 分母 `P(e)` 是**实测**的（麦克风真实残差）✔
- 分子 `P(d̂)` 是**模型推算**的。把 d̂ 展开：`d̂ = d + (S − Ŝ)·y`
  → 只要 `Ŝ ≠ S`（未校准），分子就混入 `ΔS·y` 这一**虚假项**。
- 而 `SecondaryPath::setDefault()`（`anc_core.cpp:108-112`）造的是一条
  **7.9 ms 延迟 + 合成高/低通尾**的假模型，并**显式 `calibrated_ = false`**。
  实测 `y` 已达满量程 55%（`AudioTrack maxAmplitude ≈ 1.19e9`），ΔS·y 很大
  → **分子被显著抬高 → 报出来的 dB 会虚高**。

结论：**旧口径的 5.4 dB 不能作为"真实降噪量"的证据。** 它混了模型误差。

**新口径（已实施）**：控制器起来前先**静音输出 0.5 s**，用同一路麦克风实测本底功率 `P0`，
之后上报

```
NR_measured = 10·log10( P0 / P(e) )
```

两侧都来自麦克风实采，**与 Ŝ 准不准完全无关**，是真正的"开/关"对比。
旧口径降级为诊断量 `nr_model_db_`（不再上报）。

注意：该值代表**麦克风所在那一点**的降噪量，不等于用户耳朵位置听到的效果。

### 8.5 遗留 / 待实测

- 真机端到端验证（NR>0、有声音输出、无啸叫）需在 `latest` release 的 debug APK 上实测确认。
- 第 5 节的产品定位仍适用：单麦手机方案的合理预期是**窄带稳态噪声 5–15 dB（近距离）**，而非"开放空间 10–25 dB 宽带静区"。

---

*报告结束*
