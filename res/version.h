// Single source of truth for the version number.
//
// The original program had three different ones at once (1166 in the registry,
// v1.4.6 in the UI, 1.1.5 in the resources). Everything here derives from the
// four numbers below: the executable's version resource, the About box and the
// registry values all read them.
#pragma once

#define TUNER_VERSION_MAJOR 2
#define TUNER_VERSION_MINOR 0
#define TUNER_VERSION_PATCH 2
#define TUNER_VERSION_BUILD 0

// The resource compiler cannot build strings out of macros, so the same numbers
// appear once more in the form it needs. Keep them in sync with the values
// above; nothing else in the project hardcodes a version.
#define TUNER_VERSION_COMMA4 2, 0, 2, 0
#define TUNER_VERSION_DOT3 "2.0.2"
#define TUNER_VERSION_DOT4 "2.0.2.0"

// Wide literal with the same text, for the title bar of the settings window.
#define TUNER_VERSION_DOT3W L"2.0.2"
