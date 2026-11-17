// libMTSClient.h - stub for uP2noDX (MIT). dx7note.cc can ask the MTS-ESP microtuning service for
// note frequencies; the MPC has no such service, so there is never a "master" and nothing is called.
#ifndef UP2NODX_MTS_STUB_H
#define UP2NODX_MTS_STUB_H
struct MTSClient {};
inline bool MTS_HasMaster(MTSClient*) { return false; }
inline double MTS_NoteToFrequency(MTSClient*, char midinote, char) { return 440.0 * __builtin_pow(2.0, (midinote - 69) / 12.0); }
#endif
