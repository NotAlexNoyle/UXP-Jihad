/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Jihad (T-633): force-included into ICU, which is built without the STL wrappers (DISABLE_STL_WRAPPING) under
 * gcc_hidden.h's "visibility push(hidden)". GCC 12's libstdc++ declares std::type_info::operator== out of line on
 * ARM EABI, so ICU's typeid comparisons (numfmt.cpp) referenced it as a HIDDEN undefined symbol and gold refused the
 * link ("hidden symbol '_ZNKSt9type_infoeqERKS_' is not defined locally"). Declared here first, with default
 * visibility, as every wrapped file already sees it. GCC 9's inline operator== is unaffected. */
#if defined(__cplusplus) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC visibility push(default)
#include <typeinfo>
#pragma GCC visibility pop
#endif
