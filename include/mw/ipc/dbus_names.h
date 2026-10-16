// Copyright 2026 VMX Middleware Team.
//
// Bus names, object paths, interfaces and members of the middleware wire
// protocol. The authoritative description lives in dbus/*.xml.

#ifndef MW_IPC_DBUS_NAMES_H_
#define MW_IPC_DBUS_NAMES_H_

namespace mw {
namespace dbus {

// MediaManager: owns the camera, the microphone and the DMA-BUF frame pool.
const char kMediaBusName[] = "org.vmx_mw.MediaManager";
const char kMediaObjectPath[] = "/org/vmxmw/MediaManager";
const char kMediaInterface[] = "org.vmx_mw.MediaManager1";

const char kMediaSubscribeVideo[] = "SubscribeVideo";
const char kMediaSubscribeAudio[] = "SubscribeAudio";
const char kMediaUnsubscribe[] = "Unsubscribe";
const char kMediaReleaseFrame[] = "ReleaseFrame";
const char kMediaListStreams[] = "ListStreams";
const char kMediaGetStats[] = "GetStats";
const char kMediaFrameReady[] = "FrameReady";
const char kMediaStreamState[] = "StreamStateChanged";

// NpuManager: owns the NPU and all models.
const char kNpuBusName[] = "org.vmx_mw.NpuManager";
const char kNpuObjectPath[] = "/org/vmxmw/NpuManager";
const char kNpuInterface[] = "org.vmx_mw.NpuManager1";

const char kNpuListModels[] = "ListModels";
const char kNpuSetModelEnabled[] = "SetModelEnabled";
const char kNpuEnrollSpeaker[] = "EnrollSpeaker";
const char kNpuRemoveSpeaker[] = "RemoveSpeaker";
const char kNpuListSpeakers[] = "ListSpeakers";
const char kNpuGetStats[] = "GetStats";
const char kNpuResult[] = "Result";

// Errors carry the name "org.genio.Error.<StatusCodeName>".
const char kErrorPrefix[] = "org.vmx_mw.Error.";

}  // namespace dbus
}  // namespace mw

#endif  // MW_IPC_DBUS_NAMES_H_
