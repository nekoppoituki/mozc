// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef MOZC_BASE_WIN32_COM_IMPLEMENTS_H_
#define MOZC_BASE_WIN32_COM_IMPLEMENTS_H_

#include <guiddef.h>
#include <objbase.h>
#include <unknwn.h>
#include <windows.h>

#include <atomic>
#include <type_traits>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"

namespace mozc::win32 {
namespace com_implements_internal {

// Reference counter for the COM module. Use CanComModuleUnloadNow() to
// determine if the COM module can unload safely.
ABSL_CONST_INIT extern std::atomic<int> com_module_ref_count;

}  // namespace com_implements_internal

// Returns true if the COM module doesn't have any active objects.
// Simply call this function to implement DllCanUnloadNow().
// Note that the return value is HRESULT, so S_FALSE is 1.
HRESULT CanComModuleUnloadNow();

// This is the default implementation of IsIIDOf. If the COM interface derives
// another COM interface (not IUnknown), explicitly define an overload of
// IsIIDOf.
// For example, ITfLangBarItemButton derives from ITfLangBarItem. In order to
// answer QueryInterface() for IID_ITfLangBarItem, define:
//
// template<>
// IsIIDOf<ITfLangBarItemButton>(REFIID riid) {
//   return IsIIDOf<ITfLangBarItemButton, ITfLangBarItem>(riid);
// }
//
// This way, IsIIDOf<ITfLangBarItemButton>() will check if
// `riid` is for one of the specified interface types (ITfLangBarItemButton and
// ITfLangBarItem in this example).
template <typename... Interfaces>
bool IsIIDOf(REFIID riid) {
  return (... || IsEqualIID(riid, __uuidof(Interfaces)));
}

// ComImplements is the base class for COM implementation classes and implements
// the IUnknown methods.
//
// class FooBar : public ComImplements<IFoo, IBar> {
//  ...
// }
//
// Note that you need to define a specialization of IsIIDOf<IFoo>() if IFoo
// is not an immediate derived interface of IUnknown.
template <typename... Interfaces>
class ComImplements : public Interfaces... {
 public:
  static_assert(std::conjunction_v<std::is_base_of<IUnknown, Interfaces>...>,
                "COM interfaces must derive from IUnknown.");

  ComImplements() { ++com_implements_internal::com_module_ref_count; }
  virtual ~ComImplements() { --com_implements_internal::com_module_ref_count; }

  // IUnknown methods.
  STDMETHODIMP_(ULONG) AddRef() final;
  STDMETHODIMP_(ULONG) Release() final;
  STDMETHODIMP QueryInterface(REFIID riid, void **out) final;

 private:
  template <typename T, typename... Rest>
  void *QueryInterfaceImpl(REFIID riid);

  std::atomic<ULONG> ref_count_;
};

template <typename... Interfaces>
STDMETHODIMP_(ULONG)
ComImplements<Interfaces...>::AddRef() {
  return ++ref_count_;
}

template <typename... Interfaces>
STDMETHODIMP_(ULONG)
ComImplements<Interfaces...>::Release() {
  const ULONG new_value = --ref_count_;
  if (new_value == 0) {
    delete this;
  }
  return new_value;
}

template <typename... Interfaces>
STDMETHODIMP ComImplements<Interfaces...>::QueryInterface(REFIID riid,
                                                          void **out) {
  if (out == nullptr) {
    return E_POINTER;
  }
  *out = QueryInterfaceImpl<Interfaces...>(riid);
  if (*out == nullptr) {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

template <typename... Interfaces>
template <typename T, typename... Rest>
void *ComImplements<Interfaces...>::QueryInterfaceImpl(REFIID riid) {
  if (IsIIDOf<T>(riid)) {
    return absl::implicit_cast<T *>(this);
  }
  if constexpr (sizeof...(Rest) == 0) {
    // This is the last QueryInterfaceImpl in the list. Check for IUnknown.
    if (IsIIDOf<IUnknown>(riid)) {
      return absl::implicit_cast<IUnknown *>(absl::implicit_cast<T *>(this));
    }
    return nullptr;
  } else {
    // Continue the lookup.
    return QueryInterfaceImpl<Rest...>(riid);
  }
}

}  // namespace mozc::win32

#endif  // MOZC_BASE_WIN32_COM_IMPLEMENTS_H_