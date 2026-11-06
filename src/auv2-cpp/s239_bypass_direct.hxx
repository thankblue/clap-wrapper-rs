#pragma once

// ---------------------------------------------------------------------------------------------
// §239 甲-1 落码：AUv2 的 bypass 通报在主线程直发（hint 反向拉取形状）
// ---------------------------------------------------------------------------------------------
//
// 症状：面板拨 bypass 后，宿主（LUNA）的 bypass 钮与听感不跟随，除非传输在跑。
//
// 定位（§235 结论，§238 实测）：setupWrapperSpecifics 是基类构造期的虚调用，mixin 在那儿
// 取不到 clap_plugin_t*（探针 imgs=0 钉死）。CLAP 把「插件自己动过的参数」只能作为 output
// event 在 process() 或 params.flush() 栈里吐给宿主，于是送达只发生在宿主驱动的栈内。
//
// 机制（§239 甲-1 批准形状）：
//   * 插件侧（auv2-export 的 hint 模块）以原子三元（seq／标识／value）递值；
//   * 本 mixin 以 weak extern 反向拉取 hint 符号（无符号即整条路径自然失效）；
//   * wrapper 侧只用公开虚函数 GetParameterList/GetParameterInfo 按显示名认 bypass，
//     不碰私有成员、不动子模块；
//   * 失败安全：无匹配 ⇒ 路径惰性＋原因码计数，不许猜、不许 panic。
//
// 作用面（§237.1 要求以 diff 面证明的界，沿用至 §239）：
//   * 只读 bypass 参数（按显示名哈希识别）；其他参数一概不动。
//   * 不调 clap_plugin_params_t::flush()，不碰 output_parameter_events，不碰 process adapter。
//   * 基类 param_request_flush() 照常先执行（`_requestedFlush = true` 语义不变）。
//   * 只在主线程发（pthread_main_np），渲染线程上的 request_flush 一律跳过。
//
// 挂在哪儿：不改 free-audio/clap-wrapper 子模块。子模块的 wrapasauv2.cpp 第一行就 include
// 本目录的 generated_entrypoints.hxx，而那个文件本来就以 `struct wrapAsAUV2_inst##n : WrapAsAUV2`
// 派生实例类，所以「改 WrapAsAUV2」这件事在本 fork 的自有文件里就能做完。
//
// 下面标了 TEMPORARY 的计数段是 §237.1 要求的临时插桩（装机复验失败时自带证据），
// §218 关账后整段删除；删它不需要动上面那条直发逻辑。

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "clap/events.h"
#include "clap/ext/params.h"

// ── §239 甲-1：weak extern 声明 hint 符号 ─────────────────────────────────────
//
// 这些符号由 auv2-export 的 hint 模块（crates/plugin/src/hint.rs）导出。
// 如果插件没链接 hint 模块（例如旧版本或 VST3 镜像），符号不存在，weak extern
// 解析为 nullptr，整条直发路径自然失效（惰性）。
extern "C"
{
  extern uint64_t nice_au_hint_seq() __attribute__((weak));
  extern uint32_t nice_au_hint_id() __attribute__((weak));
  extern float nice_au_hint_value() __attribute__((weak));
  extern uint64_t nice_au_hint_reason(uint32_t index) __attribute__((weak));
  extern uint32_t nice_au_hint_reason_count() __attribute__((weak));
}

