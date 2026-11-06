#pragma once

// ---------------------------------------------------------------------------------------------
// §237.1 甲案落码：AUv2 的 bypass 通报在主线程直发（ai-mastering-app handoffs §218/§230-§237）
// ---------------------------------------------------------------------------------------------
//
// 症状：面板拨 bypass 后，宿主（LUNA）的 bypass 钮与听感不跟随，除非传输在跑。
//
// 定位（§235 结论，jim §237.1 采信）：CLAP 把「插件自己动过的参数」只能作为 output event 在
// process() 或 params.flush() 栈里吐给宿主（见 wrapasauv2.cpp 里 onIdle() 上方那段长注释）。
// 于是送达只发生在宿主驱动的 process/flush 两条栈内：传输一停、宿主不再拉 onIdle（α）或拉了但
// 没促成 enable（β），通报就出不去。VST3／JUCE／Steinberg 的 AU wrapper 不走这条路——它们在
// 编辑器回调栈里直接 AUParameterSet/AUEventListenerNotify。本文件给 AUv2 补上同构的那一步，
// 且只给 bypass 这一个参数补。
//
// 作用面（§237.1 要求以 diff 面证明的界）：
//   * 只读 bypass 参数（用 CLAP_PARAM_IS_BYPASS 扫出来，不硬编码 id）；其他参数一概不动。
//   * 不调 clap_plugin_params_t::flush()，不碰 output_parameter_events，不碰 process adapter。
//   * 基类 param_request_flush() 照常先执行（`_requestedFlush = true` 语义不变），onIdle() 的
//     三块定标启发式与其中那条自动化 flush 一字未改。
//   * 只在主线程发（pthread_main_np），渲染线程上的 request_flush 一律跳过——那条路径上
//     process() 末尾本来就会把事件吐出去。
//
// 挂在哪儿：不改 free-audio/clap-wrapper 子模块。子模块的 wrapasauv2.cpp 第一行就 include
// 本目录的 generated_entrypoints.hxx（-I ./src/auv2-cpp 排在子模块 include 之前），而那个文件
// 本来就以 `struct wrapAsAUV2_inst##n : WrapAsAUV2` 派生实例类，所以「改 WrapAsAUV2」这件事
// 在本 fork 的自有文件里就能做完。这里用同样的宏再套一层 mixin。
//
// 下面标了 TEMPORARY 的计数段是 §237.1 要求的临时插桩（装机复验失败时自带证据），
// §218 关账后整段删除；删它不需要动上面那条直发逻辑。

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "clap/events.h"
#include "clap/ext/params.h"

namespace nice_s238
{

// TEMPORARY(§237.1): fork 侧计数，由 Rust 探针读走写进同一份探针日志。§218 关账后删除。
enum Counter : uint32_t
{
  Counter_RequestFlushEntered = 0, // param_request_flush() 进门次数
  Counter_NotMainThread = 1,       // 因不在主线程而跳过
  Counter_NoBypassParam = 2,       // 没拿到 params ext 或没扫到 CLAP_PARAM_IS_BYPASS
  Counter_GetValueFailed = 3,      // 插件侧读值失败
  Counter_HostMoveInWindow = 4,    // 分歧来自宿主这一侧（防把宿主刚写的值倒灌回去），跳过
  Counter_AlreadyKnown = 5,        // 插件值 == AU 值，宿主已经知道，不发
  Counter_DirectSent = 6,          // 直发触发：Globals()->SetParameter + 参数变化事件
  Counter_PropertyChanged = 7,     // kAudioUnitProperty_BypassEffect 发出（宿主 bypass 钮只跟这个）
  Counter_Armed = 8,               // Initialize() 里建档（扫 id + 立基线）次数
  Counter_InstancesBuilt = 9,      // 经本 mixin 构造出来的 AU 实例数
  Counter_Count = 10
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

// Base = free_audio::auv2_wrapper::WrapAsAUV2
template <typename Base>
class BypassDirect : public Base
{
 public:
  // Same 4-argument constructor the generated entry points already call
  // (WrapAsAUV2("", "", idx, componentInstance) since v0.17.0). Written out rather than
  // inherited so the instance count has somewhere to live.
  explicit BypassDirect(const std::string &clapname, const std::string &clapid, int idx,
                        AudioComponentInstance ci)
      : Base(clapname, clapid, idx, ci)
  {
    note(Counter_InstancesBuilt);
  }

