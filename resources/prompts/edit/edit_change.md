# Change one node
Edit the current patch above. The user picked one node in Pulsar's node view and asked for a change.

Node: {node}
Instruction: {instruction}

- Change only what the instruction needs, at or next to that node. Keep every other line of code, the voiceMode, the name, the macro labels and defaults, and the graph as they are, unless the instruction asks otherwise.
- Keep every existing slot number. A new call takes a free slot of its pool; never renumber or reuse another call's slot.
- "Before this" / "after this" mean in the signal path into / out of that node.
- If the instruction names a macro (m1..m8), drive the change from it and give that macro a fitting label if it had none.
- Add graph edges for anything new.
- The hard rules still apply. Reply with the complete patch in the usual format.