namespace nice_s238
{

// §239 甲-1：bypass 显示名 "Bypass" 的 FNV-1a 64-bit 哈希（低 32 位）。
// 与 crates/plugin/src/hint.rs 里的 BYPASS_DISPLAY_NAME_HASH 保持一致。
constexpr uint32_t BYPASS_DISPLAY_NAME_HASH = 0x0146944d;

// TEMPORARY(§237.1): fork 侧计数，由 Rust 探针读走写进同一份探针日志。§218 关账后删除。
enum Counter : uint32_t
{
  Counter_RequestFlushEntered = 0,  // param_request_flush() 进门次数
  Counter_NotMainThread = 1,        // 因不在主线程而跳过
  Counter_NoBypassParam = 2,        // 没扫到显示名匹配的 bypass 参数
  Counter_HintSymbolsMissing = 3,   // weak extern 解析失败（hint 模块未链接）
  Counter_HintReadInconsistent = 4, // 读 hint 时 seq 变了，重试后仍不一致
  Counter_HostMoveInWindow = 5,     // 分歧来自宿主这一侧（防把宿主刚写的值倒灌回去），跳过
  Counter_AlreadyKnown = 6,         // 插件值 == AU 值，宿主已经知道，不发
  Counter_DirectSent = 7,           // 直发触发：Globals()->SetParameter + 参数变化事件
  Counter_PropertyChanged = 8,      // kAudioUnitProperty_BypassEffect 发出
  Counter_Armed = 9,                // Initialize() 里建档次数
  Counter_InstancesBuilt = 10,      // 经本 mixin 构造出来的 AU 实例数
  Counter_Count = 11
};

inline std::atomic<uint64_t> &counter(uint32_t index)
{
  static std::atomic<uint64_t> counters[Counter_Count];
  return counters[index < Counter_Count ? index : 0];
}

inline void note(uint32_t index)
{
  if (index < Counter_Count) counter(index).fetch_add(1, std::memory_order_relaxed);
}

// §239 甲-1：检查 hint 符号是否可用。
inline bool hintSymbolsAvailable()
{
  return nice_au_hint_seq != nullptr && nice_au_hint_id != nullptr &&
         nice_au_hint_value != nullptr;
}

// §239 甲-1：读 hint 值，带一致性校验（seq 前后比对）。
// 返回 true 表示读到一致值，false 表示不一致或符号缺失。
inline bool readHintConsistent(uint32_t &outId, float &outValue)
{
  if (!hintSymbolsAvailable())
  {
    note(Counter_HintSymbolsMissing);
    return false;
  }
  // 最多重试 3 次（seq 在读的过程中变了就重读）。
  for (int retry = 0; retry < 3; ++retry)
  {
    uint64_t seqBefore = nice_au_hint_seq();
    uint32_t id = nice_au_hint_id();
    float value = nice_au_hint_value();
    uint64_t seqAfter = nice_au_hint_seq();
    if (seqBefore == seqAfter)
    {
      outId = id;
      outValue = value;
      return true;
    }
    // seq 变了，重试。
  }
  note(Counter_HintReadInconsistent);
  return false;
}

// Base = free_audio::auv2_wrapper::WrapAsAUV2
template <typename Base>
class BypassDirect : public Base
{
 public:
  explicit BypassDirect(const std::string &clapname, const std::string &clapid, int idx,
                        AudioComponentInstance ci)
      : Base(clapname, clapid, idx, ci)
  {
    note(Counter_InstancesBuilt);
  }

  // §239 甲-1：不再依赖 setupWrapperSpecifics 取 clap_plugin_t*（那条路在构造期虚分派，
  // §238 实测取不到）。改为在 Initialize() 里用 GetParameterList 按显示名哈希认 bypass。

  OSStatus Initialize() override
  {
    const OSStatus status = Base::Initialize();
    if (status == noErr) arm();
    return status;
  }

  void Cleanup() override
  {
    fBypassID = CLAP_INVALID_ID;
    fArmed.store(false, std::memory_order_relaxed);
    Base::Cleanup();
  }

  // Host -> plugin. Recorded only, never modified.
  OSStatus SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                        AudioUnitParameterValue inValue, UInt32 inBufferOffsetInFrames) override
  {
    const OSStatus status =
        Base::SetParameter(inID, inScope, inElement, inValue, inBufferOffsetInFrames);

    if (inScope == kAudioUnitScope_Global && inID == fBypassID && fBypassID != CLAP_INVALID_ID)
    {
      fHostWriteValue.store(static_cast<double>(inValue), std::memory_order_relaxed);
      fHostWriteSeen.store(false, std::memory_order_relaxed);
    }
    return status;
  }

  // Plugin -> host. nice-plug calls this from the editor's parameter write, on the main thread.
  void param_request_flush() override
  {
    Base::param_request_flush();
    notifyHostIfBypassMoved();
  }

