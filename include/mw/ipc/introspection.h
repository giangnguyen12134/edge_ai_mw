// Copyright 2026 Genio Middleware Authors.
//
// D-Bus introspection XML of the services. The definitions are generated at
// build time from dbus/*.xml (see cmake/EmbedText.cmake).

#ifndef MW_IPC_INTROSPECTION_H_
#define MW_IPC_INTROSPECTION_H_

namespace mw {
namespace dbus {

extern const char kMediaIntrospectionXml[];
extern const char kNpuIntrospectionXml[];

}  // namespace dbus
}  // namespace mw

#endif  // MW_IPC_INTROSPECTION_H_
