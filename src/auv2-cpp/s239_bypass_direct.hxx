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

#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
//
// §245：认参失败要能分格（清单中心＝认参），所以原来的 Counter_NoBypassParam 拆成
// get-list 失败／GetParameterInfo 失败／cfNameString 缺失／名字不匹配四格；另加宿主写
// 进门两格（SetParameter(bypass) 与 SetProperty(BypassEffect)）与 hint 成功拉取一格。
enum Counter : uint32_t
{
  Counter_RequestFlushEntered = 0,   // param_request_flush() 进门次数
  Counter_NotMainThread = 1,         // 因不在主线程而跳过
  Counter_GetListFailed = 2,         // GetParameterList 取不到列表
  Counter_GetInfoFailed = 3,         // GetParameterInfo 对某个参数返回错误
  Counter_CfNameMissing = 4,         // cfNameString 为空（或转不了 UTF-8）
  Counter_NameNoMatch = 5,           // 名字哈希没有一个对上
  Counter_HintIdMismatch = 6,        // hint 的标识不是我们认识的 bypass
  Counter_HintSymbolsMissing = 7,    // weak extern 解析失败（hint 模块未链接）
  Counter_HintReadInconsistent = 8,  // 读 hint 时 seq 变了，重试后仍不一致
  Counter_HostMoveInWindow = 9,      // 分歧来自宿主这一侧（防把宿主刚写的值倒灌回去），跳过
  Counter_AlreadyKnown = 10,         // 插件值 == AU 值，宿主已经知道，不发
  Counter_DirectSent = 11,           // 直发触发：onPerformEdit + 参数变化事件
  Counter_PropertyChanged = 12,      // kAudioUnitProperty_BypassEffect 发出
  Counter_Armed = 13,                // Initialize() 里建档次数
  Counter_InstancesBuilt = 14,       // 经本 mixin 构造出来的 AU 实例数
  Counter_SetParamBypassEntered = 15,   // SetParameter() 收到 bypass 参数写（宿主→插件）
  Counter_SetPropertyBypassEntered = 16, // SetProperty(BypassEffect) 进门（宿主→wrapper）
  Counter_HintPulled = 17,           // hint 三元读取一致（fork 侧成功拉取次数）
  Counter_Count = 18
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
      // §245：同一次一致读取在 arm 与直发里都发生；两处都算「fork 拉到了 hint」。
      note(Counter_HintPulled);
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
      // §245：宿主→插件方向的 bypass 参数写进门（SetBypassEffect 也会落到这里）。
      note(Counter_SetParamBypassEntered);
      fHostWriteValue.store(static_cast<double>(inValue), std::memory_order_relaxed);
      fHostWriteSeen.store(false, std::memory_order_relaxed);
    }
    return status;
  }

  // §245：宿主对 AU 说「旁通」的方式是 kAudioUnitProperty_BypassEffect。基类的
  // SetBypassEffect() 不是虚函数，但它最终走上面的 SetParameter()；这里再把属性入口
  // 单独记一格，两者都能在台架上读到。（只记，不改语义。）
  OSStatus SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                       AudioUnitElement inElement, const void *inData,
                       UInt32 inDataSize) override
  {
    if (inID == kAudioUnitProperty_BypassEffect && inScope == kAudioUnitScope_Global)
    {
      note(Counter_SetPropertyBypassEntered);
    }
    return Base::SetProperty(inID, inScope, inElement, inData, inDataSize);
  }

  // Plugin -> host. nice-plug calls this from the editor's parameter write, on the main thread.
  void param_request_flush() override
  {
    Base::param_request_flush();
    notifyHostIfBypassMoved();
  }

 private:
  // §245：用 GetParameterList/GetParameterInfo 按显示名哈希认 bypass。
  //
  // §244 无头枚举钉死两处缺陷：①AUv2 的 AudioUnitParameterInfo.name 按 API 契约恒为空，
  // 名字只在 cfNameString（clap-wrapper 每次调用都会 CFRetain 它）；②hint 常量是
  // FNV-1a **64** 的低 32 位，不是 32 位算法。修法只订正这两个字段/宽度，机制形状不变。
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
      note(Counter_HintIdMismatch);
      return;
    }

    // 现在用 GetParameterList 找显示名为 "Bypass" 的参数 ID。
    // AUv2 的 GetParameterList 返回所有参数 ID 列表。
    UInt32 paramListSize = 0;
    OSStatus err = this->GetParameterList(kAudioUnitScope_Global, nullptr, paramListSize);
    if (err != noErr || paramListSize == 0)
    {
      note(Counter_GetListFailed);
      return;
    }

    std::vector<AudioUnitParameterID> paramList(paramListSize);
    err = this->GetParameterList(kAudioUnitScope_Global, paramList.data(), paramListSize);
    if (err != noErr)
    {
      note(Counter_GetListFailed);
      return;
    }

    // 遍历参数列表。名字只认 cfNameString：name 字段按契约被包装层清零
    // （wrapasauv2.cpp 的 memset；SDK 注释 "name — UNUSED - set to zero"）。
    for (AudioUnitParameterID paramID : paramList)
    {
      AudioUnitParameterInfo paramInfo;
      std::memset(&paramInfo, 0, sizeof(paramInfo));
      err = this->GetParameterInfo(kAudioUnitScope_Global, paramID, paramInfo);
      if (err != noErr)
      {
        note(Counter_GetInfoFailed);
        continue;
      }
      if (paramInfo.cfNameString == nullptr)
      {
        note(Counter_CfNameMissing);
        continue;
      }

      // clap-wrapper 对该字段每次都 CFRetain；带 CFNameRelease 标志就必须由我们释放，
      // 否则每次 Initialize 泄漏 37 个 CFString。
      const bool release = (paramInfo.flags & kAudioUnitParameterFlag_CFNameRelease) != 0;
      char nameBuf[256];
      nameBuf[0] = 0;
      const Boolean converted = CFStringGetCString(paramInfo.cfNameString, nameBuf, sizeof(nameBuf),
                                                   kCFStringEncodingUTF8);
      if (release) CFRelease(paramInfo.cfNameString);

      if (!converted || nameBuf[0] == 0)
      {
        note(Counter_CfNameMissing);
        continue;
      }

      if (fnv1a_64_low32(nameBuf) == BYPASS_DISPLAY_NAME_HASH)
      {
        fBypassID = paramID;
        break;
      }
    }

    if (fBypassID == CLAP_INVALID_ID)
    {
      note(Counter_NameNoMatch);
      return;
    }

    fHostWriteSeen.store(true, std::memory_order_relaxed);
    fArmed.store(true, std::memory_order_relaxed);
    note(Counter_Armed);
  }

  // §245：FNV-1a **64** 的低 32 位——与 hint 模块的 64-bit 实现逐位一致。
  // （上一版误用 32 位算法：FNV-1a 32("Bypass") = 0x34540dcd ≠ 0x0146944d。）
  static uint32_t fnv1a_64_low32(const char *str)
  {
    uint64_t hash = 14695981039346656037ull;
    for (const char *p = str; *p; ++p)
    {
      hash ^= static_cast<uint64_t>(static_cast<uint8_t>(*p));
      hash *= 1099511628211ull;
    }
    return static_cast<uint32_t>(hash & 0xffffffffull);
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