 private:
  // §239 甲-1：用 GetParameterList/GetParameterInfo 按显示名哈希认 bypass。
  void arm()
  {
    // 先检查 hint 符号是否可用。
    if (!hintSymbolsAvailable())
    {
      note(Counter_HintSymbolsMissing);
      return;
    }

    // 用 hint 模块的显示名哈希校验：读 hint id，与本地常量比对。
    uint32_t hintId = 0;
    float hintValue = 0.0f;
    if (!readHintConsistent(hintId, hintValue))
    {
      // 读不一致或符号缺失，路径惰性。
      return;
    }
    if (hintId != BYPASS_DISPLAY_NAME_HASH)
    {
      // 显示名哈希不匹配，不是我们认识的 bypass 参数，路径惰性。
      note(Counter_NoBypassParam);
      return;
    }

    // 现在用 GetParameterList 找显示名为 "Bypass" 的参数 ID。
    // AUv2 的 GetParameterList 返回所有参数 ID 列表。
    UInt32 paramListSize = 0;
    OSStatus err = this->GetParameterList(kAudioUnitScope_Global, nullptr, paramListSize);
    if (err != noErr || paramListSize == 0)
    {
      note(Counter_NoBypassParam);
      return;
    }

    std::vector<AudioUnitParameterID> paramList(paramListSize);
    err = this->GetParameterList(kAudioUnitScope_Global, paramList.data(), paramListSize);
    if (err != noErr)
    {
      note(Counter_NoBypassParam);
      return;
    }

    // 遍历参数列表，用 GetParameterInfo 读显示名，比对哈希。
    for (AudioUnitParameterID paramID : paramList)
    {
      AudioUnitParameterInfo paramInfo;
      err = this->GetParameterInfo(kAudioUnitScope_Global, paramID, paramInfo);
      if (err != noErr) continue;

      // 计算显示名的 FNV-1a 哈希（低 32 位）。
      uint32_t nameHash = fnv1a_32(paramInfo.name);
      if (nameHash == BYPASS_DISPLAY_NAME_HASH)
      {
        fBypassID = paramID;
        break;
      }
    }

    if (fBypassID == CLAP_INVALID_ID)
    {
      note(Counter_NoBypassParam);
      return;
    }

    fHostWriteSeen.store(true, std::memory_order_relaxed);
    fArmed.store(true, std::memory_order_relaxed);
    note(Counter_Armed);
  }

  // FNV-1a 32-bit 哈希（与 hint 模块的 64-bit 哈希低 32 位一致）。
  static uint32_t fnv1a_32(const char *str)
  {
    uint32_t hash = 2166136261u;
    for (const char *p = str; *p; ++p)
    {
      hash ^= static_cast<uint8_t>(*p);
      hash *= 16777619u;
    }
    return hash;
  }

  void notifyHostIfBypassMoved()
  {
    note(Counter_RequestFlushEntered);

    if (!fArmed.load(std::memory_order_relaxed)) return;

    if (pthread_main_np() == 0)
    {
      note(Counter_NotMainThread);
      return;
    }

    // §239 甲-1：从 hint 模块读 bypass 值（带一致性校验）。
    uint32_t hintId = 0;
    float pluginValueFloat = 0.0f;
    if (!readHintConsistent(hintId, pluginValueFloat))
    {
      // 读不一致，跳过本次（下次 request_flush 再试）。
      return;
    }
    double pluginValue = static_cast<double>(pluginValueFloat);

    const double auValue = static_cast<double>(this->Globals()->GetParameter(fBypassID));

    const bool sawHostWrite = !fHostWriteSeen.exchange(true, std::memory_order_relaxed);
    if (sawHostWrite && pluginValue != fHostWriteValue.load(std::memory_order_relaxed))
    {
      note(Counter_HostMoveInWindow);
      return;
    }

    if (pluginValue == auValue)
    {
      note(Counter_AlreadyKnown);
      return;
    }

    clap_event_param_value_t event{};
    event.header.size = sizeof(event);
    event.header.time = 0;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_PARAM_VALUE;
    event.header.flags = 0;
    event.param_id = fBypassID;
    event.value = pluginValue;

    this->onPerformEdit(&event);
    note(Counter_DirectSent);

    this->PropertyChanged(kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0);
    note(Counter_PropertyChanged);
  }

  clap_id fBypassID{CLAP_INVALID_ID};
  std::atomic<bool> fArmed{false};
  std::atomic<double> fHostWriteValue{0.0};
  std::atomic<bool> fHostWriteSeen{true};
};

} // namespace nice_s238

// TEMPORARY(§237.1): 探针读计数的入口。§218 关账后删除。
extern "C" uint64_t nice_auv2_s238_counter(uint32_t index)
{
  return nice_s238::counter(index).load(std::memory_order_relaxed);
}

extern "C" uint32_t nice_auv2_s238_counter_count() { return nice_s238::Counter_Count; }
