"""Counts sound starts in a game's MK9_TRACE_SOUND trace (frames rollback keeps only: from a fight's start on).

Goes through the trace in order. A frame has a first run and, after each restore that takes it back, a run again. A start in
a run again "repeats" when an earlier run of that frame started the same event as often already (same event, same place
among that event's starts in the frame) and that sound was neither stopped since nor its instance given to another start;
else it comes "only in the corrected frames". What FMOD really did is the how word on the line:
  first   a frame's first run: started
  fresh   run again, no note of it: started
  kept    run again, the sound still plays: not started again (the fix)
  silent  run again, the sound ended already: started muted (the fix)
  moved   run again, another instance plays it: started muted, the sound goes on where it is (the fix)
kept, silent and moved may come with " of <frame>": the note was a frame off, the sound started in that frame (a frame's
first run can find one too).
Without the fix (MK9_RB_SOUND_AGAIN=1) every start says fresh; and FMOD's start does nothing when the instance plays with its
channels active (0x10 in the state word on the line), so those are counted apart.

Printed: starts on first runs; starts in frames run again that were heard: duplicates (the same instance started over, or
another beside the first) and those only in the corrected frames; those asked of FMOD with no effect; those the fix
kept out; the sounds of frames taken back that no later run started again, stopped or left playing; the ends FMOD told in
frames taken back, and how many were told again.
python -I soundcount.py <trace> [--names]
"""
import collections
import re
import sys

path = sys.argv[1]
names = "--names" in sys.argv

pat = re.compile(r"start (\S+) event (\S+) (\S*) lr (\S+) = (\S+) (\S+)(?: state (\S+))?(?: of (\d+))?")
kinds = collections.Counter()
threads = collections.Counter()
told_back = collections.Counter()
told = []
notes = collections.defaultdict(dict)  # frame -> {(event, nth): handle}, what earlier runs of the frame started and still stands
this_run = collections.defaultdict(lambda: collections.Counter())  # frame -> starts of each event in its latest run
last_run = collections.defaultdict(set)  # frame -> (event, nth) its latest run started
again_frames = set()
stopped = told_again = told_not = left_to_component = 0
first = same_instance = other_instance = no_effect = corrected = kept = silent = moved = near = 0
name_of = {}
dup_names = collections.Counter()
new_names = collections.Counter()
gone_names = collections.Counter()
gone = 0
last_frame = 0


def count_gone():
    global gone
    for f in again_frames:
        for k in notes[f]:
            if k not in last_run[f]:
                gone += 1
                gone_names[name_of[k[0]]] += 1


for line in open(path, errors="replace"):
    p = line.split()
    if len(p) < 5 or not p[0].isdigit():
        continue
    frame, again, gen, thread, kind = int(p[0]), p[1] == "1", int(p[2]), p[3], p[4]
    # a rematch's frames count from 0 again
    if not again and frame + 100 < last_frame:
        count_gone()
        notes.clear()
        this_run.clear()
        last_run.clear()
        again_frames.clear()
        told = []
    if not again:
        last_frame = frame
    kinds[kind + (" again" if again else "") + (" other-thread" if thread != "m" else "")] += 1
    if kind == "restore":
        to = int(p[5])
        told_back["restores"] += 1
        for e in told:
            if e[0] >= to:
                told_back[f"type {e[2]}"] += 1
        told = [e for e in told if e[0] < to]
        for f in list(this_run):
            if f >= to:
                this_run[f] = collections.Counter()
                last_run[f] = set()
                again_frames.add(f)
    elif kind == "callback" and p[7] in ("0", "3", "4"):
        told = [e for e in told if e[0] > frame - 20]
        told.append((frame, p[5], p[7]))
    elif kind == "stopped":
        stopped += 1
        handle, event, of = p[5], p[7], int(p[10])
        for k in [k for k, h in notes[of].items() if k[0] == event and h == handle]:
            del notes[of][k]
    elif kind == "left":
        left_to_component += 1
    elif kind == "told-again":
        told_again += 1
    elif kind == "told-not-again":
        told_not += 1
    elif kind == "start":
        m = pat.match(" ".join(p[4:]))
        if not m:
            continue
        handle, event, name, lr, res, how, state, of = m.groups()
        state = int(state or "0", 16)
        if res != "0":
            kinds["start failed"] += 1
            continue
        threads[thread] += 1
        name_of[event] = name
        nth = this_run[frame][event]
        this_run[frame][event] += 1
        key = (event, nth)
        last_run[frame].add(key)
        if of is not None:
            near += 1
            of = int(of)
            for k in [k for k in notes[of] if k[0] == event]:
                del notes[of][k]
                break
        if how == "kept":
            kept += 1
        elif how == "silent":
            silent += 1
        elif how == "moved":
            moved += 1
        elif not again:
            first += 1
        elif key in notes[frame]:
            if notes[frame][key] == handle and state != 0x80000000 and state & 0x10:
                no_effect += 1
            else:
                dup_names[name] += 1
                if notes[frame][key] == handle:
                    same_instance += 1
                else:
                    other_instance += 1
        elif state != 0x80000000 and state & 0x10:
            no_effect += 1  # FMOD's start of an instance whose channels are active
        else:
            corrected += 1
            new_names[name] += 1
        # a start of an instance ends what it played before: no later frame's note of it stands
        if how == "fresh":
            for f in notes:
                if f > frame:
                    for k in [k for k, h in notes[f].items() if h == handle]:
                        del notes[f][k]
        notes[frame][key] = handle

count_gone()

print(path)
print(f"  starts on frames' first runs: {first}")
print(f"  in frames run again, heard: duplicates {same_instance + other_instance} ({same_instance} the same instance started "
      f"over, {other_instance} another beside the first), only in the corrected frames {corrected}")
print(f"  in frames run again, asked of FMOD with no effect: {no_effect} (the instance plays, its channels active)")
print(f"  not heard again: {kept} kept playing, {silent} ended already (muted), {moved} on another instance; {near} of them a frame off")
print(f"  sounds of frames taken back that no later run started: {stopped} stopped, {gone} left playing, {left_to_component} a component holds")
print(f"  ends told in frames taken back: {dict(told_back)}; told again {told_again}, no longer the component's {told_not}")
print(f"  threads: {dict(threads)}")
print(f"  kinds: {dict(sorted(kinds.items()))}")
if names:
    print("  duplicates:", dup_names.most_common(25))
    print("  only in corrected frames:", new_names.most_common(25))
    print("  taken back and left playing:", gone_names.most_common(25))
