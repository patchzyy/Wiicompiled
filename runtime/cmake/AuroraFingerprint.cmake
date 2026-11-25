cmake_minimum_required(VERSION 3.20)

file(READ "${PROVENANCE_FILE}" _provenance)
string(JSON _recorded ERROR_VARIABLE _error GET "${_provenance}" AuroraSourceFingerprint)
if(_error OR NOT _recorded STREQUAL AURORA_FINGERPRINT)
    message(FATAL_ERROR "Native prebuilt package does not match the Aurora sources. Regenerate it with Prepare-NativePrebuilt.sh or omit --native-prebuilt-dir.")
endif()
