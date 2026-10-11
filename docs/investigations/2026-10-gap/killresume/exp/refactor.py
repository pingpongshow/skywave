import sys
root=sys.argv[1]
p=root+'/src/worker.cpp'; s=open(p).read()
# remove the earlier budget guard, restore plain budget
i=s.index('  // INGEST BEFORE DECODE.'); j=s.index('  for (auto it = pendingRx_.begin(); it != pendingRx_.end();) {', i)
s=s[:i]+'  int decodeBudget = 2;\n'+s[j:]
# split function
s=s.replace('void ModemWorker::pumpRxLocked(double now) {\n','''// INGEST BEFORE DECODE. A long-window decode attempt costs seconds of CPU
// (measured 2-7 s for 20-60 s windows on a loaded host) and the audio
// callback keeps writing into the RX ring meanwhile. Reading ONE chunk per
// pass and then decoding again let the ring fill and OVERRUN - the audio
// callback dropped samples silently and whatever frame was on air was
// destroyed (the ST3 kill/resume stall: ~100-195 s of audio lost per
// station). So every pass first drains the ring through the cheap path
// (downconvert, metering, detector) and only then spends the decode
// budget. Audio is never shed; under sustained decode overload the
// capture backlog cap sheds the OLDEST captures instead.
void ModemWorker::pumpRxLocked(double now) {
  if (!ingestChunkLocked(now)) return;
  for (int k = 0; k < 512 && rx_.readAvailable() >= kChunkSamples; ++k) {
    if (!ingestChunkLocked(now)) break;
  }
  decodePendingLocked(now);
}

bool ModemWorker::ingestChunkLocked(double now) {
''',1)
# within ingest: returns
seg_start=s.index('bool ModemWorker::ingestChunkLocked(double now) {')
split=s.index('  // Grow each pending capture along the retry ladder', seg_start)
seg=s[seg_start:split]
seg=seg.replace('  if (got == 0) return;','  if (got == 0) return false;',1)
seg=seg.replace('  if (bb.empty()) return;','  if (bb.empty()) return true;',1)
s=s[:seg_start]+seg+'''  return true;
}

void ModemWorker::decodePendingLocked(double now) {
'''+s[split:]
open(p,'w').write(s)
p=root+'/src/worker.hpp'; h=open(p).read()
h=h.replace('  void pumpRxLocked(double now);','''  void pumpRxLocked(double now);
  bool ingestChunkLocked(double now);   // one chunk: convert, meter, detect
  void decodePendingLocked(double now);  // capture ladder, bounded budget''',1)
open(p,'w').write(h)
