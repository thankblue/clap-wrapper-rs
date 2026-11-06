#pragma once
#include "detail/auv2/auv2_base_classes.h"
#include "s239_bypass_direct.hxx"

// The instance classes gain one more layer (nice_s238::BypassDirect) between them and
// WrapAsAUV2; see that file for what it adds and why it can live entirely in this repository.
#define CLAP_AUV2_ENTRY(n)                                                                    \
    struct wrapAsAUV2_inst##n : nice_s238::BypassDirect<free_audio::auv2_wrapper::WrapAsAUV2> \
    {                                                                                         \
        wrapAsAUV2_inst##n(AudioComponentInstance ci)                                         \
            : nice_s238::BypassDirect<free_audio::auv2_wrapper::WrapAsAUV2>(                  \
                  "", "", n, ci) {}                                                           \
    };                                                                                        \
    AUSDK_COMPONENT_ENTRY(ausdk::AUMusicDeviceFactory, wrapAsAUV2_inst##n);

CLAP_AUV2_ENTRY(0);
CLAP_AUV2_ENTRY(1);
CLAP_AUV2_ENTRY(2);
CLAP_AUV2_ENTRY(3);