  // The only place an AU instance ever learns its clap_plugin_t: Clap::Plugin::initialize()
  // hands it to the host through this virtual before it sets up busses and parameters
  // (clap_proxy.cpp:324). The base override is not final, and it must run first: it is what
  // publishes the factory presets.
  void setupWrapperSpecifics(const clap_plugin_t *plugin) override
  {
    Base::setupWrapperSpecifics(plugin);

    fPlugin = plugin;
    fParams = nullptr;
    fBypassID = CLAP_INVALID_ID;
    if (plugin != nullptr)
    {
      fParams = static_cast<const clap_plugin_params_t *>(
          plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    }
  }

  // Everything is settled by now: the parameter tree is built, Globals() holds the values the
  // base pushed in setupParameters(), and the plugin has been activated. Take the bypass
  // parameter's identity and the matching baseline here rather than at the first request_flush,
  // so a host write that landed before the plugin caught up cannot be mistaken for a panel move.
  OSStatus Initialize() override
  {
    const OSStatus status = Base::Initialize();
    if (status == noErr) arm();
    return status;
  }

  void Cleanup() override
  {
    // Drop the borrowed plugin pointer before the base destroys the CLAP instance.
    fParams = nullptr;
    fPlugin = nullptr;
    fBypassID = CLAP_INVALID_ID;
    fArmed.store(false, std::memory_order_relaxed);
    Base::Cleanup();
  }

  // Host -> plugin. Recorded only, never modified: the base still queues the event exactly as
  // it did, with its buffer offset, and still sets _requestedFlush.
  OSStatus SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                        AudioUnitParameterValue inValue, UInt32 inBufferOffsetInFrames) override
  {
    const OSStatus status =
        Base::SetParameter(inID, inScope, inElement, inValue, inBufferOffsetInFrames);

    if (inScope == kAudioUnitScope_Global && inID == fBypassID && fBypassID != CLAP_INVALID_ID)
    {
      // This runs on the render thread too (AUBase::ScheduleParameter), so the pair has to be
      // lock-free. A torn read here can only cost one skipped direct send, and the deferred
      // flush path still delivers it.
      fHostWriteValue.store(static_cast<double>(inValue), std::memory_order_relaxed);
      fHostWriteSeen.store(false, std::memory_order_relaxed);
    }
    return status;
  }

  // Plugin -> host. nice-plug calls this from the editor's parameter write, on the main thread,
  // right after the plugin's own value has changed. The base's deferral is left fully in place.
  void param_request_flush() override
  {
    Base::param_request_flush();
    notifyHostIfBypassMoved();
  }

 private:
  void arm()
  {
    if (fPlugin == nullptr || fParams == nullptr) return;

    // Same discovery the base uses in setupParameters() (wrapasauv2.cpp:650): ask the plugin
    // which parameter carries CLAP_PARAM_IS_BYPASS. Nothing here is specific to one product.
    clap_id bypassID = CLAP_INVALID_ID;
    const uint32_t count = fParams->count(fPlugin);
    for (uint32_t index = 0; index < count; ++index)
    {
      clap_param_info_t info;
      if (!fParams->get_info(fPlugin, index, &info)) continue;
      if ((info.flags & CLAP_PARAM_IS_BYPASS) != 0)
      {
        bypassID = info.id;
        break;
      }
    }
    if (bypassID == CLAP_INVALID_ID)
    {
      note(Counter_NoBypassParam);
      return;
    }
    fBypassID = bypassID;

    double value = 0.0;
    if (!fParams->get_value(fPlugin, fBypassID, &value))
    {
      fBypassID = CLAP_INVALID_ID;
      note(Counter_GetValueFailed);
      return;
    }
    fHostWriteSeen.store(true, std::memory_order_relaxed);
    fArmed.store(true, std::memory_order_relaxed);
    note(Counter_Armed);
  }

