#! /bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

MOZ_APP_NAME=xulrunner
MOZ_APP_DISPLAYNAME=XULRunner
MOZ_APP_VERSION=$MOZILLA_VERSION
MOZ_XULRUNNER=1

# Jihad: AppCompat GUID (Pale Moon's UXP_APPCOMPAT_GUID feature, palemoon/confvars.sh:71).
# Our app GUID is frozen and named by ZERO existing extensions, so without this every real XPI
# arrives appDisabled and the web-install path treats that as a failed install
# (amWebInstallListener.js:112-115) -- i.e. XPI support is structurally unreachable. With it,
# XPIProvider.jsm accepts a targetApplication naming extensions.guid.appCompatId and
# substitutes extensions.guid.appCompatVersion (XPIProvider.jsm:6357-6377, extensions only --
# themes are deliberately excluded upstream). Basilisk solves the same problem by shipping
# Firefox's GUID outright; that is not open to us, the app ID is frozen by user decision.
UXP_APPCOMPAT_GUID=1
