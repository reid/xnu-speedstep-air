#!/bin/bash

VERSION=$( cat Source/IntelEnhancedSpeedStep.xcodeproj/project.pbxproj | grep -m 1 CURRENT_PROJECT_VERSION )
VERSION=${VERSION##* }
VERSION=${VERSION%%;*}

tar cvjf IntelEnhancedSpeedStep-${VERSION}.tar.bz2 \
  --exclude .hgignore \
  --exclude *.pbxuser* \
  --exclude *.mode1v3* \
  --exclude *.orig \
  --exclude Source/build \
  --exclude .DS_Store \
  --exclude ._.DS_Store \
  --exclude ._IntelEnhancedSpeedStep.cpp \
  --exclude ._IntelEnhancedSpeedStep.h \
  --exclude ._Info.plist \
  ./Source