  void notifyHostIfBypassMoved()
  {
    note(Counter_RequestFlushEntered);

    if (!fArmed.load(std::memory_order_relaxed)) return;

    // The point of the whole exercise: an AU property change is main-thread work, and the host
    // only re-reads the bypass property from its own main thread. Anything arriving from the
    // render thread is already covered by the events process() pushes at its end.
    if (pthread_main_np() == 0)
    {
      note(Counter_NotMainThread);
      return;
    }

    double pluginValue = 0.0;
    if (!fParams->get_value(fPlugin, fBypassID, &pluginValue))
    {
      note(Counter_GetValueFailed);
      return;
    }
    const double auValue = static_cast<double>(this->Globals()->GetParameter(fBypassID));

    // Has the plugin moved, or is it merely behind a write the host made? A host write leaves
    // Globals() ahead of the plugin, and until the plugin has caught up with that write a
    // divergence proves nothing: sending the plugin's stale value back would hand the host a
    // bypass state it had just set - the exact wrong-way flicker this whole path is meant to
    // remove. So the first request_flush after an unobserved host write stands down, and the
    // flag is consumed either way, which means the next one (any later panel write) decides
    // normally. Trading an immediate send for that one write is deliberate: the loss is bounded
    // and falls back on the paths that already exist (onIdle's deferred flush, the intent re-sync
    // at activate), whereas the clobber would be a new regression in the Enable cells.
    const bool sawHostWrite = !fHostWriteSeen.exchange(true, std::memory_order_relaxed);
    if (sawHostWrite && pluginValue != fHostWriteValue.load(std::memory_order_relaxed))
    {
      note(Counter_HostMoveInWindow);
      return;
    }

    if (pluginValue == auValue)
    {
      // The host already has this value, whatever the direction of travel.
      note(Counter_AlreadyKnown);
      return;
    }

    // Bypass is a two-state parameter and the base relies on that too: noteBypassParameterValue()
    // (wrapasauv2.cpp:839) tests the midpoint of the declared range, and SetBypassEffect() writes
    // exactly min/max. A plain != is therefore not a float-rounding hazard here, and the send is
    // idempotent if it ever fires spuriously.

    clap_event_param_value_t event{};
    event.header.size = sizeof(event);
    event.header.time = 0;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_PARAM_VALUE;
    event.header.flags = 0;
    event.param_id = fBypassID;
    event.value = pluginValue;

    // The ready-made direct send: Globals()->SetParameter + noteBypassParameterValue +
    // AUEventListenerNotify(kAudioUnitEvent_ParameterValueChange) (wrapasauv2.cpp:1870-1892).
    this->onPerformEdit(&event);
    note(Counter_DirectSent);

    // And the part the queued path could never do while the host stood still: the host's bypass
    // button follows kAudioUnitProperty_BypassEffect and nothing else (see the comment above
    // onIdle()'s _bypassChanged branch). onIdle() would send this too, whenever it comes - which
    // is precisely what §230-§235 measured as unreliable. Send it here as well; a duplicate
    // property change with the same value is harmless and the host reads the same answer.
    this->PropertyChanged(kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0);
    note(Counter_PropertyChanged);
  }

  const clap_plugin_t *fPlugin{nullptr};
  const clap_plugin_params_t *fParams{nullptr};
  clap_id fBypassID{CLAP_INVALID_ID};
  std::atomic<bool> fArmed{false};
  std::atomic<double> fHostWriteValue{0.0};
  std::atomic<bool> fHostWriteSeen{true};
};

} // namespace nice_s238

// TEMPORARY(§237.1): the probe in the product reads the counters through these and appends them
// to its own log file, which is the one place §237.2 says evidence has to land. Remove with the
// counters after §218 closes.
extern "C" uint64_t nice_auv2_s238_counter(uint32_t index)
{
  return nice_s238::counter(index).load(std::memory_order_relaxed);
}

extern "C" uint32_t nice_auv2_s238_counter_count() { return nice_s238::Counter_Count; }
