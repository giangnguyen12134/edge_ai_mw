// Copyright 2026 Genio Middleware Authors.
//
// Small utility macros shared by all modules.

#ifndef MW_COMMON_MACROS_H_
#define MW_COMMON_MACROS_H_

// Deletes the copy constructor and copy assignment operator. Place in the
// public section of a class.
#define MW_DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&) = delete;         \
  TypeName& operator=(const TypeName&) = delete

#endif  // MW_COMMON_MACROS_H_
